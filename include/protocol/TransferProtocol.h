#pragma once
#ifdef _WIN32
#include <winsock2.h>          // Windows 下的 socket 头文件（包含 htonl/ntohl）
#pragma comment(lib, "ws2_32.lib")  // 链接 Winsock 库（Windows 必需）
#else
#include <arpa/inet.h>         // Linux 下的字节序转换头文件（包含 htonl/ntohl）
#include <netinet/in.h>        // 辅助包含，确保函数声明完整
#endif
#include <string>
#include <vector>
#include <cstdint>

enum class CommandType :uint8_t {
    UPLOAD_REQUEST = 0x01, // 上传请求（带文件名、文件大小）
    DATA_BLOCK = 0x02, // 数据块（带块编号、块数据）
    UPLOAD_FINISH = 0x03, // 上传完成（带MD5）
    UPLOAD_ACK = 0x04 // 服务器响应（成功/失败）
};

// 协议工具类（封装打包/解包逻辑）
class TransferProtocol {

    public:
    // 1. 打包上传请求：[0x01][4字节文件名长度][文件名][4字节文件总大小]
    static std::vector<char> pack_upload_request(const std::string& filename, uint64_t file_size);

    // 2. 解包上传请求：从缓冲区中解析出文件名和文件大小
    static bool unpack_upload_request(const std::vector<char>& buf, std::string& filename, uint64_t& file_size);

    // 3. 打包上传完成指令：[0x03][32字节MD5值]
    static std::vector<char> pack_upload_finish(const std::string& md5);

    // 4. 解包上传完成指令：解析出MD5值
    static bool unpack_upload_finish(const std::vector<char>& buf, std::string& md5);

    // 5. 打包服务器响应：[0x04][1字节状态（0=成功，1=失败）]
    static std::vector<char> pack_upload_ack(bool success, const std::string& msg = "");
};