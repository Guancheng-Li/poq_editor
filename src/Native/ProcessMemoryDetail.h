#pragma once
// ProcessMemory.cpp 内部 Win32/字符串辅助(多 TU 共享, 不进入公开 API)。

#include <windows.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace poq::mem_detail {

class HandleGuard {
 public:
  explicit HandleGuard(HANDLE h);
  ~HandleGuard();
  HandleGuard(const HandleGuard&) = delete;
  HandleGuard& operator=(const HandleGuard&) = delete;
  HANDLE get() const { return h_; }

 private:
  HANDLE h_;
};

std::string format_win32_error(DWORD err);
std::string addr_hex(uintptr_t addr);
std::string trim(const std::string& s);
std::string to_lower(const std::string& s);
std::string wide_to_utf8(const std::wstring& w);
std::wstring utf8_to_wide(const std::string& s);
std::optional<std::string> decode_bytes(const std::string& bytes, UINT cp);
void append_le32(std::vector<unsigned char>& v, uint32_t x);

}  // namespace poq::mem_detail
