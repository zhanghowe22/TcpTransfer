#include "network/TCPClient.h"
#include "protocol/TransferProtocol.h"
#include "utils/MD5.h"
#include <iostream>
#include <csignal>
#include <fstream>
#include <filesystem>  // C++17文件系统库，获取文件大小

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
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

//获取文件大小
uint64_t get_file_size(const std::string& filename) {
    try {
        return std::filesystem::file_size(filename);
    } catch (...) {
        return 0;
    }
}

// 核心函数：上传文件
bool upload_file(TCPClient& client, const std::string& local_filename) {
    // 1. 检查文件是否存在
    std::ifstream file(local_filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "无法打开文件：" << local_filename << std::endl;
        return false;
    }

    // 2. 获取文件名（仅保留文件名，去掉路径）
    std::string filename = std::filesystem::path(local_filename).filename().string();
    // 3. 获取文件大小
    uint64_t file_size = get_file_size(local_filename);
    if (file_size == 0) {
        std::cerr << "文件为空或获取大小失败：" << local_filename << std::endl;
        file.close();
        return false;
    }

    // 4. 计算文件MD5（传输完成后校验）
    std::string file_md5 = MD5::compute_file(local_filename);
    if (file_md5.empty()) {
        std::cerr << "计算MD5失败：" << local_filename << std::endl;
        file.close();
        return false;
    }
    std::cout << "文件信息：" << filename << "，大小：" << file_size << "字节，MD5：" << file_md5 << std::endl;

    // 5. 打包并发送“上传请求”
    std::vector<char> req_buf = TransferProtocol::pack_upload_request(filename, file_size);
    int send_len = send(client.get_client_fd(), req_buf.data(), req_buf.size(), 0);
    if (send_len != static_cast<int>(req_buf.size())) {
        std::cerr << "发送上传请求失败" << std::endl;
        file.close();
        return false;
    }
    std::cout << "已发送上传请求，等待服务器响应..." << std::endl;

    // 6. 接收服务器响应（简化：假设服务器收到请求后直接准备接收数据）
    // （后续可扩展：服务器返回“准备就绪”后再发数据）

    // 7. 发送文件数据（一次性发送小文件，后续扩展分块）
    const int BUF_SIZE = 4096;
    char buf[BUF_SIZE];
    uint64_t sent_size = 0;
    std::cout << "开始上传文件..." << std::endl;
    while (!file.eof()) {
        file.read(buf, BUF_SIZE);
        size_t read_len = file.gcount();
        if (read_len == 0) break;

        // 发送数据块（简化：直接发数据，后续按协议打包块编号）
        send_len = send(client.get_client_fd(), buf, read_len, 0);
        if (send_len <= 0) {
            std::cerr << "发送数据失败" << std::endl;
            file.close();
            return false;
        }

        sent_size += send_len;
        // 打印上传进度
        float progress = (static_cast<float>(sent_size) / file_size) * 100;
        std::cout << "\r上传进度：" << progress << "% (" << sent_size << "/" << file_size << "字节)";
        std::cout.flush();
    }
    std::cout << std::endl << "文件数据发送完成" << std::endl;

    // 8. 发送“上传完成”指令（带MD5）
    std::vector<char> finish_buf = TransferProtocol::pack_upload_finish(file_md5);
    send_len = send(client.get_client_fd(), finish_buf.data(), finish_buf.size(), 0);
    if (send_len != static_cast<int>(finish_buf.size())) {
        std::cerr << "发送上传完成指令失败" << std::endl;
        file.close();
        return false;
    }

    // 9. 接收服务器的最终响应
    char ack_buf[1024] = {0};
    int recv_len = recv(client.get_client_fd(), ack_buf, sizeof(ack_buf), 0);
    if (recv_len <= 0) {
        std::cerr << "未收到服务器响应" << std::endl;
        file.close();
        return false;
    }

    // 10. 解析服务器响应
    std::vector<char> ack_vec(ack_buf, ack_buf + recv_len);
    if (ack_vec.size() >= 2 && static_cast<CommandType>(ack_vec[0]) == CommandType::UPLOAD_ACK) {
        bool success = (ack_vec[1] == 0x00);
        if (success) {
            std::cout << "=== 上传成功！服务器校验MD5一致 ===" << std::endl;
        } else {
            std::cout << "=== 上传失败！服务器校验MD5不一致 ===" << std::endl;
        }
        file.close();
        return success;
    }

    file.close();
    return true;
}

int main(int argc, char* argv[])
{
    // 注册信号处理（Ctrl+C优雅退出）
    signal(SIGINT, signal_handler);

    // 解析命令行参数：./client IP 端口 upload 文件名
    if (argc != 5 || std::string(argv[3]) != "upload") {
        std::cerr << "用法错误！正确格式：" << std::endl;
        std::cerr << "./client 服务器IP 端口 upload 本地文件路径" << std::endl;
        std::cerr << "示例：./client 192.168.1.105 8888 upload ./test.pdf" << std::endl;
        return 1;
    }

    // 解析命令行参数
    std::string server_ip   = argv[1];
    uint16_t    server_port = static_cast<uint16_t>(atoi(argv[2])); // 字符串转端口号
    std::string local_filename = argv[4];

    // 初始化客户端并连接服务器
    TCPClient client;
    g_client = &client;

    if (!client.connect_server(server_ip, server_port)) {
        std::cerr << "客户端启动失败" << std::endl;
        return 1;
    }

    // 执行上传
    bool upload_success = upload_file(client, local_filename);

    // 上传完成后断开连接
    client.disconnect();
    return upload_success ? 0 : 1;
}