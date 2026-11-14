// GameModder —— 物品指针 / 基础属性读写。
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "Core/GameModder.h"
#include "Core/GameModderInternal.h"

namespace poq {

using modder_detail::addr_hex8;
using modder_detail::append_cmp_esi_imm;
using modder_detail::append_le32;
using modder_detail::gbk_encode_one;
using modder_detail::hex_bytes;
using modder_detail::utf8_to_codepoints;

uintptr_t GameModder::get_item_pointer() {
  if (!cur_item_addr_) return 0;
  return static_cast<uintptr_t>(mem_.read_u_int32(cur_item_addr_));
}

uintptr_t GameModder::get_item_base() {
  uintptr_t p = get_item_pointer();
  if (!cur_item_addr_)
    throw std::runtime_error(
        "尚未注入捕获 hook:请检查是否已点\"初始化\"(注入失败时重新初始化)。"
        "人物修改不受影响。");
  if (p == 0 || p == 0xFFFFFFFF)
    throw std::runtime_error("当前无手持物品:请在游戏中拾起物品并保持在光标上,再读取/保存。");
  return p + static_cast<uintptr_t>(static_cast<long long>(config_.current().item_base_adjust));
}

// ============================================================================
// 基础属性:读
// ============================================================================

BasicState GameModder::read_basic_fields(uintptr_t item_base) {
  BasicState result;
  const ModeCfg& mode = config_.current();
  // 读"类型"字段值,用于条件/强制判定
  const BasicFieldCfg* type_field = nullptr;
  for (const auto& f : mode.basic_fields)
    if (f.label == "类型") {
      type_field = &f;
      break;
    }
  std::optional<int> type_val;
  if (type_field) type_val = static_cast<int>(mem_.read_u_int32(item_base + type_field->offset));

  for (const auto& f : mode.basic_fields) {
    if (f.type == "string_ptr") {
      result.strs[f.label] = read_string_field(item_base, f);
    } else if (f.type == "word") {
      result.ints[f.label] = static_cast<int>(mem_.read_word(item_base + f.offset));
    } else {  // dword / enum
      result.ints[f.label] = static_cast<int>(mem_.read_u_int32(item_base + f.offset));
    }
  }
  // force_when:类型为箭/矢时,孔数/耐久度/耐久度上限强制为指定值
  for (const auto& f : mode.basic_fields) {
    if (f.force_when && f.is_forced(type_val)) result.ints[f.label] = f.force_when->value;
  }
  // 弓箭数量:非箭/矢显示 -1
  for (const auto& f : mode.basic_fields) {
    if (f.conditional && f.conditional->field == "类型") {
      bool in = type_val && std::find(f.conditional->values.begin(), f.conditional->values.end(),
                                      *type_val) != f.conditional->values.end();
      if (!in) result.ints[f.label] = -1;
    }
  }
  return result;
}

std::string GameModder::read_string_field(uintptr_t item_base, const BasicFieldCfg& f) {
  try {
    uintptr_t ptr = mem_.read_u_int32(item_base + f.offset);
    if (ptr == 0 || ptr == 0xFFFFFFFF) return "未知物品";
    auto s = mem_.read_string_gbk(ptr, 100, f.codepage);
    return s ? *s : std::string("未知物品");
  } catch (...) {
    return "未知物品";
  }
}

void GameModder::write_string_field(uintptr_t item_base, const BasicFieldCfg& f,
                                    const std::string& text) {
  // 把字符串(武器名字)原地写入 [item+offset] 指向的字符串位置。
  // 按 GBK 编码 + 结尾 0。最多 50 个字符(按字符边界截断,不截断多字节汉字),
  // 字节上限 100(50 个汉字正好 100 字节)。不改指针。
  try {
    uintptr_t ptr = mem_.read_u_int32(item_base + f.offset);
    if (ptr == 0 || ptr == 0xFFFFFFFF) return;
    // 计划 P1/P2(#10): 旧实现无条件写最多 101 字节, 而"上限 100"并不代表目标 buffer
    // 真有 100 字节容量 —— 若旧字符串只分配了很小空间, 会覆盖相邻堆对象。
    // 保守策略: 不超过"当前字符串实际长度"(装得下才写), 也不超过 100 字节硬上限。
    // 代价: 比现字符串更长的新名字会被截断, 但不会破坏堆。
    const size_t kMaxStringBytes = 100;  // 50 个汉字
    size_t curLen = 0;
    try {
      auto opt = mem_.read_string_gbk(ptr, kMaxStringBytes + 1, f.codepage);
      curLen = opt ? opt->size() : 0;
    } catch (...) {
      curLen = 0;  // 读不到就按最小容量处理
    }
    const size_t kLimit = std::min(curLen, kMaxStringBytes);
    std::string data;
    for (uint32_t cp : utf8_to_codepoints(text)) {
      std::string chb = gbk_encode_one(cp);
      if (data.size() + chb.size() > kLimit) break;
      data += chb;
    }
    data.push_back('\0');
    mem_.write(ptr, std::vector<unsigned char>(data.begin(), data.end()));
  } catch (...) {
  }
}

// ============================================================================
// 基础属性:写(含校验/clamp)
// ============================================================================

void GameModder::write_basic_fields(uintptr_t item_base, const BasicState& values) {
  const ModeCfg& mode = config_.current();
  // 类型值(force_when/conditional 都引用它):优先输入值,否则读内存
  const BasicFieldCfg* type_field = nullptr;
  for (const auto& f : mode.basic_fields)
    if (f.label == "类型") {
      type_field = &f;
      break;
    }
  std::optional<int> type_val;
  auto tv_it = values.ints.find("类型");
  if (tv_it != values.ints.end()) {
    type_val = tv_it->second;
  } else if (type_field) {
    try {
      type_val = static_cast<int>(mem_.read_u_int32(item_base + type_field->offset));
    } catch (...) {
      type_val = std::nullopt;
    }
  }

  // 耐久度上限写入值(强制或 clamp),供耐久度 max_ref 使用
  const BasicFieldCfg* cap_field = nullptr;
  for (const auto& f : mode.basic_fields)
    if (f.label == "耐久度上限") {
      cap_field = &f;
      break;
    }
  std::optional<int> cap_val;
  if (cap_field && values.ints.count(cap_field->label) && !cap_field->readonly) {
    if (cap_field->is_forced(type_val))
      cap_val = cap_field->force_when->value;
    else
      cap_val = cap_field->clamp(values.ints.at(cap_field->label));
  }

  // 快照当前耐久度/上限(补满判定用;强制时不需)
  const BasicFieldCfg* dur_field = nullptr;
  for (const auto& f : mode.basic_fields)
    if (f.max_ref) {
      dur_field = &f;
      break;
    }
  std::optional<int> mem_dur, mem_cap;
  if (dur_field && !dur_field->is_forced(type_val)) {
    try {
      mem_dur = static_cast<int>(mem_.read_u_int32(item_base + dur_field->offset));
    } catch (...) {
      mem_dur = std::nullopt;
    }
  }
  if (cap_field) {
    try {
      mem_cap = static_cast<int>(mem_.read_u_int32(item_base + cap_field->offset));
    } catch (...) {
      mem_cap = std::nullopt;
    }
  }

  // 第一遍:写无依赖、无强制的字段(含 类型、武器名字)
  for (const auto& f : mode.basic_fields) {
    bool has = values.ints.count(f.label) > 0 || values.strs.count(f.label) > 0;
    if (!has || f.readonly) continue;
    if (f.type == "string_ptr") {
      // 字符串字段(武器名字)直接写
      auto sit = values.strs.find(f.label);
      if (sit != values.strs.end()) write_string_field(item_base, f, sit->second);
      continue;
    }
    auto it = values.ints.find(f.label);
    if (it == values.ints.end()) continue;
    if (f.max_ref || f.conditional || f.force_when) continue;
    write_field(item_base, f, f.clamp(it->second));
  }

  // 第二遍:写依赖/强制字段
  for (const auto& f : mode.basic_fields) {
    auto it = values.ints.find(f.label);
    if (it == values.ints.end() || f.readonly) continue;
    if (f.force_when && f.is_forced(type_val)) {
      // 强制值(箭矢:孔数=0/耐久度=-1/耐久度上限=-1),bypass clamp
      write_field(item_base, f, f.force_when->value);
    } else if (f.max_ref) {
      // 耐久度:与耐久度上限联动。
      // 上限为 0/-1 时强制保持一致(0 无法装备 / -1 永不损坏);
      // 其余情况下耐久度 ≤ 上限(改上限且新上限>原耐久度时补满到上限)。
      int v = it->second;
      if (cap_val && (*cap_val == 0 || *cap_val == -1)) {
        v = *cap_val;
      } else {
        int eff_max = cap_val ? *cap_val : (f.max ? *f.max : 254);
        if (cap_val && mem_cap && mem_dur && *cap_val != *mem_cap && *cap_val > *mem_dur) {
          v = *cap_val;
        } else {
          if (f.min && v < *f.min) v = *f.min;
          if (v > eff_max) v = eff_max;
        }
      }
      write_field(item_base, f, v);
    } else if (f.conditional) {
      // 弓箭数量:仅类型为箭/矢时写入
      if (type_val && std::find(f.conditional->values.begin(), f.conditional->values.end(),
                                *type_val) != f.conditional->values.end())
        write_field(item_base, f, f.clamp(it->second));
      // 否则不写入
    } else if (f.force_when) {
      // 有 force_when 但当前未强制:按普通字段 clamp 写入
      write_field(item_base, f, f.clamp(it->second));
    }
  }
}

void GameModder::write_field(uintptr_t item_base, const BasicFieldCfg& f, int v) {
  if (f.type == "word")
    mem_.write_word(item_base + f.offset, static_cast<uint16_t>(v & 0xFFFF));
  else
    mem_.write_u_int32(item_base + f.offset, static_cast<uint32_t>(v));
}
}  // namespace poq
