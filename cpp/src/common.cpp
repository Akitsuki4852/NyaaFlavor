#include "common.h"

#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <stdexcept>

namespace nyaa {

namespace {
// 文件类错误最常见的几类，直接翻成用户能判断的中文；其余保留系统错误码。
std::string win32_error_text(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND: return "文件不存在";
        case ERROR_PATH_NOT_FOUND: return "路径不存在";
        case ERROR_ACCESS_DENIED: return "拒绝访问";
        case ERROR_SHARING_VIOLATION: return "文件被其他程序占用";
        case ERROR_DISK_FULL: return "磁盘空间不足";
        case ERROR_WRITE_PROTECT: return "磁盘写保护";
        default: return "系统错误 " + std::to_string(code);
    }
}

struct NativeFile {
    HANDLE handle;
    NativeFile(const std::string& path, DWORD access, DWORD creation, DWORD flags) {
        handle = CreateFileW(u8_to_w(path).c_str(), access, FILE_SHARE_READ, nullptr, creation, flags, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            throw std::runtime_error("无法打开文件（" + win32_error_text(error) + "）：" + path);
        }
    }
    ~NativeFile() { CloseHandle(handle); }
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
};
}

std::string read_file(const std::string& path) {
    NativeFile file(path, GENERIC_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle, &size) || size.QuadPart < 0)
        throw std::runtime_error("无法读取文件长度（" + win32_error_text(GetLastError()) + "）：" + path);
    std::string bytes;
    if (static_cast<uint64_t>(size.QuadPart) > bytes.max_size()) throw std::runtime_error("文件过大: " + path);
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1024 * 1024));
        DWORD got = 0;
        if (!ReadFile(file.handle, bytes.data() + offset, chunk, &got, nullptr) || !got)
            throw std::runtime_error("文件读取失败（" + win32_error_text(GetLastError()) + "）：" + path);
        offset += got;
    }
    return bytes;
}

void write_file(const std::string& path, const std::string& bytes) {
    NativeFile file(path, GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file.handle, bytes.data() + offset, chunk, &written, nullptr) || !written)
            throw std::runtime_error("文件写入失败（" + win32_error_text(GetLastError()) + "）：" + path);
        offset += written;
    }
}

TemporaryDirectory::TemporaryDirectory() {
    static std::atomic<unsigned long> sequence{0};
    const auto parent = std::filesystem::temp_directory_path();
    for (int i = 0; i < 100; ++i) {
        auto candidate = parent / (L"NyaaFlavor-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence));
        if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); return; }
    }
    throw std::runtime_error("无法创建临时文件夹");
}
TemporaryDirectory::~TemporaryDirectory() {
    // Only a directory successfully created by this object can reach this cleanup.
    if (!path.empty()) { std::error_code ec; std::filesystem::remove_all(path, ec); }
}

namespace {

// cp1252 的 0x80..0x9F 区（与 Python 的 cp1252 编解码表一致；
// 未定义的 5 个字节 0x81/0x8d/0x8f/0x90/0x9d 映射到对应的 C1 控制码）
constexpr uint32_t kHigh[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

}  // namespace

std::optional<std::vector<uint32_t>> utf8_decode_strict(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const auto b0 = static_cast<uint8_t>(s[i]);
        if (b0 < 0x80) {
            out.push_back(b0);
            ++i;
            continue;
        }
        int extra = 0;
        uint32_t cp = 0;
        if ((b0 & 0xE0) == 0xC0) {
            extra = 1;
            cp = b0 & 0x1F;
        } else if ((b0 & 0xF0) == 0xE0) {
            extra = 2;
            cp = b0 & 0x0F;
        } else if ((b0 & 0xF8) == 0xF0) {
            extra = 3;
            cp = b0 & 0x07;
        } else {
            return std::nullopt;
        }
        if (i + extra >= n) return std::nullopt;
        for (int k = 1; k <= extra; ++k) {
            const auto b = static_cast<uint8_t>(s[i + k]);
            if ((b & 0xC0) != 0x80) return std::nullopt;
            cp = (cp << 6) | (b & 0x3F);
        }
        // 过长编码 / 代理区 / 越界
        static const uint32_t kMin[4] = {0, 0x80, 0x800, 0x10000};
        if (cp < kMin[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return std::nullopt;
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

std::vector<uint32_t> utf8_decode(const std::string& s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const auto b0 = static_cast<uint8_t>(s[i]);
        if (b0 < 0x80) {
            out.push_back(b0);
            ++i;
            continue;
        }
        int extra = ((b0 & 0xE0) == 0xC0) ? 1 : ((b0 & 0xF0) == 0xE0) ? 2 : ((b0 & 0xF8) == 0xF0) ? 3 : -1;
        if (extra < 0 || i + extra >= n) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        uint32_t cp = (extra == 1) ? (b0 & 0x1F) : (extra == 2) ? (b0 & 0x0F) : (b0 & 0x07);
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const auto b = static_cast<uint8_t>(s[i + k]);
            if ((b & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

std::string utf8_encode(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return s;
}

std::string utf8_encode(const std::vector<uint32_t>& cps) {
    std::string s;
    for (auto cp : cps) s += utf8_encode(cp);
    return s;
}

std::wstring u8_to_w(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string w_to_u8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

uint32_t cp1252_decode_byte(uint8_t b) {
    if (b < 0x80) return b;
    if (b < 0xA0) return kHigh[b - 0x80];
    return b;
}

bool cp1252_encode_char(uint32_t cp, char& out) {
    if (cp < 0x80) {
        out = static_cast<char>(cp);
        return true;
    }
    if (cp >= 0xA0 && cp <= 0xFF) {
        out = static_cast<char>(cp);
        return true;
    }
    for (int i = 0; i < 32; ++i) {
        if (kHigh[i] == cp) {
            out = static_cast<char>(0x80 + i);
            return true;
        }
    }
    return false;
}

bool cp1252_can_encode(uint32_t cp) {
    char tmp;
    return cp1252_encode_char(cp, tmp);
}

bool is_cp1252_encodable(const std::string& utf8) {
    for (auto cp : utf8_decode(utf8)) {
        if (!cp1252_can_encode(cp)) return false;
    }
    return true;
}

std::string cp1252_encode_str(const std::string& utf8) {
    std::string out;
    for (auto cp : utf8_decode(utf8)) {
        char c;
        if (cp1252_encode_char(cp, c)) {
            out.push_back(c);
        } else {
            out.push_back('?');  // errors='replace'
        }
    }
    return out;
}

bool is_ascii(const std::string& utf8) {
    for (auto cp : utf8_decode(utf8))
        if (cp > 127) return false;
    return true;
}

bool py_isspace(uint32_t cp) {
    if (cp >= 0x09 && cp <= 0x0D) return true;
    if (cp >= 0x1C && cp <= 0x1F) return true;
    if (cp == 0x20 || cp == 0x85 || cp == 0xA0) return true;
    if (cp == 0x1680) return true;
    if (cp >= 0x2000 && cp <= 0x200A) return true;
    if (cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000) return true;
    return false;
}

bool py_isdigit(uint32_t cp) {
    if (cp >= '0' && cp <= '9') return true;
    // 常见 Unicode 十进制数字
    if (cp >= 0x0660 && cp <= 0x0669) return true;
    if (cp >= 0x06F0 && cp <= 0x06F9) return true;
    if (cp >= 0xFF10 && cp <= 0xFF19) return true;
    return false;
}

std::string py_strip(const std::string& utf8) {
    std::vector<uint32_t> cps = utf8_decode(utf8);
    size_t a = 0, b = cps.size();
    while (a < b && py_isspace(cps[a])) ++a;
    while (b > a && py_isspace(cps[b - 1])) --b;
    return utf8_encode(std::vector<uint32_t>(cps.begin() + a, cps.begin() + b));
}

std::string py_lower(const std::string& utf8) {
    std::string out;
    for (auto cp : utf8_decode(utf8)) {
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 32;
        out += utf8_encode(cp);
    }
    return out;
}

}  // namespace nyaa
