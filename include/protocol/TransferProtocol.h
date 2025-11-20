#pragma once
#ifdef _WIN32
#include <winsock2.h>              // Windows 下的 socket 头文件（包含 htonl/ntohl）
#pragma comment(lib, "ws2_32.lib") // 链接 Winsock 库（Windows 必需）
#else
#include <arpa/inet.h>  // Linux 下的字节序转换头文件（包含 htonl/ntohl）
#include <netinet/in.h> // 辅助包含，确保函数声明完整
#endif
#include <cstdint>
#include <string>
#include <vector>

enum class CommandType : uint8_t {
    UPLOAD_REQUEST       = 0x01, // 上传请求（带文件名、文件大小）
    UPLOAD_FINISH        = 0x02, // 上传完成（带MD5）
    UPLOAD_ACK           = 0x03, // 服务器响应（成功/失败）
    BLOCK_UPLOAD_REQUEST = 0x04, // 文件分块发送请求
    BLOCK_QUERY          = 0x05, // 分块查询
    BLOCK_DATA           = 0x06, // 分块数据
    BLOCK_FINISH         = 0x07  // 分块发送完成
};

// 协议工具类（封装打包/解包逻辑）
class TransferProtocol
{
  public:
    // 打包上传请求：[0x01][4字节文件名长度][文件名][4字节文件总大小]
    static std::vector<char> pack_upload_request(const std::string& filename, uint64_t file_size);

    // 解包上传请求：从缓冲区中解析出文件名和文件大小
    static bool unpack_upload_request(const std::vector<char>& buf, std::string& filename, uint64_t& file_size);

    // 打包上传完成指令：[0x03][32字节MD5值]
    static std::vector<char> pack_upload_finish(const std::string& md5);

    // 分块上传协议的打包函数
    // 打包“分块上传初始化请求”（客户端→服务端）
    static std::vector<char>
    pack_block_upload_request(const std::string& filename, uint64_t file_size, uint32_t block_size);

    // 打包“块查询请求”（客户端→服务端）
    static std::vector<char> pack_block_query(const std::string& file_id);

    // 打包“块数据”（客户端→服务端）
    static std::vector<char>
    pack_block_data(const std::string& file_id, uint32_t block_idx, const std::vector<char>& block_data);

    // 打包“分块上传完成通知”（客户端→服务端）
    static std::vector<char> pack_block_finish(const std::string& file_id, uint32_t total_blocks);

    // 分块上传协议的解包函数
    // 解包“分块上传初始化响应”（服务端→客户端），提取file_id
    static bool unpack_block_upload_ack(const std::vector<char>& data, std::string& file_id);

    // 解包“块查询响应”（服务端→客户端），提取缺失的块序号
    static bool unpack_block_query_ack(const std::vector<char>& data, std::vector<uint32_t>& missing_blocks);

    // 解包“块数据响应”（服务端→客户端），判断是否成功
    static bool unpack_block_data_ack(const std::vector<char>& data, bool& success);

    // 解包“分块上传完成响应”（服务端→客户端），提取最终结果
    static bool
    unpack_block_finish_ack(const std::vector<char>& data, bool& success, std::string& msg, std::string& md5);

    // 解包上传完成指令：解析出MD5值
    static bool unpack_upload_finish(const std::vector<char>& buf, std::string& md5);

    // 打包服务器响应：[0x04][1字节状态（0=成功，1=失败）]
    static std::vector<char> pack_upload_ack(bool success, const std::string& msg = "");

    // TransferProtocol.h 新增声明
    // 分块上传请求解包（客户端→服务端）
    static bool unpack_block_upload_request(const std::vector<char>& data,
                                            std::string&             filename,
                                            uint64_t&                total_size,
                                            uint32_t&                block_size);
    // 块查询请求解包
    static bool unpack_block_query(const std::vector<char>& data, std::string& file_id);
    // 块数据解包
    static bool unpack_block_data(const std::vector<char>& data,
                                  std::string&             file_id,
                                  uint32_t&                block_idx,
                                  std::vector<char>&       block_data);
    // 分块完成请求解包
    static bool unpack_block_finish(const std::vector<char>& data, std::string& file_id, uint32_t& total_blocks);

    // 分块初始化响应打包（服务端→客户端）
    static std::vector<char> pack_block_upload_ack(bool success, const std::string& file_id);
    // 块查询响应打包
    static std::vector<char> pack_block_query_ack(bool success, const std::vector<uint32_t>& missing_blocks);
    // 块数据响应打包
    static std::vector<char> pack_block_data_ack(bool success, const std::string& msg = "");
    // 分块完成响应打包
    static std::vector<char> pack_block_finish_ack(bool success, const std::string& msg, const std::string& md5);
};