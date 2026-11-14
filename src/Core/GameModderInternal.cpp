#include "Core/GameModderInternal.h"
#include <windows.h>
#include <cstdio>

namespace poq::modder_detail {

std::string addr_hex8(uintptr_t addr) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%08llX", static_cast<unsigned long long>(addr));
  return buf;
}

std::string hex_bytes(const std::vector<unsigned char>& data) {
  std::string out;
  char buf[8];
  for (size_t i = 0; i < data.size(); ++i) {
    if (i) out += ' ';
    std::snprintf(buf, sizeof(buf), "%02X", data[i]);
    out += buf;
  }
  return out;
}

void append_le32(std::vector<unsigned char>& v, uint32_t x) {
  v.push_back(static_cast<unsigned char>(x & 0xFF));
  v.push_back(static_cast<unsigned char>((x >> 8) & 0xFF));
  v.push_back(static_cast<unsigned char>((x >> 16) & 0xFF));
  v.push_back(static_cast<unsigned char>((x >> 24) & 0xFF));
}

void append_cmp_esi_imm(std::vector<unsigned char>& v, uint32_t off, int32_t imm) {
  if (imm >= -128 && imm <= 127) {
    v.push_back(0x83);
    v.push_back(0xBE);
    append_le32(v, off);
    v.push_back(static_cast<unsigned char>(imm & 0xFF));
  } else {
    v.push_back(0x81);
    v.push_back(0xBE);
    append_le32(v, off);
    append_le32(v, static_cast<uint32_t>(imm));
  }
}

std::string gbk_encode_one(uint32_t cp) {
  wchar_t wbuf[2];
  int n = 0;
  if (cp > 0xFFFF) {
    uint32_t v = cp - 0x10000;
    wbuf[0] = static_cast<wchar_t>(0xD800 + (v >> 10));
    wbuf[1] = static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
    n = 2;
  } else {
    wbuf[0] = static_cast<wchar_t>(cp);
    n = 1;
  }
  char buf[8];
  int len = ::WideCharToMultiByte(936, 0, wbuf, n, buf, 8, nullptr, nullptr);
  if (len <= 0) return "?";
  return std::string(buf, len);
}

std::vector<uint32_t> utf8_to_codepoints(const std::string& s) {
  std::vector<uint32_t> out;
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      out.push_back(c);
      ++i;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
      out.push_back(((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F));
      i += 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
      out.push_back(((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(s[i + 2]) & 0x3F));
      i += 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
      uint32_t cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                    ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(s[i + 3]) & 0x3F);
      if (cp >= 0x10000 && cp <= 0x10FFFF) out.push_back(cp);
      i += 4;
    } else {
      ++i;
    }
  }
  return out;
}

}  // namespace poq::modder_detail
