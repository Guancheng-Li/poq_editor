#include "Native/ProcessMemoryDetail.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace poq::mem_detail {

HandleGuard::HandleGuard(HANDLE h) : h_(h) {}
HandleGuard::~HandleGuard() {
  if (h_ && h_ != INVALID_HANDLE_VALUE) ::CloseHandle(h_);
}

std::string format_win32_error(DWORD err) {
  char* buf = nullptr;
  DWORD n = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&buf), 0,
      nullptr);
  std::string msg =
      (buf && n > 0) ? std::string(buf, n) : std::string("error ") + std::to_string(err);
  if (buf) ::LocalFree(buf);
  while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' '))
    msg.pop_back();
  return msg;
}

std::string addr_hex(uintptr_t addr) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%08llX", static_cast<unsigned long long>(addr));
  return buf;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string to_lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return "";
  int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                  nullptr, nullptr);
  if (len <= 0) return "";
  std::string out(static_cast<size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], len, nullptr,
                        nullptr);
  return out;
}

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return L"";
  int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (wlen <= 0) return L"";
  std::wstring out(static_cast<size_t>(wlen), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], wlen);
  return out;
}

std::optional<std::string> decode_bytes(const std::string& bytes, UINT cp) {
  if (bytes.empty()) return std::nullopt;
  int wlen = ::MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
  if (wlen <= 0) return std::nullopt;
  std::vector<wchar_t> wbuf(static_cast<size_t>(wlen));
  ::MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), wbuf.data(), wlen);
  int ulen = ::WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), wlen, nullptr, 0, nullptr, nullptr);
  if (ulen <= 0) return std::nullopt;
  std::string out(static_cast<size_t>(ulen), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), wlen, &out[0], ulen, nullptr, nullptr);
  return out;
}

void append_le32(std::vector<unsigned char>& v, uint32_t x) {
  v.push_back(static_cast<unsigned char>(x & 0xFF));
  v.push_back(static_cast<unsigned char>((x >> 8) & 0xFF));
  v.push_back(static_cast<unsigned char>((x >> 16) & 0xFF));
  v.push_back(static_cast<unsigned char>((x >> 24) & 0xFF));
}

}  // namespace poq::mem_detail
