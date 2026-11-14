#pragma once
// GameModder 子模块共享小工具(注入/读写/词条池).

#include <cstdint>
#include <string>
#include <vector>

namespace poq::modder_detail {

std::string addr_hex8(uintptr_t addr);
std::string hex_bytes(const std::vector<unsigned char>& data);
void append_le32(std::vector<unsigned char>& v, uint32_t x);
void append_cmp_esi_imm(std::vector<unsigned char>& v, uint32_t off, int32_t imm);
std::string gbk_encode_one(uint32_t cp);
std::vector<uint32_t> utf8_to_codepoints(const std::string& s);

}  // namespace poq::modder_detail
