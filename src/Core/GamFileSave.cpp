#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include "Core/GamFile.h"
#include "Core/GamFileInternal.h"

namespace poq {

bool GamFile::save(GamFile& f, const std::string& out_path,
                   const std::vector<std::pair<int, std::vector<uint8_t>>>& added,
                   std::string& err) {
  // 内容一旦比原来短, 游戏按固定偏移读的字段就会错位(实测: 地上物品消失)。
  // 这里先把所有"变短的" dirty 物品补回原长度(在 G_Info 尾部补空格)。
  for (auto& it : f.items)
    if (it.dirty && !it.is_new && !it.removed && it.content.size() < it.len)
      it.pad_content_to(it.len);

  // 先判断是否有长度变化的 dirty 物品(词条改长/改短/加词条)或被删物品
  bool need_relayout = false;
  for (const auto& it : f.items)
    if (it.removed || (it.dirty && !it.is_new && it.content.size() != it.len)) {
      need_relayout = true;
      break;
    }

  std::vector<uint8_t> b = f.buf;
  if (need_relayout) {
    // 支持单/多容器: 全文件按 dirty/删除物品重排(字节整体平移), 再按"受影响容器"更新长度回显。
    // 容器尾 echo = 容器长度(干净容器); 含紧凑NPC的容器尾不是纯 echo,
    // 跳过(游戏按遍历加载不受影响)。 按 pos 升序: dirty 物品重编, removed 物品整帧丢弃
    std::vector<int> idx;
    for (size_t i = 0; i < f.items.size(); ++i)
      if ((f.items[i].dirty && !f.items[i].is_new) || f.items[i].removed)
        idx.push_back(static_cast<int>(i));
    std::sort(idx.begin(), idx.end(), [&](int a, int c) {
      return f.items[static_cast<size_t>(a)].pos < f.items[static_cast<size_t>(c)].pos;
    });
    std::vector<uint8_t> nb;
    nb.reserve(f.buf.size() + 0x100);
    nb.insert(nb.end(), f.buf.begin(), f.buf.begin() + f.dataoff);  // 保留文件头
    size_t src = f.dataoff;
    for (int i : idx) {
      const auto& it = f.items[static_cast<size_t>(i)];
      if (it.pos < src) {
        err = "重排冲突(位置异常)";
        return false;
      }
      nb.insert(nb.end(), f.buf.begin() + src, f.buf.begin() + it.pos);
      if (!it.removed) {  // 删除帧: 不加回(即丢弃)
        std::vector<uint8_t> rec;
        GamItem::build_record(it, rec);
        nb.insert(nb.end(), rec.begin(), rec.end());
      }
      src = it.pos + 4 + it.len + 8;
    }
    nb.insert(nb.end(), f.buf.begin() + src, f.buf.end());
    // 平移量: delta_at(旧位置) = 该位置之前所有变更物品的长度差之和(removed 为负)
    auto delta_at = [&](size_t old_pos) -> long long {
      long long d = 0;
      for (int i : idx) {
        const auto& it = f.items[static_cast<size_t>(i)];
        if (it.pos < old_pos) {
          if (it.removed)
            d -= static_cast<long long>(4 + it.len + 8);
          else
            d += static_cast<long long>(it.content.size()) - static_cast<long long>(it.len);
        }
      }
      return d;
    };
    auto has_change = [&](const GamContainer& c) {
      for (int i : c.item_idx)
        if (i >= 0 && static_cast<size_t>(i) < f.items.size() &&
            ((f.items[static_cast<size_t>(i)].dirty && !f.items[static_cast<size_t>(i)].is_new) ||
             f.items[static_cast<size_t>(i)].removed))
          return true;
      return false;
    };
    // 递减被删物品所属组的计数(写在平移后的位置)
    {
      std::map<size_t, int> dec;
      for (int i : idx)
        if (f.items[static_cast<size_t>(i)].removed && f.items[static_cast<size_t>(i)].cnt_off)
          dec[f.items[static_cast<size_t>(i)].cnt_off] += 1;
      for (auto& kv : dec) {
        size_t off_old = kv.first;
        size_t off_new = static_cast<size_t>(static_cast<long long>(off_old) + delta_at(off_old));
        if (off_old + 4 > f.buf.size() || off_new + 4 > nb.size()) continue;
        uint32_t c = GamFile::x59_dw(f.buf, off_old);
        uint32_t nc =
            (c >= static_cast<uint32_t>(kv.second)) ? c - static_cast<uint32_t>(kv.second) : 0;
        std::vector<uint8_t> cb;
        gamfile_detail::raw_x59_range(reinterpret_cast<const uint8_t*>(&nc), 4, 0x59, cb);
        memcpy(&nb[off_new], cb.data(), 4);
      }
    }
    // 更新每个受影响容器的尾部长度回显(仅当其旧尾 echo==旧容器长)
    bool first_dirty = false;
    for (auto& c : f.containers) {
      if (!has_change(c)) continue;
      if (c.start == f.dataoff && c.end == f.dataoff + f.cont_len) first_dirty = true;
      if (c.end < 4 || c.end > f.buf.size()) continue;
      uint32_t old_len = static_cast<uint32_t>(c.end - c.start);
      std::vector<uint8_t> tb(f.buf.begin() + static_cast<ptrdiff_t>(c.end) - 4,
                              f.buf.begin() + static_cast<ptrdiff_t>(c.end));
      uint32_t old_tail = GamFile::x59_dw(tb, 0, 0x59);
      if (old_tail != old_len) continue;  // 非纯 echo 容器, 不动
      long long d_end = delta_at(c.end);
      size_t new_end = static_cast<size_t>(static_cast<long long>(c.end) + d_end);
      size_t new_start = static_cast<size_t>(static_cast<long long>(c.start) + delta_at(c.start));
      uint32_t new_len = static_cast<uint32_t>(new_end - new_start);
      if (new_end >= 4 && new_end <= nb.size()) {
        std::vector<uint8_t> totb;
        gamfile_detail::raw_x59_range(reinterpret_cast<const uint8_t*>(&new_len), 4, 0x59, totb);
        memcpy(&nb[new_end - 4], totb.data(), 4);
      }
    }
    // h2 @0x5C = 首容器长度(连续密钥 0x61..0x64)
    if (first_dirty) {
      for (auto& c : f.containers) {
        if (c.start == f.dataoff && c.end == f.dataoff + f.cont_len) {
          size_t new_end = static_cast<size_t>(static_cast<long long>(c.end) + delta_at(c.end));
          size_t new_start =
              static_cast<size_t>(static_cast<long long>(c.start) + delta_at(c.start));
          uint32_t new_len = static_cast<uint32_t>(new_end - new_start);
          for (int i = 0; i < 4; ++i)
            nb[static_cast<size_t>(0x5C + i)] =
                static_cast<uint8_t>(((new_len >> (8 * i)) & 0xFF) ^ (0x61 + i));
          break;
        }
      }
    }
    b = std::move(nb);
    // 重读一遍, 刷新容器锚点/cont_len(供下方新增插入使用)
    poq::GamFile tf;
    tf.buf = std::move(b);
    if (!gamfile_detail::parse_loaded(tf, f.path, err)) return false;
    f = std::move(tf);
    b = f.buf;
  } else {
    // 等长: 就地重写 dirty 物品; 跳过 is_new
    for (auto& it : f.items) {
      if (!it.dirty || it.is_new) continue;
      if (it.content.size() != it.len) {
        err = "物品内容长度变化需走插入流程";
        return false;
      }
      if (it.pos + 4 + it.len + 8 > f.buf.size()) {
        err = "物品位置越界";
        return false;
      }
      std::vector<uint8_t> lenb, ccb, cntb, totb;
      gamfile_detail::raw_x59_range(reinterpret_cast<const uint8_t*>(&it.len), 4, 0x59, lenb);
      gamfile_detail::raw_x59_range(it.content.data(), it.content.size(), 0x59, ccb);
      uint32_t zero = 0;
      gamfile_detail::raw_x59_range(reinterpret_cast<const uint8_t*>(&zero), 4, 0x59, cntb);
      uint32_t tot = it.len + 12;
      gamfile_detail::raw_x59_range(reinterpret_cast<const uint8_t*>(&tot), 4, 0x59, totb);
      size_t q = it.pos;
      memcpy(&b[q], lenb.data(), 4);
      q += 4;
      memcpy(&b[q], ccb.data(), ccb.size());
      q += ccb.size();
      memcpy(&b[q], cntb.data(), 4);
      q += 4;
      memcpy(&b[q], totb.data(), 4);
    }
  }
  // 2) 插入新增物品
  for (const auto& ad : added) {
    int ci = ad.first;
    if (ci < 0 || ci >= static_cast<int>(f.containers.size())) continue;
    const auto& rec = ad.second;
    GamContainer& c = f.containers[static_cast<size_t>(ci)];
    size_t pos_at = c.insert_pos;
    size_t cnt_off = c.insert_cnt_off;
    if (pos_at > b.size() || cnt_off >= pos_at) {
      err = "插入位置异常";
      return false;
    }
    // bump count (x59 编码)
    uint32_t old_c = GamFile::x59_dw(b, cnt_off);
    if (old_c >= 0x10000) {
      char hb[128];
      std::snprintf(hb, sizeof(hb), "计数异常(容器 %s 计数偏移=0x%X 值=0x%X)", c.alias.c_str(),
                    static_cast<unsigned>(cnt_off), static_cast<unsigned>(old_c));
      err = hb;
      return false;
    }
    uint32_t new_c = old_c + 1;
    std::vector<uint8_t> cntb;
    gamfile_detail::raw_x59_range(reinterpret_cast<const uint8_t*>(&new_c), 4, 0x59, cntb);
    memcpy(&b[cnt_off], cntb.data(), 4);
    // 插入记录
    b.insert(b.begin() + static_cast<ptrdiff_t>(pos_at), rec.begin(), rec.end());
    // 若插入目标有 [total==长度] 字段, 更新
    // 首个容器: 更新 hdr dword[2] + 容器尾 total
    if (c.first) {
      uint32_t new_clen = f.cont_len + static_cast<uint32_t>(rec.size());
      f.cont_len = new_clen;
      // hdr dword[2] @0x5C : 40B 头连续密钥 0x61..(0x5C-0x54=8)
      {
        uint32_t v = new_clen;
        for (int i = 0; i < 4; ++i)
          b[static_cast<size_t>(0x5C + i)] =
              static_cast<uint8_t>(((v >> (8 * i)) & 0xFF) ^ (0x61 + i));
      }
      size_t total_off = f.dataoff + new_clen - 4;
      if (total_off + 4 <= b.size()) {
        uint32_t v = new_clen;
        std::vector<uint8_t> totb;
        gamfile_detail::raw_x59_range(reinterpret_cast<const uint8_t*>(&v), 4, 0x59, totb);
        memcpy(&b[total_off], totb.data(), 4);
      }
    }
  }
  FILE* fp = gamfile_detail::f_open_w(out_path, L"wb");
  if (!fp) {
    err = "无法写入: " + out_path;
    return false;
  }
  fwrite(b.data(), 1, b.size(), fp);
  fclose(fp);
  return true;
}

}  // namespace poq
