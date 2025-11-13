#include <cstring>
#include <iostream>
#include <network/TCPClient.h>

#ifdef _WIN32
#include <winsock2.h>  // Windows的socket函数
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>  // 新增：Linux下inet_pton的头文件
#include <unistd.h>     // 新增：Linux下close的头文件
#endif

// Windows初始化Winsock
#ifdef _WIN32
bool TCPClient::init_socket()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup失败，错误码：" << WSAGetLastError() << std::endl;
        return false;
    }
    return true;
}
#else
bool TCPClient::init_socket()
{
    return true; // Linux无需额外初始化
}
#endif

TCPClient::TCPClient() : client_fd_(-1), is_connected_(false) {}

TCPClient::~TCPClient()
{
    disconnect(); // RAII：析构时自动断开连接
#ifdef _WIN32
    WSACleanup(); // Windows清理资源
#endif
}

bool TCPClient::connect_server(const std::string& server_ip, uint16_t server_port)
{
    // 先初始化socket
    if (!init_socket()) {
        return false;
    }

    // 创建客户端socket（IPv4、TCP）
    client_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd_ == -1) {
#ifdef _WIN32
        std::cerr << "创建socket失败，错误码：" << WSAGetLastError() << std::endl;
#else
        perror("创建socket失败");
#endif
        return false;
    }

    sockaddr_in server_addr{};
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(server_port);

    // 将服务器IP字符串转换为二进制格式
#ifdef _WIN32
    // Windows用inet_addr（兼容IPv4）
    server_addr.sin_addr.s_addr = inet_addr(server_ip.c_str());
    if (server_addr.sin_addr.s_addr == INADDR_NONE) {
        std::cerr << "IP地址格式错误：" << server_ip << std::endl;
        return false;
    }
#else
    // Linux用inet_pton（标准方法，兼容IPv4/IPv6）
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        perror("IP地址格式错误");
        return false;
    }
#endif

    if (connect(client_fd_, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
#ifdef _WIN32
        std::cerr << "连接服务器失败，错误码：" << WSAGetLastError() << std::endl;
#else
        perror("连接服务器失败");
#endif
        return false;
    }

    is_connected_ = true;
    std::cout << "成功连接到服务器：" << server_ip << ":" << server_port << std::endl;
    return true;
}

void TCPClient::disconnect()
{
    if (client_fd_ != -1) {
#ifdef _WIN32
        closesocket(client_fd_);
#else
        close(client_fd_);
#endif
        client_fd_ = -1;
    }
    is_connected_ = false;
    std::cout << "已断开与服务器的连接" << std::endl;
}