#include "protocol/TransferProtocol.h"
#include <cstring> // 用于memcpy

// 打包上传请求
std::vector<char> TransferProtocol::pack_upload_request(const std::string& filename, uint64_t file_size)
{
    std::vector<char> buf;
    // 1. 写入指令类型（1字节）
    buf.push_back(static_cast<char>(CommandType::UPLOAD_REQUEST));
    // 2. 写入文件名长度（4字节，大端字节序）
    uint32_t filename_len = static_cast<uint32_t>(filename.size());
    for (int i = 3; i >= 0; --i) { // 大端：高位在前
        buf.push_back((filename_len >> (8 * i)) & 0xFF);
    }
    // 3. 写入文件名
    buf.insert(buf.end(), filename.begin(), filename.end());
    // 4. 写入文件总大小（4字节？不，支持2GB需8字节！这里用uint64_t，8字节）
    for (int i = 7; i >= 0; --i) {
        buf.push_back((file_size >> (8 * i)) & 0xFF);
    }
    return buf;
}

// 解包上传请求
bool TransferProtocol::unpack_upload_request(const std::vector<char>& buf, std::string& filename, uint64_t& file_size)
{
    // 校验缓冲区最小长度：1（指令）+4（文件名长度）+1（文件名至少1字节）+8（文件大小）=14字节
    if (buf.size() < 14) {
        return false;
    }
    // 1. 校验指令类型
    if (static_cast<CommandType>(buf[0]) != CommandType::UPLOAD_REQUEST) {
        return false;
    }
    // 2. 解析文件名长度（4字节，大端转主机序）
    uint32_t filename_len = 0;
    for (int i = 0; i < 4; ++i) {
        filename_len = (filename_len << 8) | (static_cast<uint8_t>(buf[1 + i]));
    }
    // 3. 解析文件名
    if (buf.size() < 1 + 4 + filename_len + 8) { // 总长度校验
        return false;
    }
    filename.assign(&buf[5], &buf[5 + filename_len]); // 从第5字节开始，取filename_len个字节
    // 4. 解析文件大小（8字节，大端转主机序）
    file_size = 0;
    for (int i = 0; i < 8; ++i) {
        file_size = (file_size << 8) | (static_cast<uint8_t>(buf[5 + filename_len + i]));
    }
    return true;
}

// 打包上传完成（MD5）
std::vector<char> TransferProtocol::pack_upload_finish(const std::string& md5)
{
    std::vector<char> buf;
    if (md5.size() != 32) { // MD5是32位十六进制字符串
        return buf;
    }
    buf.push_back(static_cast<char>(CommandType::UPLOAD_FINISH));
    buf.insert(buf.end(), md5.begin(), md5.end());
    return buf;
}

// 解包上传完成（MD5）
bool TransferProtocol::unpack_upload_finish(const std::vector<char>& buf, std::string& md5)
{
    if (buf.size() != 1 + 32) { // 1字节指令 +32字节MD5
        return false;
    }
    if (static_cast<CommandType>(buf[0]) != CommandType::UPLOAD_FINISH) {
        return false;
    }
    md5.assign(&buf[1], &buf[1 + 32]);
    return true;
}

// 打包服务器响应
std::vector<char> TransferProtocol::pack_upload_ack(bool success, const std::string& msg)
{
    std::vector<char> buf;
    // 1. 命令类型（1字节，必须是 UPLOAD_ACK）
    buf.push_back(static_cast<char>(CommandType::UPLOAD_ACK));

    // 2. 成功标志（1字节，0x00=成功，0x01=失败）
    buf.push_back(success ? 0x00 : 0x01);

    // 3. 消息长度（4字节，网络字节序）
    uint32_t msg_len     = static_cast<uint32_t>(msg.size());
    uint32_t msg_len_net = htonl(msg_len); // 主机字节序 → 网络字节序
    buf.insert(
        buf.end(), reinterpret_cast<char*>(&msg_len_net), reinterpret_cast<char*>(&msg_len_net) + sizeof(uint32_t));

    // 4.消息内容（N字节）
    if (msg_len > 0) {
        buf.insert(buf.end(), msg.begin(), msg.end());
    }

    return buf;
}
