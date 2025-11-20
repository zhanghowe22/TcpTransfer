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

// 打包“分块上传初始化请求”
std::vector<char>
TransferProtocol::pack_block_upload_request(const std::string& filename, uint64_t file_size, uint32_t block_size)
{
    std::vector<char> data;
    // 1. 命令类型
    data.push_back(static_cast<char>(CommandType::BLOCK_UPLOAD_REQUEST));
    // 2. 文件名长度（网络字节序）
    uint32_t filename_len = htonl(static_cast<uint32_t>(filename.size()));
    data.insert(data.end(), (char*)&filename_len, (char*)&filename_len + 4);
    // 3. 文件名
    data.insert(data.end(), filename.begin(), filename.end());
    // 4. 文件总大小（8字节，直接存储）
    data.insert(data.end(), (char*)&file_size, (char*)&file_size + 8);
    // 5. 块大小（网络字节序）
    uint32_t block_size_net = htonl(block_size);
    data.insert(data.end(), (char*)&block_size_net, (char*)&block_size_net + 4);
    return data;
}

// 打包“块查询请求”
std::vector<char> TransferProtocol::pack_block_query(const std::string& file_id)
{
    std::vector<char> data;
    // 1. 命令类型
    data.push_back(static_cast<char>(CommandType::BLOCK_QUERY));
    // 2. file_id长度（网络字节序）
    uint32_t id_len = htonl(static_cast<uint32_t>(file_id.size()));
    data.insert(data.end(), (char*)&id_len, (char*)&id_len + 4);
    // 3. file_id
    data.insert(data.end(), file_id.begin(), file_id.end());
    return data;
}

// 打包“块数据”
std::vector<char>
TransferProtocol::pack_block_data(const std::string& file_id, uint32_t block_idx, const std::vector<char>& block_data)
{
    std::vector<char> data;
    // 1. 命令类型
    data.push_back(static_cast<char>(CommandType::BLOCK_DATA));
    // 2. file_id
    data.insert(data.end(), file_id.begin(), file_id.end());
    // 3. 块序号（网络字节序）
    uint32_t block_idx_net = htonl(block_idx);
    data.insert(data.end(), (char*)&block_idx_net, (char*)&block_idx_net + 4);
    // 4. 块数据长度（网络字节序）
    uint32_t data_len = htonl(static_cast<uint32_t>(block_data.size()));
    data.insert(data.end(), (char*)&data_len, (char*)&data_len + 4);
    // 5. 块数据
    data.insert(data.end(), block_data.begin(), block_data.end());
    return data;
}

// 打包“分块上传完成通知”
std::vector<char> TransferProtocol::pack_block_finish(const std::string& file_id, uint32_t total_blocks)
{
    std::vector<char> data;
    // 1. 命令类型
    data.push_back(static_cast<char>(CommandType::BLOCK_FINISH));
    // 2. file_id
    data.insert(data.end(), file_id.begin(), file_id.end());
    // 3. 总块数（网络字节序）
    uint32_t total_blocks_net = htonl(total_blocks);
    data.insert(data.end(), (char*)&total_blocks_net, (char*)&total_blocks_net + 4);
    return data;
}

// 解包“分块上传初始化响应”（提取file_id）
bool TransferProtocol::unpack_block_upload_ack(const std::vector<char>& data, std::string& file_id)
{
    if (data.size() < 6) return false; // 1(命令)+1(成功标志)+4(长度)
    // 校验命令类型
    if (static_cast<CommandType>(data[0]) != CommandType::UPLOAD_ACK) return false;
    // 校验成功标志
    if (data[1] != 0x00) return false; // 0x00表示成功
    // 解析file_id长度
    uint32_t id_len = ntohl(*(uint32_t*)(data.data() + 2));
    // 解析file_id内容
    if (data.size() < 6 + id_len) return false;
    file_id = std::string(data.begin() + 6, data.begin() + 6 + id_len);
    return true;
}

// 解包“块查询响应”（提取缺失块序号）
bool TransferProtocol::unpack_block_query_ack(const std::vector<char>& data, std::vector<uint32_t>& missing_blocks)
{
    if (data.size() < 6) return false; // 1(命令)+1(成功标志)+4(数量)
    // 校验命令类型
    if (static_cast<CommandType>(data[0]) != CommandType::UPLOAD_ACK) return false;
    // 校验成功标志
    if (data[1] != 0x00) return false;
    // 解析块数量
    uint32_t block_count = ntohl(*(uint32_t*)(data.data() + 2));
    // 校验总长度
    if (data.size() < 6 + block_count * 4) return false;
    // 解析每个块序号
    missing_blocks.clear();
    for (uint32_t i = 0; i < block_count; ++i) {
        uint32_t block_idx = ntohl(*(uint32_t*)(data.data() + 6 + i * 4));
        missing_blocks.push_back(block_idx);
    }
    return true;
}

// 解包“块数据响应”（判断是否成功）
bool TransferProtocol::unpack_block_data_ack(const std::vector<char>& data, bool& success)
{
    if (data.size() < 2) return false; // 1(命令)+1(成功标志)
    if (static_cast<CommandType>(data[0]) != CommandType::UPLOAD_ACK) return false;
    success = (data[1] == 0x00); // 0x00成功，其他失败
    return true;
}

// 解包“分块上传完成响应”（提取结果和MD5）
bool TransferProtocol::unpack_block_finish_ack(const std::vector<char>& data,
                                               bool&                    success,
                                               std::string&             msg,
                                               std::string&             md5)
{
    if (data.size() < 6) return false; // 1(命令)+1(成功标志)+4(消息长度)
    if (static_cast<CommandType>(data[0]) != CommandType::UPLOAD_ACK) return false;
    // 解析成功标志
    success = (data[1] == 0x00);
    // 解析消息长度
    uint32_t msg_len = ntohl(*(uint32_t*)(data.data() + 2));
    if (data.size() < 6 + msg_len) return false;
    // 解析消息内容
    msg = std::string(data.begin() + 6, data.begin() + 6 + msg_len);
    // 提取MD5（假设消息格式为"上传成功，MD5=xxx"）
    size_t md5_pos = msg.find("MD5=");
    if (md5_pos != std::string::npos) {
        md5 = msg.substr(md5_pos + 4);
    }
    return true;
}

bool TransferProtocol::unpack_block_upload_request(const std::vector<char>& data,
                                                   std::string&             filename,
                                                   uint64_t&                total_size,
                                                   uint32_t&                block_size)
{
    if (data.size() < 4 + 8 + 4) return false;
    size_t offset = 0;
    // 文件名长度
    uint32_t filename_len = ntohl(*(uint32_t*)(data.data() + offset));
    offset += 4;
    // 文件名
    if (data.size() < offset + filename_len) return false;
    filename = std::string(data.begin() + offset, data.begin() + offset + filename_len);
    offset += filename_len;
    // 文件总大小
    total_size = *(uint64_t*)(data.data() + offset);
    offset += 8;
    // 块大小
    block_size = ntohl(*(uint32_t*)(data.data() + offset));
    return true;
}

bool TransferProtocol::unpack_block_query(const std::vector<char>& data, std::string& file_id)
{
    if (data.size() < 4) return false;
    uint32_t id_len = ntohl(*(uint32_t*)(data.data()));
    if (data.size() < 4 + id_len) return false;
    file_id = std::string(data.begin() + 4, data.begin() + 4 + id_len);
    return true;
}

bool TransferProtocol::unpack_block_data(const std::vector<char>& data,
                                         std::string&             file_id,
                                         uint32_t&                block_idx,
                                         std::vector<char>&       block_data)
{
    size_t offset = 0;
    // file_id长度（隐含在data中，按客户端打包逻辑，file_id后接块序号）
    uint32_t id_len = ntohl(*(uint32_t*)(data.data() + offset));
    offset += 4;
    if (data.size() < offset + id_len + 4 + 4) return false;
    // file_id
    file_id = std::string(data.begin() + offset, data.begin() + offset + id_len);
    offset += id_len;
    // 块序号
    block_idx = ntohl(*(uint32_t*)(data.data() + offset));
    offset += 4;
    // 块数据长度
    uint32_t data_len = ntohl(*(uint32_t*)(data.data() + offset));
    offset += 4;
    // 块数据
    if (data.size() < offset + data_len) return false;
    block_data.assign(data.begin() + offset, data.begin() + offset + data_len);
    return true;
}

bool TransferProtocol::unpack_block_finish(const std::vector<char>& data, std::string& file_id, uint32_t& total_blocks)
{
    size_t   offset = 0;
    uint32_t id_len = ntohl(*(uint32_t*)(data.data() + offset));
    offset += 4;
    if (data.size() < offset + id_len + 4) return false;
    file_id = std::string(data.begin() + offset, data.begin() + offset + id_len);
    offset += id_len;
    total_blocks = ntohl(*(uint32_t*)(data.data() + offset));
    return true;
}

std::vector<char> TransferProtocol::pack_block_upload_ack(bool success, const std::string& file_id)
{
    std::vector<char> data;
    data.push_back(static_cast<char>(CommandType::UPLOAD_ACK));
    data.push_back(success ? 0x00 : 0x01);
    uint32_t id_len = htonl(static_cast<uint32_t>(file_id.size()));
    data.insert(data.end(), (char*)&id_len, (char*)&id_len + 4);
    data.insert(data.end(), file_id.begin(), file_id.end());
    return data;
}

std::vector<char> TransferProtocol::pack_block_query_ack(bool success, const std::vector<uint32_t>& missing_blocks)
{
    std::vector<char> data;
    data.push_back(static_cast<char>(CommandType::UPLOAD_ACK));
    data.push_back(success ? 0x00 : 0x01);
    uint32_t count = htonl(static_cast<uint32_t>(missing_blocks.size()));
    data.insert(data.end(), (char*)&count, (char*)&count + 4);
    for (uint32_t idx : missing_blocks) {
        uint32_t idx_net = htonl(idx);
        data.insert(data.end(), (char*)&idx_net, (char*)&idx_net + 4);
    }
    return data;
}

std::vector<char> TransferProtocol::pack_block_data_ack(bool success, const std::string& msg)
{
    return pack_upload_ack(success, msg);
}

std::vector<char> TransferProtocol::pack_block_finish_ack(bool success, const std::string& msg, const std::string& md5)
{
    std::string full_msg = msg + "，MD5=" + md5;
    return pack_upload_ack(success, full_msg);
}