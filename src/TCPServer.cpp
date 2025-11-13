#include <iostream> // 临时用cout打印日志（后续换spdlog）
#include <network/TCPServer.h>
#include <cstring>

// Windows初始化Winsock
#ifdef _WIN32
bool TCPServer::init_socket()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return false;
    }
    return true;
}
#else
// Linux不需要额外初始化
bool TCPServer::init_socket()
{
    return true;
}
#endif

TCPServer::TCPServer(uint16_t port) : port_(port), listen_fd_(-1), is_running_(false) {}

TCPServer::~TCPServer()
{
    stop(); // 析构时自动关闭，体现RAII
#ifdef _WIN32
    WSACleanup(); // Windows清理
#endif
}

bool TCPServer::start()
{
    if (!init_socket()) {
        return false;
    }

    // 创建监听socket(IPV4, TCP)
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == -1) {
        std::cerr << "创建socket失败" << std::endl;
        return false;
    }

    // 绑定地址和端口
    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;   // 监听所有网卡
    server_addr.sin_port        = htons(port_); // 端口转换为网络字节序

    if (bind(listen_fd_, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::cerr << "绑定端口" << port_ << "失败（可能被占用）" << std::endl;
        return false;
    }

    // 开始监听（最大等待队列10个）
    if (listen(listen_fd_, 10) == -1) {
        std::cerr << "监听失败" << std::endl;
        return false;
    }

    is_running_ = true;

    std::cout << "服务器启动成功，监听端口：" << port_ << std::endl;
    return true;
}

int TCPServer::accept_client(std::string& client_ip, uint16_t& client_port)
{
    client_ip.clear();  
    client_port = 0;  

    if (!is_running_) {
        return -1;
    }

    sockaddr_in client_addr{};
    socklen_t   client_len = sizeof(client_addr);
    memset(&client_addr, 0, sizeof(client_addr));  

    int client_fd = accept(listen_fd_, (sockaddr*)&client_addr, &client_len);
    if (client_fd == -1) {
        std::cerr << "接受连接失败" << std::endl;
        return -1;
    }

    // 获取客户端IP和端口（转换为字符串和主机字节序）  
    #ifdef _WIN32  
        // Windows：inet_ntoa 返回静态缓冲区指针，直接赋值给 string（安全）
        char* ip_ptr = inet_ntoa(client_addr.sin_addr);  
        if (ip_ptr != nullptr) {  
            client_ip = ip_ptr;  
        } else {  
            client_ip = "unknown_ip";  
        }  
    #else  
        // Linux：用固定 16 字节缓冲区（IPv4 最长 15 字符 + 1 个结束符 '\0'）
        char ip_buf[16] = {0};  // 明确大小，避免 INET_ADDRSTRLEN 未定义的问题
        // 调用 inet_ntop，严格指定缓冲区大小（16）
        const char* ip_ptr = inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));  
        if (ip_ptr != nullptr) {  
            client_ip = ip_buf;  
        } else {  
            client_ip = "unknown_ip";  
            perror("inet_ntop 转换失败");  // 打印转换失败原因
        }  
    #endif  

    client_port = ntohs(client_addr.sin_port);  // 网络字节序转主机字节序  

    std::cout << "客户端连接成功：IP=" << client_ip << ", 端口=" << client_port << ", fd=" << client_fd << std::endl;  

    return client_fd; 
}

void TCPServer::stop() {  
    if (listen_fd_ != -1) {  
        #ifdef _WIN32  
        closesocket(listen_fd_);  // Windows关闭socket  
        #else  
        close(listen_fd_);  // Linux关闭socket  
        #endif  
        listen_fd_ = -1;  
    }  
    is_running_ = false;  
    std::cout << "服务器已关闭" << std::endl;  
} 