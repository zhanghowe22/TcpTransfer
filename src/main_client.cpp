#include "network/TCPClient.h"
#include "protocol/TransferProtocol.h"
#include "utils/MD5.h"
#include <iostream>
#include <csignal>
#include <fstream>
#include <filesystem>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#else
#include <unistd.h>
#include <arpa/inet.h>
#endif

// 全局客户端对象（用于信号处理）
TCPClient* g_client = nullptr;

// 信号处理（捕获Ctrl+C退出）
void signal_handler(int signum)
{
    if (g_client != nullptr && g_client->is_connected()) {
        g_client->disconnect();
    }
    std::cout << "客户端退出" << std::endl;
    exit(0);
}

// 获取文件大小
uint64_t get_file_size(const std::string& filename) {
    try {
        return std::filesystem::file_size(filename);
    } catch (...) {
        return 0;
    }
}

// 辅助函数：完整接收数据（跨平台）
bool recv_all(int sockfd, std::vector<char>& data) {
    data.clear();
    char buf[4096];
    while (true) {
        int recv_len = recv(sockfd, buf, sizeof(buf), 0);
        if (recv_len <= 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            std::cerr << "接收数据失败，错误码：" << err << std::endl;
#else
            std::cerr << "接收数据失败，错误：" << strerror(errno) << std::endl;
#endif
            return false;
        }
        data.insert(data.end(), buf, buf + recv_len);
        // 简单判断：若已收到UPLOAD_ACK且长度足够，停止接收
        if (data.size() >= 1 && static_cast<CommandType>(data[0]) == CommandType::UPLOAD_ACK) {
            break;
        }
    }
    return true;
}

// 小文件上传（原有逻辑，保持不变）
bool upload_small_file(TCPClient& client, const std::string& local_filename) {
    // 1. 检查文件是否存在
    std::ifstream file(local_filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "无法打开文件：" << local_filename << std::endl;
        return false;
    }

    // 2. 获取文件名和大小
    std::string filename = std::filesystem::path(local_filename).filename().string();
    uint64_t file_size = get_file_size(local_filename);
    if (file_size == 0) {
        std::cerr << "文件为空或获取大小失败：" << local_filename << std::endl;
        file.close();
        return false;
    }

    // 3. 计算文件MD5
    std::string file_md5 = MD5::compute_file(local_filename);
    if (file_md5.empty()) {
        std::cerr << "计算MD5失败：" << local_filename << std::endl;
        file.close();
        return false;
    }
    std::cout << "文件信息：" << filename << "，大小：" << file_size << "字节，MD5：" << file_md5 << std::endl;

    // 4. 发送“上传请求”
    std::vector<char> req_buf = TransferProtocol::pack_upload_request(filename, file_size);
    int send_len = send(client.get_client_fd(), req_buf.data(), req_buf.size(), 0);
    if (send_len != static_cast<int>(req_buf.size())) {
        std::cerr << "发送上传请求失败" << std::endl;
        file.close();
        return false;
    }
    std::cout << "已发送上传请求，等待接收..." << std::endl;

    // 5. 发送文件数据
    const int BUF_SIZE = 4096;
    char buf[BUF_SIZE];
    uint64_t sent_size = 0;
    std::cout << "开始上传文件..." << std::endl;
    while (!file.eof()) {
        file.read(buf, BUF_SIZE);
        size_t read_len = file.gcount();
        if (read_len == 0) break;

        send_len = send(client.get_client_fd(), buf, read_len, 0);
        if (send_len <= 0) {
            std::cerr << "发送数据失败" << std::endl;
            file.close();
            return false;
        }

        sent_size += send_len;
        float progress = (static_cast<float>(sent_size) / file_size) * 100;
        std::cout << "\r上传进度：" << progress << "% (" << sent_size << "/" << file_size << "字节)";
        std::cout.flush();
    }
    std::cout << std::endl << "文件数据发送完成" << std::endl;

    // 6. 发送“上传完成”指令（带MD5）
    std::vector<char> finish_buf = TransferProtocol::pack_upload_finish(file_md5);
    send_len = send(client.get_client_fd(), finish_buf.data(), finish_buf.size(), 0);
    if (send_len != static_cast<int>(finish_buf.size())) {
        std::cerr << "发送上传完成指令失败" << std::endl;
        file.close();
        return false;
    }

    // 7. 接收并解析最终响应
    std::vector<char> ack_vec;
    if (!recv_all(client.get_client_fd(), ack_vec)) {
        std::cerr << "未收到服务器响应" << std::endl;
        file.close();
        return false;
    }

    if (ack_vec.size() >= 2 && static_cast<CommandType>(ack_vec[0]) == CommandType::UPLOAD_ACK) {
        bool success = (ack_vec[1] == 0x00);
        std::string msg = "未知信息";
        if (ack_vec.size() >= 6) { // 2字节头 + 4字节长度
            uint32_t msg_len = ntohl(*reinterpret_cast<const uint32_t*>(ack_vec.data() + 2));
            if (ack_vec.size() >= 6 + msg_len) {
                msg = std::string(ack_vec.begin() + 6, ack_vec.begin() + 6 + msg_len);
            }
        }
        if (success) {
            std::cout << "=== 上传成功！===" << std::endl << "服务器消息：" << msg << std::endl;
        } else {
            std::cerr << "=== 上传失败！===" << std::endl << "失败原因：" << msg << std::endl;
        }
        file.close();
        return success;
    }

    file.close();
    return true;
}

// 大文件断点续传（新增逻辑，与原有风格一致）
bool upload_large_file(TCPClient& client, const std::string& local_filename, uint32_t block_size = 4 * 1024 * 1024) {
    // 1. 检查文件
    std::ifstream file(local_filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "无法打开文件：" << local_filename << std::endl;
        return false;
    }

    // 2. 文件信息
    std::string filename = std::filesystem::path(local_filename).filename().string();
    uint64_t file_size = get_file_size(local_filename);
    if (file_size == 0) {
        std::cerr << "文件为空或获取大小失败：" << local_filename << std::endl;
        file.close();
        return false;
    }
    uint32_t total_blocks = (file_size + block_size - 1) / block_size; // 总块数（向上取整）
    std::cout << "大文件信息：" << filename << "，大小：" << file_size << "字节，分块：" 
              << total_blocks << "块（每块" << block_size << "字节）" << std::endl;

    // 3. 获取socket
    int sockfd = client.get_client_fd();

    try {
        // 4. 发送分块上传初始化请求，获取file_id
        std::vector<char> req_init = TransferProtocol::pack_block_upload_request(filename, file_size, block_size);
        if (send(sockfd, req_init.data(), req_init.size(), 0) <= 0) {
            throw std::runtime_error("发送分块初始化请求失败");
        }

        std::vector<char> ack_init;
        if (!recv_all(sockfd, ack_init)) {
            throw std::runtime_error("接收分块初始化响应失败");
        }

        std::string file_id;
        if (!TransferProtocol::unpack_block_upload_ack(ack_init, file_id)) {
            throw std::runtime_error("解析file_id失败");
        }
        std::cout << "分块上传初始化成功，file_id：" << file_id << std::endl;

        // 5. 查询已上传的块（获取缺失块）
        std::vector<char> req_query = TransferProtocol::pack_block_query(file_id);
        if (send(sockfd, req_query.data(), req_query.size(), 0) <= 0) {
            throw std::runtime_error("发送块查询请求失败");
        }

        std::vector<char> ack_query;
        if (!recv_all(sockfd, ack_query)) {
            throw std::runtime_error("接收块查询响应失败");
        }

        std::vector<uint32_t> missing_blocks;
        if (!TransferProtocol::unpack_block_query_ack(ack_query, missing_blocks)) {
            throw std::runtime_error("解析缺失块失败");
        }
        std::cout << "需上传的块数：" << missing_blocks.size() << std::endl;

        // 6. 上传缺失的块
        for (size_t i = 0; i < missing_blocks.size(); ++i) {
            uint32_t block_idx = missing_blocks[i];
            uint64_t offset = block_idx * block_size; // 块在文件中的偏移量
            uint32_t data_len = (block_idx == total_blocks - 1) 
                ? static_cast<uint32_t>(file_size - offset)  // 最后一块可能更小
                : block_size;

            // 读取块数据
            std::vector<char> block_data(data_len);
            file.seekg(offset);
            file.read(block_data.data(), data_len);
            if (!file.good()) {
                throw std::runtime_error("读取块" + std::to_string(block_idx) + "数据失败");
            }

            // 发送块数据
            std::vector<char> req_block = TransferProtocol::pack_block_data(file_id, block_idx, block_data);
            if (send(sockfd, req_block.data(), req_block.size(), 0) <= 0) {
                throw std::runtime_error("发送块" + std::to_string(block_idx) + "失败");
            }

            // 接收块响应
            std::vector<char> ack_block;
            if (!recv_all(sockfd, ack_block)) {
                throw std::runtime_error("接收块" + std::to_string(block_idx) + "响应失败");
            }

            bool block_success;
            if (!TransferProtocol::unpack_block_data_ack(ack_block, block_success) || !block_success) {
                throw std::runtime_error("块" + std::to_string(block_idx) + "上传失败");
            }

            // 进度显示（与小文件风格一致）
            float progress = (static_cast<float>(i + 1) / missing_blocks.size()) * 100;
            std::cout << "\r上传进度：" << progress << "%（" << i + 1 << "/" << missing_blocks.size() << "块）";
            std::cout.flush();
        }
        if (!missing_blocks.empty()) {
            std::cout << std::endl;
        }

        // 7. 发送上传完成通知
        std::vector<char> req_finish = TransferProtocol::pack_block_finish(file_id, total_blocks);
        if (send(sockfd, req_finish.data(), req_finish.size(), 0) <= 0) {
            throw std::runtime_error("发送完成通知失败");
        }

        // 8. 接收最终结果
        std::vector<char> ack_finish;
        if (!recv_all(sockfd, ack_finish)) {
            throw std::runtime_error("接收最终结果失败");
        }

        bool success;
        std::string msg, md5;
        if (!TransferProtocol::unpack_block_finish_ack(ack_finish, success, msg, md5)) {
            throw std::runtime_error("解析最终结果失败");
        }

        if (success) {
            std::cout << "=== 上传成功！===" << std::endl << "服务器消息：" << msg << std::endl;
            std::cout << "文件MD5：" << md5 << std::endl;
        } else {
            std::cerr << "=== 上传失败！===" << std::endl << "失败原因：" << msg << std::endl;
        }

        file.close();
        return success;

    } catch (const std::exception& e) {
        std::cerr << "上传异常：" << e.what() << std::endl;
        file.close();
        return false;
    }
}

// 统一上传入口（自动判断文件大小选择方式）
bool upload_file(TCPClient& client, const std::string& local_filename) {
    const uint64_t LARGE_FILE_THRESHOLD = 10 * 1024 * 1024; // 10MB作为大文件阈值
    uint64_t file_size = get_file_size(local_filename);

    if (file_size == 0) {
        std::cerr << "无效文件大小" << std::endl;
        return false;
    }

    // 根据文件大小选择上传方式
    if (file_size > LARGE_FILE_THRESHOLD) {
        std::cout << "检测到大文件，使用断点续传模式" << std::endl;
        return upload_large_file(client, local_filename);
    } else {
        std::cout << "使用普通上传模式" << std::endl;
        return upload_small_file(client, local_filename);
    }
}

int main(int argc, char* argv[])
{
    // 注册信号处理
    signal(SIGINT, signal_handler);

    // 解析命令行参数（保持原有格式）
    if (argc != 5 || std::string(argv[3]) != "upload") {
        std::cerr << "用法错误！正确格式：" << std::endl;
        std::cerr << "./client 服务器IP 端口 upload 本地文件路径" << std::endl;
        std::cerr << "示例：./client 192.168.1.105 8888 upload ./test.pdf" << std::endl;
        return 1;
    }

    std::string server_ip   = argv[1];
    uint16_t    server_port = static_cast<uint16_t>(atoi(argv[2]));
    std::string local_filename = argv[4];

    // 初始化客户端并连接
    TCPClient client;
    g_client = &client;

    if (!client.connect_server(server_ip, server_port)) {
        std::cerr << "客户端启动失败" << std::endl;
        return 1;
    }

    // 执行上传（自动选择模式）
    bool upload_success = upload_file(client, local_filename);

    // 断开连接
    client.disconnect();
    return upload_success ? 0 : 1;
}