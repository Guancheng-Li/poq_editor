// GamFile —— 词条与 G_Info 同步 / 写回(从 GamFileAffix 拆出)。
#include <cstring>
#include "Core/GamFile.h"
#include "Core/ItemData.h"

namespace poq {

static uint32_t float_to_bits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}

bool GamItem::sync_affixes_from_g_info() {
  if (!has_affixes) return false;
  std::vector<GInfoRow> rows;
  if (!parse_g_info_rows(rows)) return false;

  // 上下限成对属性: 下限 id -> 上限 id(G_Info 无 ~ 时两者同值; 数组里必须成对存在)
  const std::map<int, int>& kPair = ItemData::affix_pairs();

  // +0x10 = "比例"标志。语料实测分两类:
  //   非天生百分比的属性(命中率/闪避率/普通防御/各系攻击力防御/生命/精气…) -> A=1.0 时文本才带 %
  //   天生百分比的属性(下表: 攻击速度/吸命/致命一击/眩晕几率/抗性/时间类…) -> 文本恒带 %, 与 A
  //   无关,
  //       且原数组里 A 绝大多数是 0(偶有 1) —— 文本表达不出来, 所以按原节点保留 A
  static const int kAlwaysPct[] = {0x40, 0x41, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B,
                                   0x4C, 0x4D, 0x50, 0x52, 0x54, 0x56, 0x58, 0x5A,
                                   0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x62};
  auto defC = [](int id) -> float { return id == 0x41 ? -1.0f : 0.0f; };
  auto colorOf = [](char ch) -> uint32_t {
    switch (ch) {
      case 'K':
        return 1;
      case 'C':
        return 2;
      case 'H':
        return 2;
      case 'Z':
        return 3;
      default:
        return 0;
    }
  };
  auto activeOf = [](char ch) -> uint32_t { return ch == 'H' ? 0u : 1u; };
  // 原节点的"占用"标记: 同一 id 在数组里可能有多个(如攻击速度出现两次), 必须按出现顺序
  // 一一对应, 否则会把一个节点的系数搬到另一个上。
  std::vector<bool> origUsed(affixes.size(), false);
  auto mk = [&](int id, uint32_t col, uint32_t act, float v, bool pct) {
    AffixNode nd;
    nd.id = id;
    nd.active = act;
    nd.color = col;
    nd.value = v;
    nd.c = defC(id);
    bool always = false;
    for (int x : kAlwaysPct)
      if (x == id) always = true;
    bool found = false;
    // 优先取"同 id 且同颜色/同激活"的原节点, 再退回同 id 的第一个 —— 同 id 多节点时
    // 这样才不会把 C 系数搬到另一个节点上(实测: 攻击速度出现两次时 C 会串位)
    for (int pass = 0; pass < 2 && !found; ++pass) {
      for (size_t oi = 0; oi < affixes.size(); ++oi) {
        if (origUsed[oi] || affixes[oi].id != id) continue;
        if (pass == 0 && (affixes[oi].color != col || affixes[oi].active != act)) continue;
        origUsed[oi] = true;
        nd.c = affixes[oi].c;
        if (always) nd.a = affixes[oi].a;
        found = true;
        break;
      }
    }
    if (!always)
      nd.a = pct ? 1.0f : 0.0f;
    else if (!found)
      nd.a = 0.0f;
    return nd;
  };

  std::vector<AffixNode> list;
  static const int kAtkElem[] = {0x2B, 0x2D, 0x2F, 0x31, 0x33};
  static const int kDefElem[] = {0x36, 0x37, 0x38, 0x39, 0x3A};
  auto idAt = [&](size_t i) { return ItemData::attr_id_from_g_info_name(rows[i].name); };
  bool hasAtkElem = false, hasDefElem = false;
  for (size_t i = 0; i < rows.size(); ++i) {
    int id = idAt(i);
    for (int e : kAtkElem)
      if (id == e) hasAtkElem = true;
    for (int e : kDefElem)
      if (id == e) hasDefElem = true;
  }
  auto origHas = [&](int id) {
    for (const auto& o : affixes)
      if (o.id == id) return true;
    return false;
  };

  // 五行攻击力的"聚合形态"(游戏实测 tmp/_u01o.gam): 5 系下限相同 -> G_Info 里只有一行
  // "五行攻击力 +V", 紧跟 5 行 "X系攻击力 +W"(没有"最大"字样, W 其实是各系的**上限**)。
  // 例: 数组 2B=1,2C=2,2D=1,2E=2… -> 文本 "五行攻击力 +1 / 金系攻击力 +2 / 木系攻击力 +2…"
  // 所以要还原成 min=V、max=各行的值, 否则照字面解析会把下限 1 写成 2。
  std::vector<bool> used(rows.size(), false);
  // 各系下限"隐形"判定: 5 个 X系攻击力行都在、都只有单值(无"最大X系攻击力"行)、且没有
  // "五行攻击力"聚合行 —— 说明聚合行被游戏丢弃了(任一系下限是浅蓝色时就会, 实测 _u01r.gam)。
  // 这时 5 系下限全都不在文本里, 必须按原数组保留, 否则下限会被写成上限值。
  bool aggDropped = false;
  if (hasAtkElem) {
    bool allElem = true, anyMax = false, anyAg = false;
    for (size_t i = 0; i < rows.size(); ++i) {
      int d = idAt(i);
      if (d == 0x01 || d == 0x02) anyAg = true;
      for (int k = 0; k < 5; ++k)
        if (d == kAtkElem[k] + 1) anyMax = true;
    }
    for (int k = 0; k < 5; ++k) {
      bool found = false;
      for (size_t i = 0; i < rows.size(); ++i)
        if (idAt(i) == kAtkElem[k]) found = true;
      if (!found) allElem = false;
    }
    aggDropped = allElem && !anyMax && !anyAg;
  }
  if (hasAtkElem) {
    int agIdx = -1, elemIdx[5] = {-1, -1, -1, -1, -1};
    bool allElem = true, anyMax = false;
    for (size_t i = 0; i < rows.size(); ++i) {
      int id = idAt(i);
      if (id == 0x01 && agIdx < 0) agIdx = static_cast<int>(i);
      for (int k = 0; k < 5; ++k) {
        if (id == kAtkElem[k] && elemIdx[k] < 0) elemIdx[k] = static_cast<int>(i);
        if (id == kAtkElem[k] + 1) anyMax = true;
      }
    }
    for (int k = 0; k < 5; ++k)
      if (elemIdx[k] < 0) allElem = false;
    if (agIdx >= 0 && allElem && !anyMax) {
      used[static_cast<size_t>(agIdx)] = true;
      // 下限节点的颜色来自"五行攻击力"那一行, 但游戏是取某一系下限的颜色作代表, 5 系各自
      // 的颜色文本里看不出来 —— 所以:
      //   聚合行颜色没变 -> 逐节点保留原颜色(只有某一系被染色的情形, 如实测的 2B=金)
      //   聚合行颜色变了 -> 视为用户要整体改色, 5 个下限都换成新色
      uint32_t agCol = colorOf(rows[static_cast<size_t>(agIdx)].color);
      uint32_t agAct = activeOf(rows[static_cast<size_t>(agIdx)].color);
      float agMin = static_cast<float>(rows[static_cast<size_t>(agIdx)].vmin);
      bool found0 = false;
      uint32_t oldAggCol = agCol, oldAggAct = agAct;
      for (const auto& o : affixes)
        if (o.id == kAtkElem[0]) {
          oldAggCol = o.color;
          oldAggAct = o.active;
          found0 = true;
          break;
        }
      bool recolor = found0 && (agCol != oldAggCol || agAct != oldAggAct);
      for (int k = 0; k < 5; ++k) {
        const size_t ri = static_cast<size_t>(elemIdx[k]);
        used[ri] = true;
        uint32_t mc = agCol, ma = agAct;
        if (!recolor)
          for (const auto& o : affixes)
            if (o.id == kAtkElem[k]) {
              mc = o.color;
              ma = o.active;
              break;
            }
        list.push_back(mk(kAtkElem[k], mc, ma, agMin, rows[static_cast<size_t>(agIdx)].is_percent));
        list.push_back(mk(kAtkElem[k] + 1, colorOf(rows[ri].color), activeOf(rows[ri].color),
                          static_cast<float>(rows[ri].vmin), rows[ri].is_percent));
      }
    }
  }

  // 五行攻击力/五行防御力: 游戏既能用 0x01/0x03 单个节点表示, 也能用 5 系节点等值表示,
  // 渲染出的文本可能一样, 所以按"原数组怎么编的"来保持:
  //   ① G_Info 里已有 5 系各行 -> 跳过聚合行(那行是它们算出来的显示)
  //   ② 原数组用的是 5 系节点 -> 继续写 5 系节点(新值)
  //   ③ 否则才写聚合 id(用户新加该词条的情形)
  for (size_t i = 0; i < rows.size(); ++i) {
    if (used[i]) continue;
    const GInfoRow& r = rows[i];
    int id = idAt(i);
    if (id < 0) continue;
    uint32_t col = colorOf(r.color), act = activeOf(r.color);
    float v = static_cast<float>(r.vmin);
    if (id == 0x01 || id == 0x03) {
      const int* elems = (id == 0x01) ? kAtkElem : kDefElem;
      bool groupRowsPresent = (id == 0x01) ? hasAtkElem : hasDefElem;
      if (groupRowsPresent) continue;  // ①
      bool origElem = false;
      for (int k = 0; k < 5; ++k)
        if (origHas(elems[k])) origElem = true;
      if (origElem) {  // ②
        // 5 系节点在原数组里, 继续按 5 系写: 下限/单值 = 聚合行的值;
        // 上限(只有攻击系有)文本里表达不出来 -> 保留原上限值。
        // 颜色同攻击侧: 聚合行颜色没变就逐节点保留原色, 变了才整组换色。
        bool found0 = false;
        uint32_t oldCol = col, oldAct = act;
        for (const auto& o : affixes)
          if (o.id == elems[0]) {
            oldCol = o.color;
            oldAct = o.active;
            found0 = true;
            break;
          }
        bool recolor = found0 && (col != oldCol || act != oldAct);
        for (int k = 0; k < 5; ++k) {
          uint32_t mc = col, ma = act;
          if (!recolor)
            for (const auto& o : affixes)
              if (o.id == elems[k]) {
                mc = o.color;
                ma = o.active;
                break;
              }
          list.push_back(mk(elems[k], mc, ma, v, r.is_percent));
          if (id == 0x01) {
            // 上限值/比例在文本里表达不出来 -> 按原上限节点保留
            float mv = v;
            bool mpct = r.is_percent;
            for (const auto& o : affixes)
              if (o.id == elems[k] + 1) {
                mv = o.value;
                mpct = (o.a != 0.0f);
                break;
              }
            list.push_back(mk(elems[k] + 1, col, act, mv, mpct));
          }
        }
        continue;
      }
      // ③ 落到下面的普通规则(0x01/0x02 是成对项, 0x03 是单项)
      if (id == 0x03) {
        list.push_back(mk(0x03, col, act, v, r.is_percent));
        continue;
      }
    }
    auto pit = kPair.find(id);
    if (pit != kPair.end()) {
      // parse_g_info_rows 会把 "X +下 ~ +上" 拆成两行, 所以"有最大行" == "文本给了上下两个值"
      bool hasMaxRow = (i + 1 < rows.size() && idAt(i + 1) == pit->second);
      float mv = hasMaxRow ? static_cast<float>(rows[i + 1].vmin) : v;
      if (hasMaxRow) used[i + 1] = true;
      // 各系攻击力下限的"隐形下限": 文本只给一个数字时它是【上限】; 若原数组该下限是
      // 浅蓝色(color 2), 游戏会把"五行攻击力"聚合行整个丢掉(实测 tmp/_u01r.gam), 下限
      // 在文本里彻底消失 —— 必须保留原下限的值/颜色, 否则会把下限写成上限值。
      bool isAtkMin = false;
      for (int e : kAtkElem)
        if (id == e) isAtkMin = true;
      // "X系攻击力 +v" 这一行的 v 是下限还是上限? 文本看不出来, 用原数组判定:
      // 原数组没有下限节点、只有上限节点 -> 这行就是上限(实测 tmp/_u01u.gam: 下限被换成了
      // 五行防御力, 只剩 2C/2E/30/32/34, 游戏照样印 "金系攻击力 +2")。
      if (!hasMaxRow && isAtkMin) {
        bool oMin = false, oMax = false;
        for (const auto& o : affixes) {
          if (o.id == id) oMin = true;
          if (o.id == id + 1) oMax = true;
        }
        if (!oMin && oMax) {
          list.push_back(mk(pit->second, col, act, v, r.is_percent));
          continue;
        }
      }
      // 各系攻击力下限: 文本只给一个数字时它是【上限】, 下限若因为聚合行被丢弃而
      // 在文本里消失(见上 aggDropped), 就整体按原数组保留下限节点。
      bool keepOrig = false;
      AffixNode origMin;
      if (!hasMaxRow && aggDropped && isAtkMin)
        for (const auto& o : affixes)
          if (o.id == id) {
            origMin = o;
            keepOrig = true;
            break;
          }
      list.push_back(keepOrig ? origMin : mk(id, col, act, v, r.is_percent));
      list.push_back(
          mk(pit->second, col, act, mv, hasMaxRow ? rows[i + 1].is_percent : r.is_percent));
      continue;
    }
    list.push_back(mk(id, col, act, v, r.is_percent));
  }
  // 值为 0 的原节点 G_Info 不渲染(游戏也不显示), 但它确实是数组内容: 原样保留,
  // 免得只改了个名字/等级就把这类占位节点丢掉。
  for (const auto& old : affixes) {
    if (old.value != 0.0f) continue;
    bool got_slot = false;
    for (const auto& nd : list)
      if (nd.id == old.id) {
        got_slot = true;
        break;
      }
    if (!got_slot) list.push_back(old);
  }

  return set_affixes(list);
}

bool GamItem::set_affixes(const std::vector<AffixNode>& list) {
  if (content.size() < affix_tail) return false;
  // 组装: [前缀][count][节点...][内容长度]
  size_t keep = content.size() - affix_tail;
  std::vector<uint8_t> out(content.begin(), content.begin() + static_cast<ptrdiff_t>(keep));
  auto putDw = [&](uint32_t v) {
    uint32_t e = v ^ 0xAAABACADu;
    out.push_back(static_cast<uint8_t>(e & 0xFF));
    out.push_back(static_cast<uint8_t>((e >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((e >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((e >> 24) & 0xFF));
  };
  putDw(static_cast<uint32_t>(list.size()));
  for (const auto& nd : list) {
    putDw(0x2Cu);
    putDw(0xFFFFFFFFu);
    putDw(static_cast<uint32_t>(nd.id));
    putDw(nd.active);
    putDw(nd.color);
    putDw(0);
    putDw(float_to_bits(nd.a));
    putDw(float_to_bits(nd.value));
    putDw(0);
    putDw(float_to_bits(nd.c));
    putDw(0);
  }
  putDw(static_cast<uint32_t>(out.size()) + 4);  // 内容长度(含本 dword)
  content = std::move(out);
  invalidate_g_info_cache();
  affixes = list;
  affix_tail = 4 + 44 * list.size() + 4;
  has_affixes = true;
  dirty = true;
  return true;
}

}  // namespace poq
