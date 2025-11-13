#pragma once
#include <string>
#include <cstdint>

// 跨平台兼容头文件
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

class TCPClient{
    public:
    TCPClient();
    ~TCPClient();

    // 连接服务器(服务器IP、端口)
    bool connect_server(const std::string& server_ip,uint16_t server_port);

    // 关闭连接
    void disconnect();

    // 判断是否已连接
    bool is_connected() const {return is_connected_;}

    // 获取客户端socket（用于传输文件）
    int get_client_fd() const {return client_fd_;}

    private:
    int client_fd_; // 客户端socket
    bool is_connected_;

    // 初始化socket（Windows需WSAStartup）
    bool init_socket();
};
