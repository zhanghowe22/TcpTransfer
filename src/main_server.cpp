#include "network/TCPServer.h"
#include "protocol/TransferProtocol.h"
#include "utils/MD5.h"
#include <csignal> // 处理Ctrl+C退出
#include <iostream>
#include <unistd.h>
#include <fstream>
#include <filesystem>
#include "thread/ThreadPool.h" 

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

// 全局变量：用于信号处理中关闭服务器
TCPServer* g_server = nullptr;

// 信号处理函数（捕获Ctrl+C）
void signal_handler(int signum)
{
    if (g_server != nullptr) {
        g_server->stop();
    }
    std::cout << "收到退出信号，正在关闭..." << std::endl;
    exit(0);
}
// 核心函数：处理客户端上传（单个客户端）
void handle_client_upload(int client_fd, const std::string& client_ip, uint16_t client_port) {
    std::cout << "开始处理客户端[" << client_ip << ":" << client_port << "]的上传请求" << std::endl;

    // 1. 接收“上传请求”指令
    char req_buf[1024] = {0};
    int recv_len = recv(client_fd, req_buf, sizeof(req_buf), 0);
    if (recv_len <= 0) {
        std::cerr << "客户端[" << client_ip << ":" << client_port << "]断开连接（未接收上传请求）" << std::endl;
        return;
    }

    // 2. 解包上传请求，获取文件名和文件大小
    std::vector<char> req_vec(req_buf, req_buf + recv_len);
    std::string filename;
    uint64_t file_size;
    if (!TransferProtocol::unpack_upload_request(req_vec, filename, file_size)) {
        std::cerr << "解析上传请求失败" << std::endl;
        return;
    }
    std::cout << "收到上传请求：文件名=" << filename << ", 大小=" << file_size << "字节" << std::endl;

    // 3. 创建保存目录（./recv），如果不存在则创建
    std::string save_dir = "./recv";
    if (!std::filesystem::exists(save_dir)) {
        std::filesystem::create_directory(save_dir);
    }
    std::string save_path = save_dir + "/" + filename;

    // 4. 创建文件，准备写入数据
    std::ofstream file(save_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "无法创建文件：" << save_path << std::endl;
        return;
    }

    // 5. 接收文件数据
    const int BUF_SIZE = 4096;
    char buf[BUF_SIZE];
    uint64_t recv_size = 0;
    std::cout << "开始接收文件数据..." << std::endl;
    while (recv_size < file_size) {
        // 计算剩余需要接收的字节数（避免缓冲区溢出）
        size_t need_recv = std::min(static_cast<size_t>(file_size - recv_size), static_cast<size_t>(BUF_SIZE));
        recv_len = recv(client_fd, buf, need_recv, 0);
        if (recv_len <= 0) {
            std::cerr << "接收文件数据失败，客户端断开" << std::endl;
            file.close();
            std::filesystem::remove(save_path);  // 删除不完整文件
            return;
        }

        // 写入文件
        file.write(buf, recv_len);
        recv_size += recv_len;

        // 打印接收进度
        float progress = (static_cast<float>(recv_size) / file_size) * 100;
        std::cout << "\r接收进度：" << progress << "% (" << recv_size << "/" << file_size << "字节)";
        std::cout.flush();
    }
    std::cout << std::endl << "文件数据接收完成，保存路径：" << save_path << std::endl;
    file.close();

    // 6. 接收客户端发送的MD5，进行校验
    char finish_buf[1024] = {0};
    recv_len = recv(client_fd, finish_buf, sizeof(finish_buf), 0);
    if (recv_len <= 0) {
        std::cerr << "未收到客户端MD5" << std::endl;
        return;
    }
    std::vector<char> finish_vec(finish_buf, finish_buf + recv_len);
    std::string client_md5;
    if (!TransferProtocol::unpack_upload_finish(finish_vec, client_md5)) {
        std::cerr << "解析MD5失败" << std::endl;
        return;
    }

    // 7. 计算服务器接收文件的MD5
    std::string server_md5 = MD5::compute_file(save_path);
    bool md5_match = (client_md5 == server_md5);
    std::cout << "客户端MD5：" << client_md5 << std::endl;
    std::cout << "服务器MD5：" << server_md5 << std::endl;
    std::cout << "MD5校验" << (md5_match ? "通过" : "失败") << std::endl;

    // 8. 向客户端发送响应
    std::vector<char> ack_buf = TransferProtocol::pack_upload_ack(md5_match);
    send(client_fd, ack_buf.data(), ack_buf.size(), 0);

    // 9. 关闭客户端连接
    #ifdef _WIN32
    closesocket(client_fd);
    #else
    close(client_fd);
    #endif
    std::cout << "处理客户端[" << client_ip << ":" << client_port << "]上传完成" << std::endl;
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
        thread_pool.submit([client_fd, client_ip, client_port]() {
            handle_client_upload(client_fd, client_ip, client_port);
        });
    }

    return 0;
}