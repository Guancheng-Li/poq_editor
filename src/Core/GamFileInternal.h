#pragma once

#include <cstdio>
#include <string>
#include <vector>
#include "Core/GamFile.h"

namespace poq::gamfile_detail {

FILE* f_open_w(const std::string& path, const wchar_t* mode);
void raw_x59_range(const uint8_t* src, size_t n, uint8_t k0, std::vector<uint8_t>& out);
bool parse_loaded(GamFile& f, const std::string& path_, std::string& err);
/// 内容末尾 4 字节 = 内容长度(词条数组区收尾); 变长后需同步。
void write_content_len(std::vector<uint8_t>& content);

}  // namespace poq::gamfile_detail
