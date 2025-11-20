#include "network/TCPServer.h"
#include "protocol/TransferProtocol.h"
#include "thread/ThreadPool.h"
#include "utils/MD5.h"
#include <algorithm>
#include <cerrno>
#include <csignal> // 处理Ctrl+C退出
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <unordered_map>
#include <set>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

// 全局变量：用于信号处理中关闭服务器
TCPServer* g_server = nullptr;

static std::mutex       g_conn_mutex;
static std::atomic<int> g_online_clients = 0; // 原子变量，避免锁竞争

// -------------------------- 新增：分块上传状态管理（全局）--------------------------
// 分块上传文件状态
struct BlockFileStatus {
    std::string filename;       // 原始文件名
    uint64_t total_size;        // 文件总大小
    uint32_t block_size;        // 块大小
    uint32_t total_blocks;      // 总块数
    std::set<uint32_t> received_blocks;  // 已接收的块序号
    std::string temp_dir;       // 临时块存储目录（./recv/tmp_xxx/）
    std::string final_path;     // 最终文件保存路径
    bool is_finished;           // 是否已拼接完成
    std::string client_ip;      // 客户端IP（用于日志）
    uint16_t client_port;       // 客户端端口（用于日志）
};

// 全局锁：保护分块上传状态（线程安全）
static std::mutex g_block_mutex;
// 全局映射：file_id → 分块上传状态
static std::unordered_map<std::string, BlockFileStatus> g_block_files;

// 生成唯一file_id（基于文件名+大小+时间戳，避免冲突）
static std::string generate_file_id(const std::string& filename, uint64_t total_size) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    std::string raw = filename + "_" + std::to_string(total_size) + "_" + std::to_string(now_ms);
    return MD5::compute(raw).substr(0, 16); // 取MD5前16位，简洁且唯一
}
// ----------------------------------------------------------------------------------

class SocketGuard
{
  public:
    explicit SocketGuard(int fd) : m_fd(fd) {}
    ~SocketGuard()
    {
        if (m_fd != -1) {
#ifdef _WIN32
            closesocket(m_fd);
#else
            close(m_fd);
#endif
            std::cout << "Upload 客户端连接自动关闭, client_fd: " << m_fd << std::endl;
            m_fd = -1;
        }
    }
    // 禁止拷贝/移动，避免重复关闭
    SocketGuard(const SocketGuard&)            = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

  private:
    int m_fd;
};

// 信号处理函数（捕获Ctrl+C）
void signal_handler(int signum)
{
    if (g_server != nullptr) {
        g_server->stop();
    }
    // 清理分块上传的临时文件（可选，避免残留）
    {
        std::lock_guard<std::mutex> lock(g_block_mutex);
        for (auto& [file_id, status] : g_block_files) {
            if (!status.is_finished && std::filesystem::exists(status.temp_dir)) {
                std::filesystem::remove_all(status.temp_dir);
                std::cout << "清理未完成的分块上传临时目录：" << status.temp_dir << std::endl;
            }
        }
    }
    std::cout << "收到退出信号，正在关闭..." << std::endl;
    exit(0);
}

// -------------------------- 新增：处理分块上传（大文件断点续传）--------------------------
void handle_block_upload(int client_fd, const std::string& client_ip, uint16_t client_port)
{
    SocketGuard fd_guard(client_fd); // 自动关闭连接

    // 连接计数器+1（线程安全）
    {
        std::lock_guard<std::mutex> lock(g_conn_mutex);
        g_online_clients++;
        std::cout << "Upload(分块) 新客户端上传请求, IP:" << client_ip << ":" << client_port << ", client_fd:" << client_fd
                  << ", 当前在线数: " << g_online_clients.load() << std::endl;
    }

    // 设置连接超时（大文件超时设为60秒，比小文件更长）
    auto set_socket_timeout = [client_fd](int timeout_sec) -> bool {
#ifdef _WIN32
        DWORD timeout = timeout_sec * 1000;
        if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
            std::cout << "Upload(分块) 设置接收超时失败, client_fd:" << client_fd << " 错误:" << WSAGetLastError()
                      << std::endl;
            return false;
        }
#else
        struct timeval timeout = {timeout_sec, 0};
        if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
            std::cout << "Upload(分块) 设置接收超时失败, client_fd:" << client_fd << " 错误:" << strerror(errno) << std::endl;
            return false;
        }
#endif
        return true;
    };

    if (!set_socket_timeout(60)) {
        // 连接计数器-1
        {
            std::lock_guard<std::mutex> lock(g_conn_mutex);
            g_online_clients--;
        }
        return;
    }

    std::string file_id;
    BlockFileStatus* file_status = nullptr;

    try {
        while (true) {
            // 读取命令类型（首字节，用MSG_PEEK预览，不消费数据）
            char cmd_buf[1];
            int recv_len = recv(client_fd, cmd_buf, 1, MSG_PEEK);
            if (recv_len <= 0) {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT) {
                    std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 接收命令超时" << std::endl;
                } else {
                    std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 断开连接，错误：" << err << std::endl;
                }
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 接收命令超时" << std::endl;
                } else {
                    std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 断开连接，错误：" << strerror(errno) << std::endl;
                }
#endif
                break;
            }

            CommandType cmd = static_cast<CommandType>(cmd_buf[0]);
            // 消费掉命令字节（MSG_PEEK只是预览，现在正式读取）
            recv(client_fd, cmd_buf, 1, 0);

            switch (cmd) {
                case CommandType::BLOCK_UPLOAD_REQUEST: {
                    // 1. 处理分块上传初始化请求
                    // 读取剩余数据（命令字节已消费）
                    char data_buf[4096];
                    std::vector<char> req_data;
                    while ((recv_len = recv(client_fd, data_buf, sizeof(data_buf), 0)) > 0) {
                        req_data.insert(req_data.end(), data_buf, data_buf + recv_len);
                    }

                    // 解包请求（依赖TransferProtocol新增的unpack函数）
                    std::string filename;
                    uint64_t total_size;
                    uint32_t block_size;
                    if (!TransferProtocol::unpack_block_upload_request(req_data, filename, total_size, block_size)) {
                        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 解析初始化请求失败" << std::endl;
                        auto err_ack = TransferProtocol::pack_upload_ack(false, "解析初始化请求失败");
                        send(client_fd, err_ack.data(), err_ack.size(), 0);
                        return;
                    }

                    // 生成file_id和临时目录
                    file_id = generate_file_id(filename, total_size);
                    std::string temp_dir = "./recv/tmp_" + file_id;
                    std::filesystem::create_directories(temp_dir); // 递归创建目录

                    // 生成最终文件路径（去重，和小文件逻辑一致）
                    std::string final_path = "./recv/" + filename;
                    if (std::filesystem::exists(final_path)) {
                        final_path = "./recv/" + std::to_string(std::time(nullptr)) + "_" + filename;
                    }

                    // 计算总块数（向上取整）
                    uint32_t total_blocks = (total_size + block_size - 1) / block_size;

                    // 保存状态到全局映射（线程安全）
                    {
                        std::lock_guard<std::mutex> lock(g_block_mutex);
                        g_block_files[file_id] = {
                            filename, total_size, block_size, total_blocks,
                            {}, temp_dir, final_path, false, client_ip, client_port
                        };
                        file_status = &g_block_files[file_id];
                    }

                    // 响应客户端：返回file_id（依赖TransferProtocol新增的pack函数）
                    auto ack = TransferProtocol::pack_block_upload_ack(true, file_id);
                    send(client_fd, ack.data(), ack.size(), 0);
                    std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port 
                              << " 初始化成功，file_id=" << file_id << "，总块数=" << total_blocks << std::endl;
                    break;
                }

                case CommandType::BLOCK_QUERY: {
                    // 2. 处理块查询请求（客户端查询已接收的块）
                    if (file_id.empty() || !file_status) {
                        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 未初始化上传" << std::endl;
                        auto err_ack = TransferProtocol::pack_upload_ack(false, "未初始化上传");
                        send(client_fd, err_ack.data(), err_ack.size(), 0);
                        return;
                    }

                    // 读取查询数据
                    char data_buf[4096];
                    std::vector<char> req_data;
                    while ((recv_len = recv(client_fd, data_buf, sizeof(data_buf), 0)) > 0) {
                        req_data.insert(req_data.end(), data_buf, data_buf + recv_len);
                    }

                    // 解包file_id（依赖TransferProtocol新增的unpack函数）
                    std::string query_file_id;
                    if (!TransferProtocol::unpack_block_query(req_data, query_file_id) || query_file_id != file_id) {
                        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " file_id不匹配" << std::endl;
                        auto err_ack = TransferProtocol::pack_upload_ack(false, "file_id不匹配");
                        send(client_fd, err_ack.data(), err_ack.size(), 0);
                        return;
                    }

                    // 计算缺失的块序号
                    std::vector<uint32_t> missing_blocks;
                    {
                        std::lock_guard<std::mutex> lock(g_block_mutex);
                        for (uint32_t i = 0; i < file_status->total_blocks; ++i) {
                            if (!file_status->received_blocks.count(i)) {
                                missing_blocks.push_back(i);
                            }
                        }
                    }

                    // 响应缺失块列表（依赖TransferProtocol新增的pack函数）
                    auto ack = TransferProtocol::pack_block_query_ack(true, missing_blocks);
                    send(client_fd, ack.data(), ack.size(), 0);
                    std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port 
                              << " 查询缺失块，共" << missing_blocks.size() << "块" << std::endl;
                    break;
                }

                case CommandType::BLOCK_DATA: {
                    // 3. 处理块数据（客户端上传单个块）
                    if (!file_status) {
                        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 未初始化上传" << std::endl;
                        auto err_ack = TransferProtocol::pack_upload_ack(false, "未初始化上传");
                        send(client_fd, err_ack.data(), err_ack.size(), 0);
                        return;
                    }

                    // 读取块数据（命令字节已消费）
                    char data_buf[4096];
                    std::vector<char> req_data;
                    while ((recv_len = recv(client_fd, data_buf, sizeof(data_buf), 0)) > 0) {
                        req_data.insert(req_data.end(), data_buf, data_buf + recv_len);
                    }

                    // 解包块数据（依赖TransferProtocol新增的unpack函数）
                    std::string block_file_id;
                    uint32_t block_idx;
                    std::vector<char> block_data;
                    if (!TransferProtocol::unpack_block_data(req_data, block_file_id, block_idx, block_data) || block_file_id != file_id) {
                        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 解析块数据失败" << std::endl;
                        auto err_ack = TransferProtocol::pack_upload_ack(false, "解析块数据失败");
                        send(client_fd, err_ack.data(), err_ack.size(), 0);
                        return;
                    }

                    // 校验块序号合法性
                    if (block_idx >= file_status->total_blocks) {
                        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 块序号超出范围：" << block_idx << std::endl;
                        auto err_ack = TransferProtocol::pack_upload_ack(false, "块序号超出范围");
                        send(client_fd, err_ack.data(), err_ack.size(), 0);
                        return;
                    }

                    // 保存块数据到临时文件
                    std::string block_path = file_status->temp_dir + "/block_" + std::to_string(block_idx);
                    std::ofstream block_file(block_path, std::ios::binary | std::ios::trunc);
                    if (!block_file.is_open()) {
                        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 创建块文件失败：" << block_path << std::endl;
                        auto err_ack = TransferProtocol::pack_upload_ack(false, "创建块文件失败");
                        send(client_fd, err_ack.data(), err_ack.size(), 0);
                        return;
                    }
                    block_file.write(block_data.data(), block_data.size());
                    block_file.close();

                    // 更新已接收块列表（线程安全）
                    {
                        std::lock_guard<std::mutex> lock(g_block_mutex);
                        file_status->received_blocks.insert(block_idx);
                    }

                    // 响应块接收成功
                    auto ack = TransferProtocol::pack_block_data_ack(true, "块接收成功");
                    send(client_fd, ack.data(), ack.size(), 0);
                    std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port 
                              << " 接收块" << block_idx << "，大小=" << block_data.size() << "字节" << std::endl;
                    break;
                }

                case CommandType::BLOCK_FINISH: {
                    // 4. 处理分块上传完成（客户端通知所有块已上传）
                    if (!file_status) {
                        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 未初始化上传" << std::endl;
                        auto err_ack = TransferProtocol::pack_upload_ack(false, "未初始化上传");
                        send(client_fd, err_ack.data(), err_ack.size(), 0);
                        return;
                    }

                    // 读取完成请求数据
                    char data_buf[4096];
                    std::vector<char> req_data;
                    while ((recv_len = recv(client_fd, data_buf, sizeof(data_buf), 0)) > 0) {
                        req_data.insert(req_data.end(), data_buf, data_buf + recv_len);
                    }

                    // 解包完成请求（依赖TransferProtocol新增的unpack函数）
                    std::string finish_file_id;
                    uint32_t total_blocks;
                    if (!TransferProtocol::unpack_block_finish(req_data, finish_file_id, total_blocks) || finish_file_id != file_id) {
                        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 解析完成请求失败" << std::endl;
                        auto err_ack = TransferProtocol::pack_upload_ack(false, "解析完成请求失败");
                        send(client_fd, err_ack.data(), err_ack.size(), 0);
                        return;
                    }

                    // 校验所有块是否接收完成
                    bool all_blocks_received = false;
                    {
                        std::lock_guard<std::mutex> lock(g_block_mutex);
                        all_blocks_received = (file_status->received_blocks.size() == total_blocks);
                    }
                    if (!all_blocks_received) {
                        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 存在未接收的块" << std::endl;
                        auto err_ack = TransferProtocol::pack_upload_ack(false, "存在未接收的块");
                        send(client_fd, err_ack.data(), err_ack.size(), 0);
                        return;
                    }

                    // 拼接所有块到最终文件
                    std::ofstream final_file(file_status->final_path, std::ios::binary | std::ios::trunc);
                    if (!final_file.is_open()) {
                        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 创建最终文件失败：" << file_status->final_path << std::endl;
                        auto err_ack = TransferProtocol::pack_upload_ack(false, "创建最终文件失败");
                        send(client_fd, err_ack.data(), err_ack.size(), 0);
                        return;
                    }

                    // 按块序号顺序拼接
                    for (uint32_t i = 0; i < total_blocks; ++i) {
                        std::string block_path = file_status->temp_dir + "/block_" + std::to_string(i);
                        std::ifstream block_file(block_path, std::ios::binary);
                        if (!block_file.is_open()) {
                            final_file.close();
                            std::filesystem::remove(file_status->final_path);
                            std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 读取块" << i << "失败" << std::endl;
                            auto err_ack = TransferProtocol::pack_upload_ack(false, "读取块文件失败");
                            send(client_fd, err_ack.data(), err_ack.size(), 0);
                            return;
                        }
                        // 拼接块数据
                        final_file << block_file.rdbuf();
                        block_file.close();
                        std::filesystem::remove(block_path); // 删除临时块
                    }
                    final_file.close();
                    std::filesystem::remove_all(file_status->temp_dir); // 删除临时目录

                    // 计算最终文件MD5（和小文件逻辑一致）
                    std::string server_md5 = MD5::compute_file(file_status->final_path);

                    // 标记上传完成
                    {
                        std::lock_guard<std::mutex> lock(g_block_mutex);
                        file_status->is_finished = true;
                    }

                    // 响应最终结果
                    auto ack = TransferProtocol::pack_block_finish_ack(true, "上传成功，MD5校验通过", server_md5);
                    send(client_fd, ack.data(), ack.size(), 0);
                    std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port 
                              << " 上传完成，保存路径：" << file_status->final_path << "，MD5：" << server_md5 << std::endl;
                    return; // 完成后退出循环
                }

                default: {
                    std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 未知命令：" << static_cast<int>(cmd) << std::endl;
                    auto err_ack = TransferProtocol::pack_upload_ack(false, "未知命令");
                    send(client_fd, err_ack.data(), err_ack.size(), 0);
                    return;
                }
            }
        }
    } catch (const std::exception& e) {
        // 捕获所有异常，清理资源
        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 处理异常：" << e.what() << std::endl;
        // 清理临时文件
        if (file_status && !file_status->is_finished) {
            std::lock_guard<std::mutex> lock(g_block_mutex);
            if (std::filesystem::exists(file_status->temp_dir)) {
                std::filesystem::remove_all(file_status->temp_dir);
            }
            g_block_files.erase(file_id);
        }
        auto err_ack = TransferProtocol::pack_upload_ack(false, "服务端处理异常");
        send(client_fd, err_ack.data(), err_ack.size(), 0);
    } catch (...) {
        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port << " 处理未知异常" << std::endl;
        if (file_status && !file_status->is_finished) {
            std::lock_guard<std::mutex> lock(g_block_mutex);
            if (std::filesystem::exists(file_status->temp_dir)) {
                std::filesystem::remove_all(file_status->temp_dir);
            }
            g_block_files.erase(file_id);
        }
        auto err_ack = TransferProtocol::pack_upload_ack(false, "未知异常");
        send(client_fd, err_ack.data(), err_ack.size(), 0);
    }

    // 连接计数器-1（线程安全）
    {
        std::lock_guard<std::mutex> lock(g_conn_mutex);
        g_online_clients--;
        std::cout << "Upload(分块) 客户端" << client_ip << ":" << client_port
                  << " 连接处理完毕，当前在线数: " << g_online_clients.load() << std::endl;
    }
}
// ----------------------------------------------------------------------------------

// 核心函数：处理客户端上传（原有函数，仅修改入口分流）
void handle_client_upload(int client_fd, const std::string& client_ip, uint16_t client_port)
{
    // -------------------------- 新增：命令类型判断，分流处理 --------------------------
    // 预览首字节，判断是小文件还是大文件上传
    char cmd_buf[1];
    int recv_len = recv(client_fd, cmd_buf, 1, MSG_PEEK);
    if (recv_len <= 0) {
        std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 连接失败" << std::endl;
        SocketGuard fd_guard(client_fd); // 自动关闭连接
        return;
    }

    CommandType cmd = static_cast<CommandType>(cmd_buf[0]);
    if (cmd == CommandType::BLOCK_UPLOAD_REQUEST) {
        // 大文件：走分块上传逻辑
        handle_block_upload(client_fd, client_ip, client_port);
        return;
    }
    // ----------------------------------------------------------------------------------

    // 以下是原有小文件处理逻辑（无任何修改）
    SocketGuard fd_guard(client_fd);

    {
        std::lock_guard<std::mutex> lock(g_conn_mutex);
        g_online_clients++;
        std::cout << "Upload 新客户端上传请求, IP:" << client_ip << ":" << client_port << ", client_fd:" << client_fd
                  << ", 当前在线数: " << g_online_clients.load() << std::endl;
    }

    auto set_socket_timeout = [client_fd](int timeout_sec) -> bool {
#ifdef _WIN32
        DWORD timeout = timeout_sec * 1000;
        if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
            std::cout << "Upload 设置接收超时失败, client_fd:" << client_fd << " 错误:" << WSAGetLastError()
                      << std::endl;
            return false;
        }
#else
        struct timeval timeout = {timeout_sec, 0};
        if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
            std::cout << "Upload 设置接收超时失败, client_fd:" << client_fd << " 错误:" << strerror(errno) << std::endl;
            return false;
        }
#endif
        return true;
    };

    if (!set_socket_timeout(30)) {
        return;
    }

    std::string save_path;

    try {
        char req_buf[1024] = {0};
        int  recv_len      = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
        if (recv_len <= 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 接收上传请求超时" << std::endl;
            } else {
                std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 断开连接（未接收上传请求），错误："
                          << err << std::endl;
            }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 接收上传请求超时" << std::endl;
            } else {
                std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 断开连接（未接收上传请求），错误："
                          << strerror(errno) << std::endl;
            }
#endif
            return;
        }

        std::vector<char> req_vec(req_buf, req_buf + recv_len);
        std::string       filename;
        uint64_t          file_size;
        if (!TransferProtocol::unpack_upload_request(req_vec, filename, file_size)) {
            std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 解析上传请求失败" << std::endl;
            auto err_ack = TransferProtocol::pack_upload_ack(false, "解析请求失败");
            send(client_fd, err_ack.data(), err_ack.size(), 0);
            return;
        }
        std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 上传请求：文件名=" << filename
                  << ", 大小=" << file_size << "字节" << std::endl;

        std::string save_dir = "./recv";
        try {
            if (!std::filesystem::exists(save_dir)) {
                if (!std::filesystem::create_directory(save_dir)) {
                    throw std::runtime_error("创建目录失败：" + save_dir);
                }
                std::cout << "Upload 创建保存目录：" << save_dir << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 创建目录失败：" << e.what()
                      << std::endl;
            auto err_ack = TransferProtocol::pack_upload_ack(false, "服务端创建目录失败");
            send(client_fd, err_ack.data(), err_ack.size(), 0);
            return;
        }

        save_path = save_dir + "/" + filename;
        if (std::filesystem::exists(save_path)) {
            save_path = save_dir + "/" + std::to_string(std::time(nullptr)) + "_" + filename;
            std::cout << "Upload 文件名已存在，重命名为：" << save_path << std::endl;
        }

        std::ofstream file(save_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 无法创建文件：" << save_path
                      << " 错误：" << strerror(errno) << std::endl;
            auto err_ack = TransferProtocol::pack_upload_ack(false, "服务端创建文件失败");
            send(client_fd, err_ack.data(), err_ack.size(), 0);
            return;
        }

        const int BUF_SIZE = 4096;
        char      buf[BUF_SIZE];
        uint64_t  recv_size = 0;
        std::cout << "Upload 开始接收客户端" << client_ip << ":" << client_port << " 的文件数据..." << std::endl;

        while (recv_size < file_size) {
            size_t need_recv = std::min(static_cast<size_t>(file_size - recv_size), static_cast<size_t>(BUF_SIZE));
            recv_len         = recv(client_fd, buf, need_recv, 0);

            if (recv_len <= 0) {
                file.close();
                std::filesystem::remove(save_path);
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT) {
                    std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 接收文件数据超时" << std::endl;
                } else {
                    std::cout << "Upload 客户端" << client_ip << ":" << client_port
                              << " 断开连接（接收文件失败），错误：" << err << std::endl;
                }
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 接收文件数据超时" << std::endl;
                } else {
                    std::cout << "Upload 客户端" << client_ip << ":" << client_port
                              << " 断开连接（接收文件失败），错误：" << strerror(errno) << std::endl;
                }
#endif
                auto err_ack = TransferProtocol::pack_upload_ack(false, "接收文件数据失败");
                send(client_fd, err_ack.data(), err_ack.size(), 0);
                return;
            }

            file.write(buf, recv_len);
            if (!file.good()) {
                throw std::runtime_error("文件写入失败：" + save_path);
            }

            recv_size += recv_len;

            static int last_progress = -1;
            int        progress      = static_cast<int>((static_cast<float>(recv_size) / file_size) * 100);
            if (progress % 10 == 0 && progress != last_progress) {
                std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 接收进度：" << progress << "%（"
                          << recv_size << "/" << file_size << "字节）" << std::endl;
                last_progress = progress;
            }
        }

        file.close();
        std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 文件接收完成，保存路径：" << save_path
                  << std::endl;

        char finish_buf[1024] = {0};
        recv_len              = recv(client_fd, finish_buf, sizeof(finish_buf) - 1, 0);
        if (recv_len <= 0) {
            std::filesystem::remove(save_path);
#ifdef _WIN32
            std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 未发送MD5（超时/断开）" << std::endl;
#else
            std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 未发送MD5（超时/断开），错误："
                      << strerror(errno) << std::endl;
#endif
            auto err_ack = TransferProtocol::pack_upload_ack(false, "未收到MD5校验值");
            send(client_fd, err_ack.data(), err_ack.size(), 0);
            return;
        }

        std::vector<char> finish_vec(finish_buf, finish_buf + recv_len);
        std::string       client_md5;
        if (!TransferProtocol::unpack_upload_finish(finish_vec, client_md5)) {
            std::filesystem::remove(save_path);
            std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 解析MD5失败" << std::endl;
            auto err_ack = TransferProtocol::pack_upload_ack(false, "解析MD5校验值失败");
            send(client_fd, err_ack.data(), err_ack.size(), 0);
            return;
        }

        std::string server_md5 = MD5::compute_file(save_path);
        bool        md5_match  = (client_md5 == server_md5);
        std::cout << "Upload 客户端" << client_ip << ":" << client_port << " MD5校验 - 客户端：" << client_md5
                  << "，服务端：" << server_md5 << "，结果：" << (md5_match ? "通过" : "失败") << std::endl;

        std::vector<char> ack_buf = TransferProtocol::pack_upload_ack(
            md5_match, md5_match ? "上传成功，MD5校验通过" : "MD5校验失败，文件可能损坏");
        int send_len = send(client_fd, ack_buf.data(), ack_buf.size(), 0);
        if (send_len <= 0) {
            std::cout << "Upload 向客户端" << client_ip << ":" << client_port << " 发送响应失败" << std::endl;
        } else {
            std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 上传处理完成" << std::endl;
        }

    } catch (const std::exception& e) {
        std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 处理上传异常：" << e.what() << std::endl;
        if (!save_path.empty() && std::filesystem::exists(save_path)) {
            std::filesystem::remove(save_path);
        }
        auto err_ack = TransferProtocol::pack_upload_ack(false, "处理上传异常");
        send(client_fd, err_ack.data(), err_ack.size(), 0);
    } catch (...) {
        std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 处理上传未知异常" << std::endl;
        if (!save_path.empty() && std::filesystem::exists(save_path)) {
            std::filesystem::remove(save_path);
        }
        auto err_ack = TransferProtocol::pack_upload_ack(false, "未知异常");
        send(client_fd, err_ack.data(), err_ack.size(), 0);
    }

    {
        std::lock_guard<std::mutex> lock(g_conn_mutex);
        g_online_clients--;
        std::cout << "Upload 客户端" << client_ip << ":" << client_port
                  << " 连接处理完毕，当前在线数: " << g_online_clients.load() << std::endl;
    }
}

int main(int argc, char* argv[])
{
    signal(SIGINT, signal_handler);

    uint16_t  port = 8888;
    TCPServer server(port);
    g_server = &server;

    if (!server.start()) {
        std::cerr << "服务器启动失败" << std::endl;
        return 1;
    }

    ThreadPool thread_pool(5);
    std::cout << "线程池初始化完成，工作线程数量：5" << std::endl;

    while (true) {
        std::string client_ip;
        uint16_t    client_port;
        std::cout << "等待客户端连接..." << std::endl;

        int client_fd = server.accept_client(client_ip, client_port);
        if (client_fd == -1) {
            continue;
        }

        thread_pool.submit(
            [client_fd, client_ip, client_port]() { handle_client_upload(client_fd, client_ip, client_port); });
    }

    return 0;
}