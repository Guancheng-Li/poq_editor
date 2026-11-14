#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include "Core/GamFile.h"
#include "Core/GamFileInternal.h"
#include "Core/ItemData.h"

namespace poq {

static std::wstring utf8_to_wide_path(const std::string& s) {
  const int len = static_cast<int>(s.size());
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), len, nullptr, 0);
  if (n <= 0) return L"";
  std::wstring w(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), len, &w[0], n);
  return w;
}

FILE* gamfile_detail::f_open_w(const std::string& path, const wchar_t* mode) {
  return _wfopen(utf8_to_wide_path(path).c_str(), mode);
}

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

static uint32_t d32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static void x59_range(const std::vector<uint8_t>& b, size_t pos, size_t n, uint8_t k0,
                      std::vector<uint8_t>& out) {
  out.resize(n);
  for (size_t i = 0; i < n; ++i)
    out[i] = static_cast<uint8_t>(b[pos + i] ^ static_cast<uint8_t>(k0 + i));
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

namespace {
struct WalkCtx {
  GamFile& f;
  GamContainer* cur = nullptr;
  WalkCtx(GamFile& f_) : f(f_) {}
};

size_t walkSubtree(const std::vector<uint8_t>& buf, GamFile& f, GamContainer& cur, size_t p,
                   int depth, int depth_limit, size_t active_cnt_off) {
  if (p + 12 > buf.size()) return 1;
  uint32_t L = GamFile::x59_dw(buf, p);
  if (L < 0x100 || L > 0x6000 || p + 4 + L + 8 > buf.size()) return 1;
  uint32_t cnt = GamFile::x59_dw(buf, p + 4 + L);
  uint32_t total = GamFile::x59_dw(buf, p + 4 + L + 4);
  std::vector<uint8_t> cc;
  x59_range(buf, p + 4, L, 0x59, cc);
  std::string a;
  size_t aend = 0;
  if (cnt == 0 && total == L + 12) {
    GamItem it;
    if (!make_item(p, buf, it)) return 4 + L + 8;
    it.cnt_off = active_cnt_off;  // 所属组的 count 偏移(删除时递减)
    int idx = static_cast<int>(f.items.size());
    f.items.push_back(std::move(it));
    cur.item_idx.push_back(idx);
    cur.insert_pos = p + 4 + L + 8;
    cur.insert_cnt_off = active_cnt_off;
    // 按组统计(最终锚点取物品最多的组; 避免锚到 blob/紧凑项)
    auto& gs = cur.gstat[active_cnt_off];
    gs.first += 1;
    gs.second = p + 4 + L + 8;
    return 4 + L + 8;
  }
  if (find_alias(cc, a, aend) && cnt >= 1 && cnt <= 600 && depth < depth_limit) {
    size_t end = p + 4 + L + 4;
    unsigned guard = 0;
    uint32_t consumed = 0;
    // 递归子节点: 直接步进, 失配则向前容差扫描(紧凑记录有尾部差异)
    while (consumed < cnt && guard < 800000u) {
      ++guard;
      size_t base = end;
      size_t step = walkSubtree(buf, f, cur, end, depth + 1, depth_limit, p + 4 + L);
      if (step > 1) {
        end += step;
        ++consumed;
        continue;
      }
      // 向前扫描直到找到可解析节点(记为一个子节点); 窗口对齐 python(0x300)
      size_t q = base;
      size_t steps = 0;
      bool found = false;
      while (steps < 0x300u) {
        ++steps;
        ++q;
        if (q + 16 > buf.size()) break;
        size_t s2 = walkSubtree(buf, f, cur, q, depth + 1, depth_limit, p + 4 + L);
        if (s2 > 1) {
          end = q + s2;
          found = true;
          break;
        }
      }
      ++consumed;
      if (!found) {
        end = base + 1;
      }
    }
    return end - p;
  }
  if (!a.empty()) return 4 + L + 8;  // obj 紧凑对象
  return 1;                          // 未知数据, 逐字节前移(对齐 python)
}
}  // namespace

bool gamfile_detail::parse_loaded(GamFile& f, const std::string& path_, std::string& err) {
  if (f.buf.size() < 0x4C + 4 || memcmp(f.buf.data(), "EASYRPG", 7) != 0) {
    err = "不是有效的 GAM 存档(缺少 EASYRPG 头)";
    return false;
  }
  f.path = path_;
  f.rec = GamFile::x59_dw(f.buf, 0x4C);
  f.dataoff = GamFile::x59_dw(f.buf, 0x50, 0x5D);
  // 40B 头: 0x54 起连续 0x59+i
  if (0x54 + 40 <= f.buf.size()) {
    std::vector<uint8_t> h40;
    x59_range(f.buf, 0x54, 40, 0x59, h40);
    f.cont_len = d32_le(&h40[8]);
  }
  if (f.dataoff < 0x4C || f.dataoff + 0x100 > f.buf.size()) {
    err = "数据区偏移异常";
    return false;
  }

  // ---- 遍历 body: 容器=顶层组节点, 走进子树收割物品(场景级分组) ----
  size_t pos = f.dataoff;
  GamContainer first;
  first.start = f.dataoff;
  first.alias = "@当前地图";
  first.first = true;
  f.containers.push_back(first);
  GamContainer* cur = &f.containers.back();
  unsigned guard = 0;
  while (pos + 16 < f.buf.size() && guard < 3000000u) {
    ++guard;
    uint32_t L = GamFile::x59_dw(f.buf, pos);
    if (L >= 0x100 && L <= 0x6000 && pos + 4 + L + 8 <= f.buf.size()) {
      uint32_t cnt = GamFile::x59_dw(f.buf, pos + 4 + L);
      uint32_t total = GamFile::x59_dw(f.buf, pos + 4 + L + 4);
      std::vector<uint8_t> cc;
      x59_range(f.buf, pos + 4, L, 0x59, cc);
      std::string a;
      size_t aend = 0;
      if (find_alias(cc, a, aend) && cnt >= 1 && cnt <= 600 && !(cnt == 0 && total == L + 12)) {
        // 顶层容器根: 走进整棵树, 物品归到本容器
        f.containers.back().end = pos;
        GamContainer nc;
        nc.start = pos;
        nc.alias = a;
        nc.insert_cnt_off = pos + 4 + L;
        f.containers.push_back(nc);
        cur = &f.containers.back();
        pos += walkSubtree(f.buf, f, *cur, pos, 0, 8, cur->insert_cnt_off);
        continue;
      }
    }
    // 平铺节点 -> 当前容器
    pos += walkSubtree(f.buf, f, *cur, pos, 0, 8, cur->insert_cnt_off);
  }
  f.containers.back().end = f.buf.size();
  size_t n = f.buf.size();
  for (auto& c : f.containers) {
    // 插入锚点 = 物品数最多的组(真正的物品组; 排除 blob/紧凑项误锚)
    if (!c.gstat.empty()) {
      size_t bo = 0;
      int bc = -1;
      size_t be = 0;
      for (auto& kv : c.gstat)
        if (kv.second.first > bc) {
          bc = kv.second.first;
          bo = kv.first;
          be = kv.second.second;
        }
      if (bc > 0 && bo) {
        c.insert_cnt_off = bo;
        c.insert_pos = be;
      }
    }
    if (c.end == n) c.last = true;
    if (c.start == f.dataoff) c.first = true;
  }
  return true;
}

bool GamFile::load(const std::string& path_, GamFile& f, std::string& err) {
  f = GamFile();
  FILE* fp = gamfile_detail::f_open_w(path_, L"rb");
  if (!fp) {
    err = "无法打开文件: " + path_;
    return false;
  }
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz < 0x100 || sz > 0x40000000L) {
    fclose(fp);
    err = "文件尺寸异常";
    return false;
  }
  f.buf.resize(static_cast<size_t>(sz));
  if (fread(f.buf.data(), 1, static_cast<size_t>(sz), fp) != static_cast<size_t>(sz)) {
    fclose(fp);
    err = "文件读取失败";
    return false;
  }
  fclose(fp);

  return gamfile_detail::parse_loaded(f, path_, err);
}

void gamfile_detail::raw_x59_range(const uint8_t* src, size_t n, uint8_t k0,
                                   std::vector<uint8_t>& out) {
  out.resize(n);
  for (size_t i = 0; i < n; ++i)
    out[i] = src[i] ^ static_cast<uint8_t>(k0 + static_cast<uint8_t>(i));
}

}  // namespace poq
