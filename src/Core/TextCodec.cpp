#include "Core/TextCodec.h"

#include <windows.h>

namespace poq {

std::string Utf8ToGbk(const std::string& u8) {
    if (u8.empty()) return "";
    int wn = ::MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), nullptr, 0);
    if (wn <= 0) return "";
    std::wstring w(static_cast<size_t>(wn), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), &w[0], wn);
    int bn = ::WideCharToMultiByte(936, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    if (bn <= 0) return "";
    std::string out(static_cast<size_t>(bn), '\0');
    ::WideCharToMultiByte(936, 0, w.data(), wn, &out[0], bn, nullptr, nullptr);
    return out;
}

std::string GbkToUtf8(const std::string& gbk) {
    // 固定 936, 不依赖系统区域(CP_ACP 在非中文系统会转乱码)
    if (gbk.empty()) return "";
    int n = ::MultiByteToWideChar(936, 0, gbk.data(), static_cast<int>(gbk.size()), nullptr, 0);
    if (n <= 0) return "";
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(936, 0, gbk.data(), static_cast<int>(gbk.size()), &w[0], n);
    int un = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
    if (un <= 0) return "";
    std::string out(static_cast<size_t>(un), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), n, &out[0], un, nullptr, nullptr);
    return out;
}

}  // namespace poq
