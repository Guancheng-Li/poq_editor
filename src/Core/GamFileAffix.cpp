// GamFile —— 词条数组(记录末尾 44 字节节点链)相关实现。
//
// 从 GamFile.cpp 拆出(计划 260919 P0/P4): 游戏真正读的是记录末尾的词条数组,
// G_Info 只是它的文本渲染; 这块逻辑与 G_Info 文本处理分开, 便于单独维护。
//
// 依赖: Core/GamFile.h(成员声明 + GamFile::dw_at)、Core/ItemData.h(affix_pairs 等)。

#include <cstring>
#include "Core/GamFile.h"
#include "Core/ItemData.h"

namespace poq {

// ------------------------------------------------------------------
// 词条数组(记录末尾): 游戏真正读的结构, G_Info 只是它的文本渲染
// ------------------------------------------------------------------

static float bits_to_float(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void GamItem::parse_affixes() {
  affixes.clear();
  has_affixes = false;
  affix_tail = 0;
  const std::vector<uint8_t>& c = content;
  if (c.size() < 64) return;
  // 末尾 4 字节 = 内容长度
  if (GamFile::dw_at(c, c.size() - 4) != static_cast<uint32_t>(c.size())) return;
  // 词条数组在内容末尾: 先从尾部按 44 步长倒着验证(P4: O(节点数), 原来整段逐字节全扫);
  // 倒着不成立时退回原来的前向全扫(兼容异常布局)。
  size_t base = 0, n = 0;
  if (c.size() >= 48 && GamFile::dw_at(c, c.size() - 48) == 0x2Cu &&
      GamFile::dw_at(c, c.size() - 48 + 4) == 0xFFFFFFFFu) {
    size_t b = c.size() - 48;
    size_t k = 1;
    while (b >= 44 && GamFile::dw_at(c, b - 44) == 0x2Cu &&
           GamFile::dw_at(c, b - 44 + 4) == 0xFFFFFFFFu) {
      b -= 44;
      ++k;
    }
    if (b >= 4 && GamFile::dw_at(c, b - 4) == static_cast<uint32_t>(k)) {
      base = b;
      n = k;
    }
  }
  if (n == 0) {
    // 回退: 收集所有 [0x2C][0xFFFFFFFF] 标记, 取最长 44 步长链且末节点收在长度字段之前
    std::vector<size_t> marks;
    for (size_t o = 0; o + 8 <= c.size(); ++o)
      if (GamFile::dw_at(c, o) == 0x2Cu && GamFile::dw_at(c, o + 4) == 0xFFFFFFFFu)
        marks.push_back(o);
    for (size_t i = 0; i < marks.size(); ++i) {
      size_t k = 1;
      while (i + k < marks.size() && marks[i + k] == marks[i] + 44 * k) ++k;
      if (marks[i] + 44 * k == c.size() - 4 && k > n) {
        base = marks[i];
        n = k;
      }
    }
  }
  if (n == 0 || base < 4) {
    // 没找到节点链: 再看是不是"0 节点数组"(词条被全部删光, 只剩 [count=0][长度])。
    // 必须放在这里(确认无链之后) —— 正常物品的末节点负载尾部本身也是 0, 提前判会全部误判。
    if (c.size() >= 8 && GamFile::dw_at(c, c.size() - 8) == 0u) {
      affix_tail = 8;
      has_affixes = true;
    }
    return;
  }
  // 首节点前一个 dword = 词条数, 与链长自洽才认
  if (GamFile::dw_at(c, base - 4) != static_cast<uint32_t>(n)) return;
  for (size_t k = 0; k < n; ++k) {
    size_t p = base + 8 + 44 * k;
    AffixNode a;
    a.id = static_cast<int32_t>(GamFile::dw_at(c, p));
    a.active = GamFile::dw_at(c, p + 4);
    a.color = GamFile::dw_at(c, p + 8);
    a.a = bits_to_float(GamFile::dw_at(c, p + 0x10));
    a.value = bits_to_float(GamFile::dw_at(c, p + 0x14));
    a.c = bits_to_float(GamFile::dw_at(c, p + 0x1C));
    affixes.push_back(a);
  }
  affix_tail = 4 + 44 * n + 4;
  has_affixes = true;
}

}  // namespace poq
