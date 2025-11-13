#pragma once
#include <string>
#include <cstdint>

// 跨平台兼容：Windows需要Winsock2.h，Linux需要sys/socket.h  
#ifdef _WIN32  
#include <winsock2.h>  
#else  
#include <sys/socket.h>  
#include <netinet/in.h>  
#include <unistd.h>
#include <arpa/inet.h>  // Linux下inet_ntop的头文件
#endif

class TCPServer{
public:
    TCPServer(uint16_t port = 8888);
    ~TCPServer();
    // 启动服务器 (绑定 + 监听)
    bool start();
    // 关闭服务器  
    void stop();
    // 等待并接受客户端连接（返回客户端socket） 
    int accept_client(std::string& client_ip, uint16_t& client_port);

private:
    int listen_fd_;  // 监听socket 
    uint16_t port_; // 端口号  
    bool is_running_;

    // 初始化socket（Windows需要WSAStartup） 
    bool init_socket();
};