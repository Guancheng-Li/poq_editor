// GmCmd —— 编码/字符串/正则常量与通用字节工具。
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <utility>
#include "Core/GmCmdInternal.h"

namespace poq::gm_cmd::detail {

// ---------------------------------------------------------------------------
// 常量(对齐 gm_cmd.py)
// ---------------------------------------------------------------------------
constexpr uintptr_t kOffMoney = 0x54;  // 玩家对象 +0x54 = 金钱镜像(未用到)

const char kMarkExits[] = "这里的出口有";
const char kMarkObjs[] = "的人和物品有";
const char kObjIdPrefix[] = "(16777";  // 对象 ID 前缀字节(0x1000000 起)

// 1.30 屏幕输出重组(对齐 gm_cmd.py capture_130_text 的正则,std::regex 默认 ECMAScript)。
const std::regex& re_scene() {
  static const std::regex re(R"(^(.{2,20})\s*\(([^()\s:]+):(\d+)\)\s*$)");
  return re;
}
const std::regex& re_exit() {
  static const std::regex re(R"(^\t?(\w+)\s*:\s*(.+?)<(\d+)>(\s*<\d+>)?\s*$)");
  return re;
}
const std::regex& re_obj_head() {
  static const std::regex re(R"(这里的人和物品有\s*:?\s*\((\d+)\))");
  return re;
}
const std::regex& re_obj() {
  static const std::regex re(R"(^\t?(.+?)\s*\(([^()\s]+)\)\s*$)");
  return re;
}
// objlist 条目行(对齐 build_objdb)。
const std::regex& re_obj_entry() {
  static const std::regex re(R"(([^\n\t:()]+?)\(([^()\n:]*)\)\s*:\s*([^()\n]*?)\((\d+)\))");
  return re;
}
// look 统计(对齐 _look_stats): "这里的人和物品有 (N)" 之后的 "名字 (别名)" 行。
const std::regex& re_look_count() {
  static const std::regex re(std::string(kMarkObjs) + R"(\s*:?\s*\((\d+)\))");
  return re;
}
const std::regex& re_look_obj() {
  static const std::regex re(R"(^[^\s()]+\s*\([^()\s]+\)$)");
  return re;
}

// ---------------------------------------------------------------------------
// 小工具(模块自包含)
// ---------------------------------------------------------------------------
std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string to_lower(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// CP936(GBK) -> UTF-8,失败返回空串。
std::string gbk_to_utf8(const char* bytes, size_t n) {
  if (n == 0) return "";
  int wlen = ::MultiByteToWideChar(936, 0, bytes, static_cast<int>(n), nullptr, 0);
  if (wlen <= 0) return "";
  std::wstring w(static_cast<size_t>(wlen), L'\0');
  ::MultiByteToWideChar(936, 0, bytes, static_cast<int>(n), &w[0], wlen);
  int ulen = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
  if (ulen <= 0) return "";
  std::string out(static_cast<size_t>(ulen), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, &out[0], ulen, nullptr, nullptr);
  return out;
}

// UTF-8 -> CP936(GBK),失败返回空串。
std::string utf8_to_gbk(const std::string& s) {
  if (s.empty()) return "";
  int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (wlen <= 0) return "";
  std::wstring w(static_cast<size_t>(wlen), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], wlen);
  int blen = ::WideCharToMultiByte(936, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
  if (blen <= 0) return "";
  std::string out(static_cast<size_t>(blen), '\0');
  ::WideCharToMultiByte(936, 0, w.data(), wlen, &out[0], blen, nullptr, nullptr);
  return out;
}

// GBK(含替换) -> UTF-8,供 wizard.lst 等宽松解码用。
std::string gbk_to_utf8_replace(const std::string& s) {
  if (s.empty()) return "";
  int wlen = ::MultiByteToWideChar(936, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (wlen <= 0) return s;  // 完全无法解码:原样返回
  std::wstring w(static_cast<size_t>(wlen), L'\0');
  ::MultiByteToWideChar(936, 0, s.data(), static_cast<int>(s.size()), &w[0], wlen);
  int ulen = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
  if (ulen <= 0) return s;
  std::string out(static_cast<size_t>(ulen), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, &out[0], ulen, nullptr, nullptr);
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

void append_le32(std::vector<unsigned char>& v, uint32_t x) {
  v.push_back(static_cast<unsigned char>(x & 0xFF));
  v.push_back(static_cast<unsigned char>((x >> 8) & 0xFF));
  v.push_back(static_cast<unsigned char>((x >> 16) & 0xFF));
  v.push_back(static_cast<unsigned char>((x >> 24) & 0xFF));
}

bool is_process_alive(unsigned int pid) {
  for (const auto& p : ProcessMemory::enumerate_processes())
    if (p.pid == pid) return true;
  return false;
}

// 读 NUL 结尾 GBK 字符串(UTF-8 返回)。读不到/空返回 ""。
std::string read_gbk_ptr(ProcessMemory& mem, uintptr_t a, int n) {
  std::vector<unsigned char> b;
  try {
    b = mem.read(a, n);
  } catch (...) {
    return "";
  }
  size_t z = 0;
  while (z < b.size() && b[z] != 0) ++z;
  std::string s = gbk_to_utf8(reinterpret_cast<const char*>(b.data()), z);
  return trim(s);
}

std::vector<unsigned char> to_bytes(const std::string& s) {
  return std::vector<unsigned char>(s.begin(), s.end());
}

// 在 data 里从 off 起找 kw(字节序列),返回位置或 npos。
size_t find_bytes(const std::vector<unsigned char>& data, const std::string& kw, size_t off) {
  if (kw.empty() || off >= data.size()) return std::string::npos;
  auto it = std::search(data.begin() + static_cast<std::ptrdiff_t>(off), data.end(), kw.begin(),
                        kw.end());
  if (it == data.end()) return std::string::npos;
  return static_cast<size_t>(it - data.begin());
}

// [start, end) 内最后一个 NUL 的位置(找不到返回 end 之前的哨兵 start-1)。
size_t r_find_nul(const std::vector<unsigned char>& data, size_t start, size_t end) {
  if (end == 0 || start >= end) return start - 1;
  size_t i = end;
  while (i > start) {
    --i;
    if (data[i] == 0) return i;
  }
  return start - 1;
}

// 从 off 起第一个 NUL 的位置(找不到返回 data.size()。
size_t find_nul_from(const std::vector<unsigned char>& data, size_t off) {
  for (size_t i = off; i < data.size(); ++i)
    if (data[i] == 0) return i;
  return data.size();
}

// 按行拆分(处理 \r\n / \r / \n)。
std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\r') {
      out.push_back(cur);
      cur.clear();
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
    } else if (c == '\n') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty() || text.empty()) out.push_back(cur);
  return out;
}

// 按空白切词。
std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a < s.size()) {
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    size_t b = a;
    while (b < s.size() && s[b] != ' ' && s[b] != '\t') ++b;
    if (b > a) out.push_back(s.substr(a, b - a));
    a = b;
  }
  return out;
}
}  // namespace poq::gm_cmd::detail
