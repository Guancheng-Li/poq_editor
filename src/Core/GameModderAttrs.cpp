// GameModder —— 词条链表读写 / 动态节点池。
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

std::vector<uintptr_t> GameModder::read_node_addrs(uintptr_t item_base) {
  const AttrListCfg& cfg = config_.current().attr_list;
  std::vector<uintptr_t> addrs;
  uint32_t head = 0;
  if (!mem_.read_buf(item_base + cfg.head_offset, &head, 4)) return addrs;  // P2: 栈缓冲, 不分配
  uintptr_t addr = head;
  while (true) {
    if (addr == 0 || addr == 0xFFFFFFFF || addr < 0x10000) break;
    if (std::find(addrs.begin(), addrs.end(), addr) != addrs.end()) break;  // 环保护
    addrs.push_back(addr);
    uint32_t next = 0;
    if (!mem_.read_buf(addr + cfg.next_offset, &next, 4)) break;
    addr = next;
  }
  return addrs;
}

std::vector<ItemProperty> GameModder::read_attributes(uintptr_t item_base) {
  const AttrListCfg& cfg = config_.current().attr_list;
  std::vector<ItemProperty> result;
  // P2: 节点字段全在同一槽内(id 0x0C..value 0x20), 整段一次读 ——
  // 原来是每节点 5 次 ReadProcessMemory + 5 次 vector 分配(20 条词条 ≈ 120 次系统调用)。
  uint32_t max_off = cfg.id_offset + 4;
  for (uint32_t o : {cfg.color_offset + 4, cfg.node_flag_offset + 4, cfg.precent_offset + 4,
                     cfg.value_offset + 4})
    if (o > max_off) max_off = o;
  const int need = static_cast<int>(max_off);
  std::vector<unsigned char> slot(static_cast<size_t>(need));  // 循环外分配一次, 逐节点复用
  for (uintptr_t addr : read_node_addrs(item_base)) {
    if (!mem_.read_buf(addr, slot.data(), need)) continue;  // 坏地址跳过(同旧行为)
    auto dw = [&](uint32_t off) -> uint32_t {
      return static_cast<uint32_t>(slot[off]) | (static_cast<uint32_t>(slot[off + 1]) << 8) |
             (static_cast<uint32_t>(slot[off + 2]) << 16) |
             (static_cast<uint32_t>(slot[off + 3]) << 24);
    };
    auto fl = [&](uint32_t off) -> float {
      uint32_t v = dw(off);
      float f;
      std::memcpy(&f, &v, 4);
      return f;
    };
    uint32_t aid = dw(cfg.id_offset);
    uint32_t color = dw(cfg.color_offset);
    uint32_t flag = dw(cfg.node_flag_offset);
    uint32_t prec = fl(cfg.precent_offset) != 0.0f ? 1 : 0;  // precent 是 float, 读后转 0/1
    float val = fl(cfg.value_offset);
    result.push_back(ItemProperty(addr, aid, color, flag ? 1u : 0u, prec, val));
  }
  return result;
}

// ============================================================================
// 非固定属性:写(改字段值 + 需要时用静态节点池追加)
// ============================================================================

std::pair<int, int> GameModder::write_attributes(uintptr_t item_base,
                                                 const std::vector<ItemProperty>& props) {
  const AttrListCfg& cfg = config_.current().attr_list;
  auto existing = read_node_addrs(item_base);
  int k = static_cast<int>(existing.size());
  int m = static_cast<int>(props.size());
  int written = 0;

  // 1) 写前 min(m,k) 行到现有节点(并自愈:哨兵/标志)。
  //    prev 不写:web.ct 模板实证游戏原生链表 prev 全为 0,保持游戏自己写的值。
  int minmk = std::min(m, k);
  for (int i = 0; i < minmk; ++i) {
    write_node(existing[static_cast<size_t>(i)], props[static_cast<size_t>(i)]);
    normalize_node(existing[static_cast<size_t>(i)]);
    ++written;
  }

  // 2) 行数 < 节点数:真正从链表移除多余节点(新末尾 next=0;空列表则 head=0)
  if (m < k) {
    if (m > 0) {
      mem_.write_u_int32(existing[static_cast<size_t>(m - 1)] + cfg.next_offset, 0);
    } else {
      mem_.write_u_int32(item_base + cfg.head_offset, 0);
    }
    // 被移除的池节点清零(脱离链表,可被后续新增复用);堆节点不动
    for (int i = m; i < k; ++i) {
      uintptr_t addr = existing[static_cast<size_t>(i)];
      if (is_pool_node(addr)) {
        try {
          mem_.write(addr, std::vector<unsigned char>(cfg.slot_size, 0));
        } catch (...) {
        }
      }
    }
  }

  // 3) 需要新增:静态节点池追加到链表尾部
  int need = m - k;
  if (need <= 0) return {written, 0};
  // 仅当链表读全(尾部 next==0,或空链表 head==0)才允许追加
  bool can_append;
  if (k > 0) {
    uint32_t tail_next = mem_.read_u_int32(existing[static_cast<size_t>(k - 1)] + cfg.next_offset);
    can_append = tail_next == 0 || tail_next == 0xFFFFFFFF;
  } else {
    uint32_t head = mem_.read_u_int32(item_base + cfg.head_offset);
    can_append = head == 0 || head == 0xFFFFFFFF;
  }
  if (!can_append) return {written, need};

  std::set<uintptr_t> used(existing.begin(), existing.end());
  uintptr_t tail = k > 0 ? existing[static_cast<size_t>(k - 1)] : 0;
  int added = 0;
  for (int i = k; i < m; ++i) {
    uintptr_t addr = find_free_static_node(used);
    if (addr == 0) continue;  // 池满/找不到空闲槽:跳过该行,不强行写
    used.insert(addr);
    write_node(addr, props[static_cast<size_t>(i)]);
    // 初始化新节点的链表/哨兵字段(web.ct 模板布局):
    //   prev(0x04)=0(原生链表 prev 全 0);sentinel(0x08)=0xFFFFFFFF。
    //   node_flag(0x10) 由 write_node 按 p.active 写入,勿在此强制为 1。
    mem_.write_u_int32(addr + cfg.prev_offset, 0);
    mem_.write_u_int32(addr + cfg.sentinel_offset, 0xFFFFFFFF);
    // 目标进程是 32 位, 指针按低 32 位存入(显式截断, 避免 C4244 隐式窄化告警)
    const auto addr32 = static_cast<unsigned int>(addr);
    if (tail) {
      mem_.write_u_int32(tail + cfg.next_offset, addr32);
    } else {
      mem_.write_u_int32(item_base + cfg.head_offset, addr32);
    }
    tail = addr;
    ++added;
  }
  if (tail) mem_.write_u_int32(tail + cfg.next_offset, 0);
  return {written + added, need - added};
}

std::pair<uintptr_t, uint32_t> GameModder::pool_cfg() {
  // 返回当前节点池的 (base, size)。池 = 编辑器自管理的 VirtualAllocEx 动态分配
  // (对齐 web.ct):游戏不认识这块内存、不会 free,地址每次运行时分配总是有效。
  // 分配失败返回 (0,0),调用方跳过新增。首次需要时惰性分配一次,之后复用。
  const AttrListCfg& cfg = config_.current().attr_list;
  if (!pool_base_ && mem_.is_open()) {
    // 动态池:保证 128 槽(slot_size 0x40 → 0x2000,够一把 50 条词条的武器 + 余量)
    uint32_t slots = 128;
    uint32_t size = slots * cfg.slot_size;
    uintptr_t base = 0;
    try {
      base = mem_.alloc(size);
      mem_.write(base, std::vector<unsigned char>(size, 0));
    } catch (...) {
      base = 0;
    }
    if (base) {
      pool_base_ = base;
      pool_size_ = size;
    }
  }
  return {pool_base_, pool_size_};
}

bool GameModder::is_pool_node(uintptr_t addr) {
  auto pc = pool_cfg();
  return pc.first && addr >= pc.first && addr < pc.first + pc.second;
}

bool GameModder::reset_pool() {
  // 把动态节点池整体清零,所有槽位回到"全 0 = 空闲"状态。
  // 用途:游戏里丢弃过武器或读档后,旧链表已废,但游戏不认识我们的池、不会替我们清零,
  // 池槽会一直非 0 而无法复用;此时整池清零即可全部回收。
  // 注意:若仍有存活物品的链表挂在池内,清零会废掉那些词条——只在确认不再使用后调用。
  auto pc = pool_cfg();
  if (!pc.first) return false;
  try {
    mem_.write(pc.first, std::vector<unsigned char>(pc.second, 0));
  } catch (...) {
    return false;
  }
  return true;
}

uintptr_t GameModder::find_free_static_node(const std::set<uintptr_t>& used) {
  // 在节点池里找一块全 0 的 slot_size 槽(最高地址优先,对齐槽网格)。
  // 槽网格对齐 = base % slot_size,保证我们的槽互不错位。全 0 要求自然避开已用节点,
  // 绝不写非零字节。找不到返回 0。
  const AttrListCfg& cfg = config_.current().attr_list;
  uint32_t size = cfg.slot_size;
  auto pc = pool_cfg();
  if (!pc.first) return 0;
  uintptr_t pool_end = pc.first + pc.second;
  uint32_t align = static_cast<uint32_t>(pc.first % size);
  // 从池顶向下扫,且候选地址与槽网格对齐
  uintptr_t addr = pool_end - size;
  addr -= (addr - align) % size;
  while (addr >= pc.first) {
    bool ok = true;
    for (uintptr_t u : used) {
      if (addr < u + size && u < addr + size) {
        ok = false;  // 与已用节点重叠
        break;
      }
    }
    if (ok) {
      try {
        auto data = mem_.read(addr, size);
        bool all_zero =
            std::all_of(data.begin(), data.end(), [](unsigned char b) { return b == 0; });
        if (all_zero) return addr;
      } catch (...) {
      }
    }
    addr -= size;
  }
  return 0;
}

void GameModder::write_node(uintptr_t addr, const ItemProperty& p) {
  // 写节点字段值。id/color/active 为 DWORD,precent/value 为 Float(对齐 CT)。
  const AttrListCfg& cfg = config_.current().attr_list;
  mem_.write_u_int32(addr + cfg.id_offset, p.attr_id);
  mem_.write_u_int32(addr + cfg.color_offset, p.color);
  mem_.write_u_int32(addr + cfg.node_flag_offset, p.active ? 1u : 0u);  // 隐藏/激活
  // precent 在 CT 中是 Float:写 1.0/0.0,而非 uint32 1(否则游戏按 float 读得到非零垃圾值)
  mem_.write_float(addr + cfg.precent_offset, p.precent ? 1.0f : 0.0f);
  mem_.write_float(addr + cfg.value_offset, p.value);
}

void GameModder::normalize_node(uintptr_t addr) {
  // 自愈:确保 sentinel(0x08)=0xFFFFFFFF(游戏据此判定节点有效)。
  // 注意:node_flag(0x10) 是"隐藏/激活"状态位(0=隐藏/未激活->存档成 [H..],1=激活->[C..]),
  // 实测泰阿剑隐藏会心一击 flag=0 / 龙渊剑激活 flag=1。绝不能写死为 1,否则保存会把
  // 未激活的隐藏词条全部激活。此函数只修 sentinel,不动 flag。
  const AttrListCfg& cfg = config_.current().attr_list;
  try {
    if (mem_.read_u_int32(addr + cfg.sentinel_offset) != 0xFFFFFFFF)
      mem_.write_u_int32(addr + cfg.sentinel_offset, 0xFFFFFFFF);
  } catch (...) {
  }
}
}  // namespace poq
