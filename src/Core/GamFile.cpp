#include "GamFile.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "Core/GamFileInternal.h"

namespace poq {

// ---------------------------------------------------------------------------
// 基础字节工具(与 python gamkit 一致)
// ---------------------------------------------------------------------------
static const uint8_t kACH[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
                               'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
                               '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '_'};
static bool is_alias_ch(uint8_t b) {
  for (size_t i = 0; i < sizeof(kACH); ++i)
    if (kACH[i] == b) return true;
  return false;
}

uint32_t GamFile::x59_dw(const std::vector<uint8_t>& b, size_t pos, uint8_t k0) {
  if (pos + 4 > b.size()) return 0xFFFFFFFFu;
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= (static_cast<uint32_t>((b[pos + i] ^ static_cast<uint8_t>(k0 + i)) & 0xFF) << (8 * i));
  }
  return v;
}

static uint32_t d32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static void put_enc_le32(std::vector<uint8_t>& c, size_t off, uint32_t enc) {
  c[off] = static_cast<uint8_t>(enc & 0xFF);
  c[off + 1] = static_cast<uint8_t>((enc >> 8) & 0xFF);
  c[off + 2] = static_cast<uint8_t>((enc >> 16) & 0xFF);
  c[off + 3] = static_cast<uint8_t>((enc >> 24) & 0xFF);
}

static void x59_range(const std::vector<uint8_t>& b, size_t pos, size_t n, uint8_t k0,
                      std::vector<uint8_t>& out) {
  out.resize(n);
  for (size_t i = 0; i < n; ++i)
    out[i] = static_cast<uint8_t>(b[pos + i] ^ static_cast<uint8_t>(k0 + i));
}

uint32_t GamFile::dw_at(const std::vector<uint8_t>& c, size_t off) {
  if (off + 4 > c.size()) return 0xFFFFFFFFu;
  return d32_le(&c[off]) ^ 0xAAABACADu;
}

bool GamFile::dec_str(const std::vector<uint8_t>& c, size_t off, std::string& out, size_t& used) {
  uint32_t slen = dw_at(c, off);
  if (slen < 2 || slen > 0x2000 || off + 4 + slen > c.size()) return false;
  size_t strlen = slen - 1;
  std::vector<uint8_t> raw(strlen);
  for (size_t i = 0; i < strlen; ++i)
    raw[i] =
        static_cast<uint8_t>(c[off + 4 + i] ^ static_cast<uint8_t>((strlen - 0x56 - i) & 0xFF));
  out.assign(raw.begin(), raw.end());
  used = 4 + slen;
  return true;
}

static uint32_t raw_enc_dw(uint32_t v) {  // 把 dword 值编码(嵌套 ^KEY)
  return v ^ 0xAAABACADu;
}

// ---------------------------------------------------------------------------
// 别名 / 记录解析
// ---------------------------------------------------------------------------
namespace {
/// 在内容里找"第一个 3..48 位全小写别名"标准字符串; 返回别名 + aend。
bool find_alias(const std::vector<uint8_t>& c, std::string& alias, size_t& aend) {
  size_t n = c.size();
  size_t hi = n > 0x200 ? 0x200 : n;
  for (size_t off = 0x04; off + 8 < hi; ++off) {
    uint32_t slen = GamFile::dw_at(c, off);
    if (slen < 3 || slen > 49) continue;
    size_t strlen = slen - 1;
    if (off + 4 + strlen > n) continue;
    std::string s;
    bool ok = true;
    size_t nz = 0;
    for (size_t i = 0; i < strlen; ++i) {
      uint8_t b = c[off + 4 + i] ^ static_cast<uint8_t>((strlen - 0x56 - i) & 0xFF);
      if (b == 0) break;
      if (!is_alias_ch(b)) {
        ok = false;
        break;
      }
      s.push_back(static_cast<char>(b));
      ++nz;
    }
    if (ok && nz >= 3) {
      alias = s;
      aend = off + 4 + slen;  // 别名尾
      return true;
    }
  }
  return false;
}
}  // namespace

static bool make_item(size_t pos, const std::vector<uint8_t>& buf, GamItem& it) {
  uint32_t L = GamFile::x59_dw(buf, pos);
  if (L < 0x100 || L > 0x6000 || pos + 4 + L + 8 > buf.size()) return false;
  uint32_t cnt = GamFile::x59_dw(buf, pos + 4 + L);
  uint32_t total = GamFile::x59_dw(buf, pos + 4 + L + 4);
  if (cnt != 0 || total != L + 12) return false;
  x59_range(buf, pos + 4, L, 0x59, it.content);
  it.len = L;
  it.pos = pos;
  it.rec_end = pos + 4 + L + 8;
  it.dirty = false;
  size_t aend = 0;
  if (!find_alias(it.content, it.alias, aend)) {
    it.alias = "?";
    aend = 0x1C + L + 1;
  }
  it.aend = aend;
  // 中文名: 位于 [+0x04 路径字段][+4] 处(路径字段长度可变, 决定名字偏移)
  {
    std::string pv;
    size_t pused = 0;
    size_t noff = 0xD;
    if (GamFile::dec_str(it.content, 0x4, pv, pused)) noff = 0x4 + pused + 4;
    it.name_off = noff;
    std::string nm;
    size_t nused = 0;
    if (GamFile::dec_str(it.content, noff, nm, nused)) it.name = nm;
  }
  it.refresh_filter_names();
  it.parse_affixes();
  return true;
}

void GamItem::refresh_filter_names() {
  auto low = [](const std::string& x) {
    std::string r = x;
    for (char& c : r)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
  };
  alias_low = low(alias);
  disp_low = (name.empty() || alias == "?") ? alias_low : low(name + "(" + alias + ")");
}

// ---------------------------------------------------------------------------
// 固定字段表(相对 aend)
// ---------------------------------------------------------------------------
namespace {
struct FieldDef {
  const char* name;
  uint32_t off;
};
const FieldDef kFields[] = {
    {"lvl", 0x0C},      {"dur", 0x30},      {"durmax", 0x34},   {"f_c8", 0xC8},
    {"price", 0xCC},    {"wgt", 0xD0},      {"type", 0xD4},     {"ident", 0xE0},
    {"f_e4", 0xE4},     {"f_e8", 0xE8},     {"sockets", 0xFC},  {"f_100", 0x100},
    {"f_104", 0x104},   {"f_10c", 0x10C},   {"f_110", 0x110},   {"f_118", 0x118},
    {"f_11c", 0x11C},   {"f_124", 0x124},   {"req_lvl", 0x128}, {"req_str", 0x12C},
    {"req_agi", 0x130}, {"req_con", 0x134}, {"wx", 0x140},      {"arrows", 0x370},
};
}  // namespace

const std::vector<GamFile::FieldDesc>& GamFile::field_list() {
  static const std::vector<FieldDesc> k = {
      {"lvl", "等级"},         {"dur", "耐久"},         {"durmax", "耐久上限"},
      {"price", "价格"},       {"wgt", "重量"},         {"type", "类型"},
      {"ident", "鉴定"},       {"sockets", "孔数"},     {"req_lvl", "等级需求"},
      {"req_str", "力量需求"}, {"req_agi", "敏捷需求"}, {"req_con", "体质需求"},
      {"wx", "五行"},          {"arrows", "箭支数量"},
  };
  return k;
}

int32_t GamItem::get_field(const char* key, bool* ok) const {
  const FieldDef* def = nullptr;
  for (const auto& f : kFields)
    if (strcmp(f.name, key) == 0) {
      def = &f;
      break;
    }
  if (!def) {
    if (ok) *ok = false;
    return 0;
  }
  size_t off = aend + def->off;
  if (off + 4 > content.size()) {
    if (ok) *ok = false;
    return 0;
  }
  if (ok) *ok = true;
  uint32_t v = GamFile::dw_at(content, off);
  return static_cast<int32_t>(v);  // -1 = 空
}

bool GamItem::set_field(const char* key, int64_t v) {
  const FieldDef* def = nullptr;
  for (const auto& f : kFields)
    if (strcmp(f.name, key) == 0) {
      def = &f;
      break;
    }
  if (!def) return false;
  size_t off = aend + def->off;
  if (off + 4 > content.size()) return false;
  const uint32_t enc = raw_enc_dw(static_cast<uint32_t>(v & 0xFFFFFFFFu));
  put_enc_le32(content, off, enc);
  dirty = true;
  return true;
}

bool GamItem::parse_record(const std::vector<uint8_t>& rec, GamItem& out) {
  // 记录字节 = [len][content][cnt=0][echo=len+12], 0x59 编码
  return make_item(0, rec, out) && out.pos == 0;
}

bool GamItem::build_record(const GamItem& it, std::vector<uint8_t>& out) {
  out.clear();
  const uint32_t L = static_cast<uint32_t>(it.content.size());
  std::vector<uint8_t> lenb, ccb, cntb, totb;
  gamfile_detail::raw_x59_range(reinterpret_cast<const uint8_t*>(&L), 4, 0x59, lenb);
  gamfile_detail::raw_x59_range(it.content.data(), it.content.size(), 0x59, ccb);
  uint32_t zero = 0;
  gamfile_detail::raw_x59_range(reinterpret_cast<const uint8_t*>(&zero), 4, 0x59, cntb);
  uint32_t tot = L + 12;
  gamfile_detail::raw_x59_range(reinterpret_cast<const uint8_t*>(&tot), 4, 0x59, totb);
  out.insert(out.end(), lenb.begin(), lenb.end());
  out.insert(out.end(), ccb.begin(), ccb.end());
  out.insert(out.end(), cntb.begin(), cntb.end());
  out.insert(out.end(), totb.begin(), totb.end());
  return true;
}

// G_Info 文本(get/set/parse/build/pad_content_to)已移至 src/Core/GamFileGInfo.cpp。

bool GamItem::set_alias(const std::string& newalias) {
  // 别名字段 = 内容里第一个标准小写串; 其占位 = 4B 长度前缀 + 字符 + 结尾 null; 尾 = aend
  size_t slen = alias.size() + 1;
  if (aend < 4 + slen) return false;
  size_t off = aend - 4 - slen;
  size_t newlen = newalias.size();
  if (newlen < 1 || newlen > 0x40) return false;
  uint32_t slenv = static_cast<uint32_t>(newlen) + 1;
  uint32_t enc = slenv ^ 0xAAABACADu;
  std::vector<uint8_t> nb;
  nb.reserve(content.size() + newlen + 8);
  nb.insert(nb.end(), content.begin(), content.begin() + static_cast<ptrdiff_t>(off));
  nb.push_back(static_cast<uint8_t>(enc & 0xFF));
  nb.push_back(static_cast<uint8_t>((enc >> 8) & 0xFF));
  nb.push_back(static_cast<uint8_t>((enc >> 16) & 0xFF));
  nb.push_back(static_cast<uint8_t>((enc >> 24) & 0xFF));
  for (size_t i = 0; i < newlen; ++i)
    nb.push_back(
        static_cast<uint8_t>(newalias[i] ^ static_cast<uint8_t>((newlen - 0x56 - i) & 0xFF)));
  nb.push_back(static_cast<uint8_t>(
      0 ^ static_cast<uint8_t>((newlen - 0x56 - newlen) & 0xFF)));  // 结尾 null
  nb.insert(nb.end(), content.begin() + static_cast<ptrdiff_t>(aend), content.end());
  content = std::move(nb);
  invalidate_g_info_cache();
  alias = newalias;
  refresh_filter_names();
  aend = off + 4 + slenv;
  if (has_affixes)
    gamfile_detail::write_content_len(content);  // 词条数组随之后移, 收尾长度字段要跟着改
  dirty = true;
  return true;
}

bool GamItem::set_name(const std::string& gbkname) {
  size_t off = name_off;
  if (off == 0) return false;
  std::string oldv;
  size_t oldused = 0;
  if (!GamFile::dec_str(content, off, oldv, oldused)) return false;
  size_t newlen = gbkname.size();
  if (newlen < 1 || newlen > 0x40) return false;
  uint32_t slenv = static_cast<uint32_t>(newlen) + 1;
  uint32_t enc = slenv ^ 0xAAABACADu;
  std::vector<uint8_t> nb;
  nb.reserve(content.size() + newlen + 8);
  nb.insert(nb.end(), content.begin(), content.begin() + static_cast<ptrdiff_t>(off));
  nb.push_back(static_cast<uint8_t>(enc & 0xFF));
  nb.push_back(static_cast<uint8_t>((enc >> 8) & 0xFF));
  nb.push_back(static_cast<uint8_t>((enc >> 16) & 0xFF));
  nb.push_back(static_cast<uint8_t>((enc >> 24) & 0xFF));
  for (size_t i = 0; i < newlen; ++i)
    nb.push_back(
        static_cast<uint8_t>(gbkname[i] ^ static_cast<uint8_t>((newlen - 0x56 - i) & 0xFF)));
  nb.push_back(static_cast<uint8_t>(0 ^ static_cast<uint8_t>((newlen - 0x56 - newlen) & 0xFF)));
  nb.insert(nb.end(), content.begin() + static_cast<ptrdiff_t>(off + oldused), content.end());
  long long delta = static_cast<long long>(4 + newlen + 1) - static_cast<long long>(oldused);
  content = std::move(nb);
  invalidate_g_info_cache();
  name = gbkname;
  refresh_filter_names();
  aend = static_cast<size_t>(static_cast<long long>(aend) +
                             delta);  // 名在别名之前, 长度变化会平移 aend
  if (has_affixes)
    gamfile_detail::write_content_len(content);  // 词条数组随之后移, 收尾长度字段要跟着改
  dirty = true;
  return true;
}

// 词条数组(parse_affixes/Sync/set_affixes)见 GamFileAffix.cpp; G_Info 文本见 GamFileGInfo.cpp。

}  // namespace poq
