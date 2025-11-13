#pragma once
#include <string>
#include <cstdint>
#include <fstream>

class MD5 {
private:
    uint32_t state[4];
    uint32_t count[2];
    uint8_t buffer[64];
    uint8_t digest[16];
    bool finalized;

    void init();
    void update(const uint8_t* data, size_t length);
    void final();
    static void transform(const uint8_t block[64], uint32_t state[4]);

public:
    MD5();
    MD5(const std::string& data);
    void update(const std::string& data);
    std::string get_digest();
    static std::string compute(const std::string& data);
    static std::string compute_file(const std::string& filename);
};

// 以下是实现（直接粘贴到头文件，简化编译，适合单文件工具）
#include <cstring>
#include <algorithm>

MD5::MD5() { init(); }
MD5::MD5(const std::string& data) { init(); update(data); }

void MD5::init() {
    state[0] = 0x67452301;
    state[1] = 0xEFCDAB89;
    state[2] = 0x98BADCFE;
    state[3] = 0x10325476;
    count[0] = count[1] = 0;
    finalized = false;
    memset(buffer, 0, sizeof(buffer));
}

inline uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
inline uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
inline uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
inline uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }
inline uint32_t rotate_left(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline void FF(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac) {
    a = rotate_left(a + F(b, c, d) + x + ac, s) + b;
}
inline void GG(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac) {
    a = rotate_left(a + G(b, c, d) + x + ac, s) + b;
}
inline void HH(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac) {
    a = rotate_left(a + H(b, c, d) + x + ac, s) + b;
}
inline void II(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac) {
    a = rotate_left(a + I(b, c, d) + x + ac, s) + b;
}

void MD5::transform(const uint8_t block[64], uint32_t state[4]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[16];
    for (int i = 0; i < 16; ++i) x[i] = (block[i*4] << 24) | (block[i*4+1] << 16) | (block[i*4+2] << 8) | block[i*4+3];

    FF(a, b, c, d, x[0], 7, 0xD76AA478); FF(d, a, b, c, x[1], 12, 0xE8C7B756);
    FF(c, d, a, b, x[2], 17, 0x242070DB); FF(b, c, d, a, x[3], 22, 0xC1BDCEEE);
    FF(a, b, c, d, x[4], 7, 0xF57C0FAF); FF(d, a, b, c, x[5], 12, 0x4787C62A);
    FF(c, d, a, b, x[6], 17, 0xA8304613); FF(b, c, d, a, x[7], 22, 0xFD469501);
    FF(a, b, c, d, x[8], 7, 0x698098D8); FF(d, a, b, c, x[9], 12, 0x8B44F7AF);
    FF(c, d, a, b, x[10], 17, 0xFFFF5BB1); FF(b, c, d, a, x[11], 22, 0x895CD7BE);
    FF(a, b, c, d, x[12], 7, 0x6B901122); FF(d, a, b, c, x[13], 12, 0xFD987193);
    FF(c, d, a, b, x[14], 17, 0xA679438E); FF(b, c, d, a, x[15], 22, 0x49B40821);

    GG(a, b, c, d, x[1], 5, 0xF61E2562); GG(d, a, b, c, x[6], 9, 0xC040B340);
    GG(c, d, a, b, x[11], 14, 0x265E5A51); GG(b, c, d, a, x[0], 20, 0xE9B6C7AA);
    GG(a, b, c, d, x[5], 5, 0xD62F105D); GG(d, a, b, c, x[10], 9, 0x2441453);
    GG(c, d, a, b, x[15], 14, 0xD8A1E681); GG(b, c, d, a, x[4], 20, 0xE7D3FBC8);
    GG(a, b, c, d, x[9], 5, 0x21E1CDE6); GG(d, a, b, c, x[14], 9, 0xC33707D6);
    GG(c, d, a, b, x[3], 14, 0xF4D50D87); GG(b, c, d, a, x[8], 20, 0x455A14ED);
    GG(a, b, c, d, x[13], 5, 0xA9E3E905); GG(d, a, b, c, x[2], 9, 0xFCEFA3F8);
    GG(c, d, a, b, x[7], 14, 0x676F02D9); GG(b, c, d, a, x[12], 20, 0x8D2A4C8A);

    HH(a, b, c, d, x[5], 4, 0xFFFA3942); HH(d, a, b, c, x[8], 11, 0x8771F681);
    HH(c, d, a, b, x[11], 16, 0x6D9D6122); HH(b, c, d, a, x[14], 23, 0xFDE5380C);
    HH(a, b, c, d, x[1], 4, 0xA4BEEA44); HH(d, a, b, c, x[4], 11, 0x4BDECFA9);
    HH(c, d, a, b, x[7], 16, 0xF6BB4B60); HH(b, c, d, a, x[10], 23, 0xBEBFBC70);
    HH(a, b, c, d, x[13], 4, 0x289B7EC6); HH(d, a, b, c, x[0], 11, 0xEAA127FA);
    HH(c, d, a, b, x[3], 16, 0xD4EF3085); HH(b, c, d, a, x[6], 23, 0x4881D05);
    HH(a, b, c, d, x[9], 4, 0xD9D4D039); HH(d, a, b, c, x[12], 11, 0xE6DB99E5);
    HH(c, d, a, b, x[15], 16, 0x1FA27CF8); HH(b, c, d, a, x[2], 23, 0xC4AC5665);

    II(a, b, c, d, x[0], 6, 0xF4292244); II(d, a, b, c, x[7], 10, 0x432AFF97);
    II(c, d, a, b, x[14], 15, 0xAB9423A7); II(b, c, d, a, x[5], 21, 0xFC93A039);
    II(a, b, c, d, x[12], 6, 0x655B59C3); II(d, a, b, c, x[3], 10, 0x8F0CCC92);
    II(c, d, a, b, x[10], 15, 0xFFEFF47D); II(b, c, d, a, x[1], 21, 0x85845DD1);
    II(a, b, c, d, x[8], 6, 0x6FA87E4F); II(d, a, b, c, x[15], 10, 0xFE2CE6E0);
    II(c, d, a, b, x[6], 15, 0xA3014314); II(b, c, d, a, x[13], 21, 0x4E0811A1);
    II(a, b, c, d, x[4], 6, 0xF7537E82); II(d, a, b, c, x[11], 10, 0xBD3AF235);
    II(c, d, a, b, x[2], 15, 0x2AD7D2BB); II(b, c, d, a, x[9], 21, 0xEB86D391);

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

void MD5::update(const uint8_t* data, size_t length) {
    if (finalized) return;
    uint32_t i = (count[0] >> 3) & 0x3F;
    if ((count[0] += length << 3) < (length << 3)) count[1]++;
    count[1] += length >> 29;
    if (i + length >= 64) {
        memcpy(&buffer[i], data, 64 - i);
        transform(buffer, state);
        for (; length + i - 64 >= 64; data += 64, length -= 64) transform(data, state);
        i = 0;
    }
    memcpy(&buffer[i], data, length);
}

void MD5::update(const std::string& data) {
    update(reinterpret_cast<const uint8_t*>(data.c_str()), data.size());
}

void MD5::final() {
    if (finalized) return;
    uint8_t bits[8];
    for (int i = 0; i < 8; ++i) {
        bits[i] = (count[i >> 3] >> ((i & 7) << 3)) & 0xFF;
    }
    update(reinterpret_cast<const uint8_t*>("\x80"), 1);
    while (((count[0] >> 3) & 0x3F) != 56) update(reinterpret_cast<const uint8_t*>("\x00"), 1);
    update(bits, 8);
    for (int i = 0; i < 16; ++i) {
        digest[i] = (state[i >> 2] >> ((i & 3) << 3)) & 0xFF;
    }
    finalized = true;
}

std::string MD5::get_digest() {
    if (!finalized) final();
    static const char hex[] = "0123456789abcdef";
    std::string res;
    for (int i = 0; i < 16; ++i) {
        res += hex[digest[i] >> 4];
        res += hex[digest[i] & 0x0F];
    }
    return res;
}

std::string MD5::compute(const std::string& data) {
    MD5 md5(data);
    return md5.get_digest();
}

std::string MD5::compute_file(const std::string& filename) {
    MD5 md5;
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) return "";
    char buf[4096];
    while (file.read(buf, sizeof(buf))) {
        md5.update(reinterpret_cast<const uint8_t*>(buf), file.gcount());
    }
    md5.update(reinterpret_cast<const uint8_t*>(buf), file.gcount());
    file.close();
    return md5.get_digest();
}