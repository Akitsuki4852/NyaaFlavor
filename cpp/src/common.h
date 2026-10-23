#pragma once
// 基础工具：UTF-8/UTF-16 互转、小端读写、文本编码辅助。
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nyaa {

// Own a uniquely created temporary directory; never use a source-adjacent MIDI as scratch space.
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory();
    ~TemporaryDirectory();
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
};

// C++20 的 path::u8string() 返回 std::u8string，统一转成 std::string
inline std::string path_to_u8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// ---- UTF-8 <-> 码点 ---------------------------------------------------------
// 严格解码（非法序列返回 nullopt）
std::optional<std::vector<uint32_t>> utf8_decode_strict(const std::string& s);
// 宽松解码（非法处给 U+FFFD）
std::vector<uint32_t> utf8_decode(const std::string& s);
std::string utf8_encode(const std::vector<uint32_t>& cps);
std::string utf8_encode(uint32_t cp);

// ---- Windows 边界 ----------------------------------------------------------
std::wstring u8_to_w(const std::string& s);
std::string w_to_u8(const std::wstring& s);

// Binary I/O through Windows Unicode APIs, avoiding iostream locale machinery.
std::string read_file(const std::string& path);
void write_file(const std::string& path, const std::string& bytes);

// ---- 小端读写 --------------------------------------------------------------
inline uint16_t rd_u16(const std::string& b, size_t o) {
    return static_cast<uint16_t>((uint8_t)b[o] | ((uint8_t)b[o + 1] << 8));
}
inline int16_t rd_i16(const std::string& b, size_t o) {
    return static_cast<int16_t>(rd_u16(b, o));
}
inline int32_t rd_i32(const std::string& b, size_t o) {
    return static_cast<int32_t>((uint32_t)(uint8_t)b[o] | ((uint32_t)(uint8_t)b[o + 1] << 8) |
                                ((uint32_t)(uint8_t)b[o + 2] << 16) | ((uint32_t)(uint8_t)b[o + 3] << 24));
}
inline void wr_u8(std::string& b, uint8_t v) { b.push_back(static_cast<char>(v)); }
inline void wr_u16(std::string& b, uint16_t v) {
    b.push_back(static_cast<char>(v & 0xFF));
    b.push_back(static_cast<char>(v >> 8));
}
inline void wr_i16(std::string& b, int16_t v) { wr_u16(b, static_cast<uint16_t>(v)); }
inline void wr_i32(std::string& b, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
}

// Python 的 floor 除法
inline int floordiv(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
inline int pymod(int a, int b) { return a - floordiv(a, b) * b; }

// ---- cp1252 ----------------------------------------------------------------
bool cp1252_can_encode(uint32_t cp);
bool cp1252_encode_char(uint32_t cp, char& out);
uint32_t cp1252_decode_byte(uint8_t b);
bool is_cp1252_encodable(const std::string& utf8);
std::string cp1252_encode_str(const std::string& utf8);  // 未定义字符 → '?'

// ---- Python 语义辅助 -------------------------------------------------------
bool is_ascii(const std::string& utf8);
bool py_isspace(uint32_t cp);
bool py_isdigit(uint32_t cp);
std::string py_strip(const std::string& utf8);
std::string py_lower(const std::string& utf8);
// Python 的 round()：四舍六入五成双（IEEE 默认舍入模式）
inline double py_round(double x) { return std::nearbyint(x); }
inline int py_round_int(double x) { return static_cast<int>(py_round(x)); }

}  // namespace nyaa
