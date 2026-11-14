// GamFile —— G_Info 词条文本(GBK 行格式)读写与解析。
//
// 从 GamFile.cpp 拆出: 游戏显示用 G_Info 字符串; 词条数组见 GamFileAffix.cpp。

#include <cstdio>
#include "Core/GamFile.h"
#include "Core/GamFileInternal.h"
#include "Core/ItemData.h"

namespace poq::gamfile_detail {

void write_content_len(std::vector<uint8_t>& c) {
  if (c.size() < 4) return;
  const uint32_t e = static_cast<uint32_t>(c.size()) ^ 0xAAABACADu;
  c[c.size() - 4] = static_cast<uint8_t>(e & 0xFF);
  c[c.size() - 3] = static_cast<uint8_t>((e >> 8) & 0xFF);
  c[c.size() - 2] = static_cast<uint8_t>((e >> 16) & 0xFF);
  c[c.size() - 1] = static_cast<uint8_t>((e >> 24) & 0xFF);
}

}  // namespace poq::gamfile_detail

namespace poq {

std::string GamItem::get_g_info() const {
  // 命中缓存: 直接读该偏移处的值字段(P3)
  if (ginfo_key_off_ != 0 && ginfo_key_off_ + 4 < content.size()) {
    std::string s;
    size_t used = 0;
    if (GamFile::dec_str(content, ginfo_key_off_, s, used) && s == "G_Info") {
      std::string val;
      size_t used2 = 0;
      if (GamFile::dec_str(content, ginfo_key_off_ + used, val, used2)) return val;
    }
    ginfo_key_off_ = 0;  // 缓存失效, 退回全扫
  }
  // 找字段名串 "G_Info", 其后紧跟的字符串即词条文本
  for (size_t off = 0; off + 4 < content.size(); ++off) {
    std::string s;
    size_t used = 0;
    if (GamFile::dec_str(content, off, s, used)) {
      if (s == "G_Info" && off + used + 4 < content.size()) {
        std::string val;
        size_t used2 = 0;
        if (GamFile::dec_str(content, off + used, val, used2)) {
          ginfo_key_off_ = off;  // 记住偏移, 后续调用 O(1)
          return val;
        }
      }
    }
  }
  return "";
}

bool GamItem::set_g_info(const std::string& gbktext) {
  // 找 G_Info 值字段, 在 content 中整体替换(等长或变长均可; 变长由 save 重排落盘)
  for (size_t off = 0; off + 4 < content.size(); ++off) {
    std::string s;
    size_t used = 0;
    if (GamFile::dec_str(content, off, s, used)) {
      if (s == "G_Info") {
        size_t valoff = off + used;
        std::string oldval;
        size_t oldused = 0;
        if (!GamFile::dec_str(content, valoff, oldval, oldused)) return false;
        if (oldval == gbktext) return true;  // 无变化: 不动数组
        // 编码新文本: [4B len=(strlen+1)^KEY][chars ^(strlen-0x56-i)][\0 ^...]
        size_t newlen = gbktext.size();
        uint32_t slen = static_cast<uint32_t>(newlen) + 1;
        uint32_t enc = slen ^ 0xAAABACADu;
        std::vector<uint8_t> nb;
        nb.reserve(valoff + 4 + newlen + 1 + (content.size() - valoff - oldused));
        nb.insert(nb.end(), content.begin(), content.begin() + valoff);
        nb.push_back(static_cast<uint8_t>(enc & 0xFF));
        nb.push_back(static_cast<uint8_t>((enc >> 8) & 0xFF));
        nb.push_back(static_cast<uint8_t>((enc >> 16) & 0xFF));
        nb.push_back(static_cast<uint8_t>((enc >> 24) & 0xFF));
        for (size_t i = 0; i < newlen; ++i)
          nb.push_back(
              static_cast<uint8_t>(gbktext[i] ^ static_cast<uint8_t>((newlen - 0x56 - i) & 0xFF)));
        nb.push_back(static_cast<uint8_t>(
            0 ^ static_cast<uint8_t>((newlen - 0x56 - newlen) & 0xFF)));  // 结尾 \0
        nb.insert(nb.end(), content.begin() + valoff + oldused, content.end());
        content = std::move(nb);
        invalidate_g_info_cache();
        if (has_affixes)
          gamfile_detail::write_content_len(content);  // G_Info 在数组前面, 变长/变短都平移了尾帧
        dirty = true;
        // 词条数组是游戏真正读的结构: G_Info 改了必须同步重建它
        // (no_sync: 补长度时只在文本尾部补空格, 词条行没变, 不必也不能重建 —— 否则
        //  第二遍会拿到已重建的数组, 让"只存上限节点"这类消歧规则失效、凭空长出下限节点)
        if (has_affixes && !no_sync && !sync_affixes_from_g_info()) return false;
        return true;
      }
    }
  }
  return false;
}

bool GamItem::pad_content_to(size_t want_len) {
  if (!has_affixes) return false;
  for (int iter = 0; iter < 4 && content.size() < want_len; ++iter) {
    std::string gi = get_g_info();
    if (gi.empty()) return false;
    size_t need = want_len - content.size();
    gi.append(need, ' ');  // G_Info 尾部补空格(解析时逐行去尾空格, 不影响词条)
    size_t before = content.size();
    no_sync = true;  // 只动文本尾部 -> 不重建词条数组
    bool okpad = set_g_info(gi);
    no_sync = false;
    if (!okpad) return false;
    if (content.size() <= before) return false;  // 没长说明补不进去
  }
  return content.size() >= want_len;
}

bool GamItem::parse_g_info_rows(std::vector<GInfoRow>& rows) const {
  std::string txt = get_g_info();
  rows.clear();
  if (txt.empty()) return false;
  char cur_color = 0;
  // 逐行(记录字节偏移, 供值重写)
  size_t scan = 0;  // 逐行扫描游标(不叫 pos: 遮蔽 GamItem::pos 成员)
  while (scan <= txt.size()) {
    size_t nl = txt.find('\n', scan);
    std::string line = (nl == std::string::npos) ? txt.substr(scan) : txt.substr(scan, nl - scan);
    bool group_ends = false;
    std::string inner = line;
    // [K / [H / [Z ... (可跨行: 组首行只写 "["+字母, ']' 落在组末行行尾)
    size_t n = inner.size();
    size_t k = 0;
    if (n >= 2 && inner[0] == '[') {
      char c = inner[1];
      if (c == 'K' || c == 'H' || c == 'Z' || c == 'C') {
        cur_color = c;
        k = 2;
      }
    }
    // 组结束: 行尾的 ']'(单行组与多行组的末行都是这个写法)
    {
      size_t e = n;
      while (e > k && (inner[e - 1] == ' ' || inner[e - 1] == '\t')) --e;
      if (e > k && inner[e - 1] == ']') {
        group_ends = true;
        n = e - 1;
      }
    }
    // 头部行: &... 或 Lv.
    {
      std::string s = inner.substr(k, n - k);
      size_t t0 = s.find_first_not_of(" \t");
      if (t0 != std::string::npos) {
        bool is_header = (s[t0] == '&');
        if (!is_header) {
          // 取名字 token(到空白/GBK对)
          size_t j = t0;
          while (j < n - k && static_cast<uint8_t>(inner[k + j]) > 0x20) {
            if (static_cast<uint8_t>(inner[k + j]) >= 0x81 && k + j + 1 < n)
              j += 2;
            else
              j += 1;
          }
          std::string token = inner.substr(k + t0, j - t0);  // 不叫 name: 遮蔽成员
          is_header = (token == "Lv" || token == "Lv.");
        }
        if (!is_header) {
          // 属性名 ... 值
          std::string attr;  // 不叫 name: 会遮蔽 GamItem::name 成员
          long vmin = 0, vmax = 0;
          // 名字到第一个空白
          size_t j = t0;
          while (j < n - k && static_cast<uint8_t>(inner[k + j]) > 0x20) {
            if (static_cast<uint8_t>(inner[k + j]) >= 0x81 && k + j + 1 < n)
              j += 2;
            else
              j += 1;
          }
          attr = inner.substr(k + t0, j - t0);
          // 跳过空白
          size_t vp = j;
          while (vp < n - k && (inner[k + vp] == ' ' || inner[k + vp] == '\t')) ++vp;
          // 值: [+-]digits [~ [+-]digits]
          size_t v1 = vp;
          bool neg = (v1 < n - k && (inner[k + v1] == '+' || inner[k + v1] == '-'));
          if (neg) ++v1;
          size_t d = v1;
          while (d < n - k && inner[k + d] >= '0' && inner[k + d] <= '9') ++d;
          if (d > v1) {
            vmin = atol(inner.substr(k + v1, d - v1).c_str());
            if (inner[k + v1 - 1] == '-') vmin = -vmin;
            // 百分比标记 \x0f
            bool is_percent = false;
            size_t t = d;
            while (t < n - k && static_cast<uint8_t>(inner[k + t]) == 0x0F) {
              is_percent = true;
              ++t;
            }
            size_t t2 = t;
            while (t2 < n - k && (inner[k + t2] == ' ' || inner[k + t2] == '\t')) ++t2;
            bool has_max = false;
            if (t2 < n - k && inner[k + t2] == '~') {
              size_t t3 = t2 + 1;
              while (t3 < n - k && (inner[k + t3] == ' ' || inner[k + t3] == '\t')) ++t3;
              bool neg2 = (t3 < n - k && (inner[k + t3] == '+' || inner[k + t3] == '-'));
              if (neg2) ++t3;
              size_t d2 = t3;
              while (d2 < n - k && inner[k + d2] >= '0' && inner[k + d2] <= '9') ++d2;
              if (d2 > t3) {
                vmax = atol(inner.substr(k + t3, d2 - t3).c_str());
                if (neg2 && t3 > 0 && inner[k + t3 - 1] == '-') vmax = -vmax;
                has_max = true;
                // 范围上限后也可能有 \x0f
                size_t tt = d2;
                while (tt < n - k && static_cast<uint8_t>(inner[k + tt]) == 0x0F) {
                  is_percent = true;
                  ++tt;
                }
              }
            }
            GInfoRow r{};
            r.color = cur_color;
            r.name = attr;
            r.vmin = vmin;
            r.vmax = 0;
            r.has_max = false;
            r.is_percent = is_percent;
            rows.push_back(std::move(r));
            if (has_max) {
              // 上限是独立词条: 拆成 "最大<名>" 一行(UI 同道具修改; 保存时再合成 "名
              // +v ~ +w")
              GInfoRow r2{};
              r2.color = cur_color;
              r2.name = std::string("\xD7\xEE\xB4\xF3") + attr;  // GBK "最大"
              r2.vmin = vmax;
              r2.is_percent = is_percent;
              rows.push_back(std::move(r2));
            }
          }
        }
      }
    }
    if (group_ends) cur_color = 0;
    if (nl == std::string::npos) break;
    scan = nl + 1;
  }
  // 文本非空即成功: 0 行(词条删光)也是合法结果 —— 不能用 rows.empty() 当失败,
  // 否则 sync_affixes_from_g_info 会放弃重建, "删光词条"就写不进数组(P0)。
  return !txt.empty();
}

bool GamItem::set_g_info_row_value(size_t idx, long vmin, long vmax) {
  // 值改写受 等长原位 约束(变长需重排)。交原文本编辑面板处理; 此处不自动改写。
  (void)idx;
  (void)vmin;
  (void)vmax;
  return false;
}

std::string GamItem::get_g_info_header() const {
  std::string txt = get_g_info();
  size_t nl = txt.find('\n');
  std::string first = (nl == std::string::npos) ? txt : txt.substr(0, nl);
  size_t t0 = first.find_first_not_of(" \t");
  if (t0 == std::string::npos) return "";
  // 头行: &... 或 Lv.
  if (first[t0] == '&') {
    return (nl == std::string::npos) ? first : first + "\n";
  }
  // 名字 token 是否为 Lv / Lv.
  size_t j = t0;
  while (j < first.size() && static_cast<uint8_t>(first[j]) > 0x20) ++j;
  std::string token = first.substr(t0, j - t0);  // 不叫 name: 遮蔽成员
  if (token == "Lv" || token == "Lv.") {
    return (nl == std::string::npos) ? first : first + "\n";
  }
  return "";
}

bool GamItem::build_g_info_text(const std::string& header, const std::vector<GInfoRow>& rows,
                                std::string& out) {
  out.clear();
  out += header;
  // 合并相邻的 (X, 最大X) -> X +v ~ +w
  // 只对真正的上下限成对属性合并(如 普通攻击力/最大普通攻击力、有效距离/最大有效距离);
  // 最大生命/最大精气/最大负重 是独立词条, 绝不能并进前一行。
  std::vector<GInfoRow> M;
  for (size_t i = 0; i < rows.size(); ++i) {
    const GInfoRow& r = rows[i];
    if (r.name.size() >= 4 && r.name.compare(0, 4, "\xD7\xEE\xB4\xF3") == 0 && !M.empty() &&
        M.back().color == r.color && !M.back().has_max && M.back().name == r.name.substr(4) &&
        ItemData::is_affix_pair_min_id(ItemData::attr_id_from_g_info_name(M.back().name))) {
      M.back().has_max = true;
      M.back().vmax = r.vmin;
      continue;
    }
    M.push_back(r);
    M.back().has_max = false;
  }
  auto sign = [](long v) { return v < 0 ? "-" : "+"; };
  // fmt: "名 +值[ ~ +上限]"(不带尾随)
  auto fmt = [&](const GInfoRow& r) {
    std::string s = r.name;
    char buf[32];
    std::snprintf(buf, sizeof(buf), " %s%ld", sign(r.vmin), r.vmin < 0 ? -r.vmin : r.vmin);
    s += buf;
    if (r.has_max) {
      std::snprintf(buf, sizeof(buf), " ~ %s%ld", sign(r.vmax), r.vmax < 0 ? -r.vmax : r.vmax);
      s += buf;
    }
    return s;
  };
  // 尾部标记: 百分比->\x0f; 有上限(~)->无; 无上限->空格 (与游戏原文一致)
  auto tail = [](const GInfoRow& r) {
    if (r.is_percent) return std::string(1, static_cast<char>(0x0F));
    if (r.has_max) return std::string();
    return std::string(" ");
  };
  auto emit = [&](const GInfoRow& r) { return fmt(r) + tail(r); };
  for (size_t i = 0; i < M.size(); ++i) {
    const GInfoRow& r = M[i];
    if (r.color == 0) {
      out += emit(r);
      out += "\n";
    } else {
      size_t j = i + 1;
      while (j < M.size() && M[j].color == r.color) ++j;
      out += "[";
      out += r.color;
      if (j == i + 1) {
        out += emit(r);
      } else {
        for (size_t k = i; k < j; ++k) {
          if (k > i) out += "\n";
          out += emit(M[k]);
        }
      }
      out += "]\n";
      i = j - 1;
    }
  }
  if (!out.empty() && out.back() == '\n') out.pop_back();  // 原文各行以 \n 分隔,末尾无换行
  return true;
}

}  // namespace poq
