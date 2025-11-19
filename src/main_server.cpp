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

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

// 全局变量：用于信号处理中关闭服务器
TCPServer* g_server = nullptr;

static std::mutex       g_conn_mutex;
static std::atomic<int> g_online_clients = 0; // 原子变量，避免锁竞争

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
    std::cout << "收到退出信号，正在关闭..." << std::endl;
    exit(0);
}
// 核心函数：处理客户端上传
void handle_client_upload(int client_fd, const std::string& client_ip, uint16_t client_port)
{
    // 自动关闭client_fd（异常/正常退出都能释放）
    SocketGuard fd_guard(client_fd);

    // 连接计数器+1（线程安全）
    {
        std::lock_guard<std::mutex> lock(g_conn_mutex);
        g_online_clients++;
        std::cout << "Upload 新客户端上传请求, IP:" << client_ip << ":" << client_port << ", client_fd:" << client_fd
                  << ", 当前在线数: " << g_online_clients.load() << std::endl;
    }

    // 1. 设置连接超时（30秒，避免阻塞卡死）
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

    // 存储文件路径（用于异常时删除不完整文件）
    std::string save_path;

    try {
        // 2. 接收“上传请求”指令
        char req_buf[1024] = {0};
        int  recv_len      = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0); // 留1字节存'\0'
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

        // 3. 解包上传请求
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

        // 4. 创建保存目录和文件（去重后唯一逻辑）
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

        // 避免覆盖已存在文件（加时间戳后缀）
        save_path = save_dir + "/" + filename;
        if (std::filesystem::exists(save_path)) {
            save_path = save_dir + "/" + std::to_string(std::time(nullptr)) + "_" + filename;
            std::cout << "Upload 文件名已存在，重命名为：" << save_path << std::endl;
        }

        // 创建文件（二进制写入，覆盖已有文件）
        std::ofstream file(save_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 无法创建文件：" << save_path
                      << " 错误：" << strerror(errno) << std::endl;
            auto err_ack = TransferProtocol::pack_upload_ack(false, "服务端创建文件失败");
            send(client_fd, err_ack.data(), err_ack.size(), 0);
            return;
        }

        // 5. 接收文件数据（带进度提示）
        const int BUF_SIZE = 4096;
        char      buf[BUF_SIZE];
        uint64_t  recv_size = 0;
        std::cout << "Upload 开始接收客户端" << client_ip << ":" << client_port << " 的文件数据..." << std::endl;

        while (recv_size < file_size) {
            // 计算本次需要接收的字节数（避免缓冲区溢出）
            size_t need_recv = std::min(static_cast<size_t>(file_size - recv_size), static_cast<size_t>(BUF_SIZE));
            recv_len         = recv(client_fd, buf, need_recv, 0);

            if (recv_len <= 0) {
                // 接收失败/超时/客户端断开
                file.close();
                std::filesystem::remove(save_path); // 删除不完整文件
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

            // 写入文件（检查写入是否成功）
            file.write(buf, recv_len);
            if (!file.good()) {
                throw std::runtime_error("文件写入失败：" + save_path);
            }

            recv_size += recv_len;

            // 每接收10%打印一次进度（避免刷屏）
            static int last_progress = -1;
            int        progress      = static_cast<int>((static_cast<float>(recv_size) / file_size) * 100);
            if (progress % 10 == 0 && progress != last_progress) {
                std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 接收进度：" << progress << "%（"
                          << recv_size << "/" << file_size << "字节）" << std::endl;
                last_progress = progress;
            }
        }

        // 关闭文件（接收完成）
        file.close();
        std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 文件接收完成，保存路径：" << save_path
                  << std::endl;

        // 6. 接收客户端发送的MD5校验值
        char finish_buf[1024] = {0};
        recv_len              = recv(client_fd, finish_buf, sizeof(finish_buf) - 1, 0);
        if (recv_len <= 0) {
            // 未收到MD5，删除文件
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

        // 7. 解析客户端MD5
        std::vector<char> finish_vec(finish_buf, finish_buf + recv_len);
        std::string       client_md5;
        if (!TransferProtocol::unpack_upload_finish(finish_vec, client_md5)) {
            std::filesystem::remove(save_path);
            std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 解析MD5失败" << std::endl;
            auto err_ack = TransferProtocol::pack_upload_ack(false, "解析MD5校验值失败");
            send(client_fd, err_ack.data(), err_ack.size(), 0);
            return;
        }

        // 8. 计算服务端文件MD5并校验
        std::string server_md5 = MD5::compute_file(save_path);
        bool        md5_match  = (client_md5 == server_md5);
        std::cout << "Upload 客户端" << client_ip << ":" << client_port << " MD5校验 - 客户端：" << client_md5
                  << "，服务端：" << server_md5 << "，结果：" << (md5_match ? "通过" : "失败") << std::endl;

        // 9. 向客户端发送最终响应
        std::vector<char> ack_buf = TransferProtocol::pack_upload_ack(
            md5_match, md5_match ? "上传成功，MD5校验通过" : "MD5校验失败，文件可能损坏");
        int send_len = send(client_fd, ack_buf.data(), ack_buf.size(), 0);
        if (send_len <= 0) {
            std::cout << "Upload 向客户端" << client_ip << ":" << client_port << " 发送响应失败" << std::endl;
        } else {
            std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 上传处理完成" << std::endl;
        }

    } catch (const std::exception& e) {
        // 捕获所有已知异常，清理资源
        std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 处理上传异常：" << e.what() << std::endl;
        // 删除不完整文件
        if (!save_path.empty() && std::filesystem::exists(save_path)) {
            std::filesystem::remove(save_path);
        }
        auto err_ack = TransferProtocol::pack_upload_ack(false, "处理上传异常");
        send(client_fd, err_ack.data(), err_ack.size(), 0);
    } catch (...) {
        // 捕获未知异常
        std::cout << "Upload 客户端" << client_ip << ":" << client_port << " 处理上传未知异常" << std::endl;
        // 删除不完整文件
        if (!save_path.empty() && std::filesystem::exists(save_path)) {
            std::filesystem::remove(save_path);
        }
        auto err_ack = TransferProtocol::pack_upload_ack(false, "未知异常");
        send(client_fd, err_ack.data(), err_ack.size(), 0);
    }

    // 连接计数器-1（线程安全）
    {
        std::lock_guard<std::mutex> lock(g_conn_mutex);
        g_online_clients--;
        std::cout << "Upload 客户端" << client_ip << ":" << client_port
                  << " 连接处理完毕，当前在线数: " << g_online_clients.load() << std::endl;
    }
}

int main(int argc, char* argv[])
{
    // 注册信号处理（支持Ctrl+C优雅退出）
    signal(SIGINT, signal_handler);

    // 初始化服务器（默认端口8888，后续可通过命令行参数修改）
    uint16_t  port = 8888;
    TCPServer server(port);
    g_server = &server;

    // 启动服务器
    if (!server.start()) {
        std::cerr << "服务器启动失败" << std::endl;
        return 1;
    }

    // 创建线程池（5个工作线程）
    ThreadPool thread_pool(5);
    std::cout << "线程池初始化完成，工作线程数量：5" << std::endl;

    // 循环接受客户端连接（暂时单线程，每次处理一个）
    while (true) {
        std::string client_ip;
        uint16_t    client_port;
        std::cout << "等待客户端连接..." << std::endl;

        int client_fd = server.accept_client(client_ip, client_port);
        if (client_fd == -1) {
            continue; // 接受失败，继续等待
        }

        // [client_fd, client_ip, client_port]() { handle_client_upload(client_fd, client_ip, client_port);}
        // 将客户端处理逻辑作为任务提交给线程池
        // 注意：用lambda捕获client_fd、client_ip、client_port，确保线程安全
        thread_pool.submit(
            [client_fd, client_ip, client_port]() { handle_client_upload(client_fd, client_ip, client_port); });
    }

    return 0;
}