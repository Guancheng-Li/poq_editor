// GameModder —— AOB 注入 / cave / 关闭还原 / 捕获冻结。
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

// ============================================================================
// 初始化:自注入(若已注入则复用)
// ============================================================================

uintptr_t GameModder::initialize(unsigned int pid) {
  // 计划 P1(#5): 换进程前必须真正清理旧进程(还原 hook + 释放远程内存),
  // 不能只 reset_state —— 那会把旧进程里仍生效的远程修改遗留成 hook 残留。
  if (pid_ != 0 && pid_ != pid && mem_.is_open()) {
    auto cr = close();
    if (!cr.hook_restored || !cr.cave_freed) {
      throw std::runtime_error(
          "切换进程失败：旧进程(" + std::to_string(pid_) + ") 的注入未能完全还原：" + cr.error +
          "。为避免残留 hook 跳向已释放内存，已中止切换；请关闭并重启游戏后重试。");
    }
  } else if (pid_ != pid) {
    reset_state();
  }
  if (!mem_.open(pid, ProcessMemory::kProcessAllAccess))
    throw std::runtime_error("OpenProcess 失败(请以管理员身份运行;若游戏刚重启请先刷新进程)");
  pid_ = pid;
  try {
    const HookCfg& cfg = config_.current().hook;
    auto base = ProcessMemory::find_module_base(pid, cfg.module);
    if (!base)
      throw std::runtime_error("未找到模块 " + cfg.module +
                               ",请确认游戏正在运行(偏移配置可改 hook.module)。");
    hook_addr_ = *base + cfg.offset;
    ensure_injection();
    return get_item_pointer();
  } catch (...) {
    mem_.close();
    throw;
  }
}

void GameModder::ensure_injection() {
  auto orig = mem_.read(hook_addr_, 5);
  if (orig[0] == 0xE9) {
    uintptr_t slot = find_cur_item_slot(hook_addr_);
    if (!slot)
      throw std::runtime_error(
          "检测到 hook 已注入(0xE9),但无法从 cave 解析 cur_item 槽。"
          "请确认注入脚本/cave 结构正常。");
    // 旧版残留自检:配置了背包门(steam/1.30)时,现有 cave 必须含对应 flag 引用;
    // 否则是更早版本工具注入的旧 cave(无边缘门),复用等于白注入——恢复原指令后重注入。
    const HookCfg& hk = config_.current().hook;
    bool stale = false;
    if (!hk.flag_module.empty() && hk.flag_offset) {
      auto fb = ProcessMemory::find_module_base(pid_, hk.flag_module);
      uint32_t want = (fb && *fb) ? static_cast<uint32_t>(*fb + hk.flag_offset) : 0;
      if (want) {
        int32_t rel = static_cast<int32_t>(orig[1]) | (static_cast<int32_t>(orig[2]) << 8) |
                      (static_cast<int32_t>(orig[3]) << 16) | (static_cast<int32_t>(orig[4]) << 24);
        uintptr_t cave_addr = static_cast<uintptr_t>((hook_addr_ + 5 + rel) & 0xFFFFFFFFULL);
        bool found = false;
        try {
          auto cave = mem_.read(cave_addr, 128);
          for (size_t i = 0; i + 4 <= cave.size() && !found; ++i) {
            uint32_t v = static_cast<uint32_t>(cave[i]) |
                         (static_cast<uint32_t>(cave[i + 1]) << 8) |
                         (static_cast<uint32_t>(cave[i + 2]) << 16) |
                         (static_cast<uint32_t>(cave[i + 3]) << 24);
            if (v == want) found = true;
          }
        } catch (...) {
        }
        stale = !found;
      }
    }
    if (stale) {
      // 从旧 cave 尾部恢复原指令 8B 44 86 <disp> 5E,再注入新版
      int32_t rel = static_cast<int32_t>(orig[1]) | (static_cast<int32_t>(orig[2]) << 8) |
                    (static_cast<int32_t>(orig[3]) << 16) | (static_cast<int32_t>(orig[4]) << 24);
      uintptr_t cave_addr = static_cast<uintptr_t>((hook_addr_ + 5 + rel) & 0xFFFFFFFFULL);
      try {
        auto cave = mem_.read(cave_addr, 128);
        for (size_t i = 0; i + 5 <= cave.size(); ++i) {
          if (cave[i] == 0x8B && cave[i + 1] == 0x44 && cave[i + 2] == 0x86 &&
              cave[i + 4] == 0x5E) {
            std::vector<unsigned char> o = {0x8B, 0x44, 0x86, cave[i + 3], 0x5E};
            mem_.write_code(hook_addr_, o);
            orig_bytes_ = o;
            inject();
            return;
          }
        }
      } catch (...) {
      }
      throw std::runtime_error("检测到旧版注入残留且无法自动恢复原指令,请重启游戏后再初始化。");
    }
    cur_item_addr_ = slot;
  } else if (is_hook_original(orig)) {
    // 原指令:自注入(用实际位移重建 cave)
    orig_bytes_ = orig;
    inject();
  } else {
    throw std::runtime_error("hook 处字节未识别:" + hex_bytes(orig) +
                             "\n期望已注入(E9 jmp)或原指令模式 8B 44 86 ?? 5E"
                             "(mov eax,[esi+eax*4+disp8]; pop esi)。"
                             "游戏版本可能不同,请在偏移配置中调整 hook。");
  }
}

bool GameModder::is_hook_original(const std::vector<unsigned char>& orig) {
  // mov eax,[esi+eax*4+disp8]; pop esi(8B 44 86 ?? 5E,注意是 0x44 不是 0x04)
  return orig.size() >= 5 && orig[0] == 0x8B && orig[1] == 0x44 && orig[2] == 0x86 &&
         orig[4] == 0x5E;
}

void GameModder::inject() {
  // 计划 P1(#6): 两阶段提交 + rollback。
  //   prepare(分配 cave / 初始化 slot / 写 cave) 失败 -> 只释放 cave, hook 未被动过;
  //   commit(patch hook) 失败 -> 先把 hook 写回原字节, 验证成功后才释放 cave。
  // 不变量与 close() 一致: 只要 hook 还指向 cave, cave 就不能释放。
  bool committed = false;
  struct Rollback {
    GameModder* self;
    bool* committed;
    ~Rollback() {
      if (*committed) return;  // 没到 commit 或 commit 后已处理, 无需回滚
      try {
        if (!self->orig_bytes_.empty() && self->hook_addr_) {
          self->mem_.write_code(self->hook_addr_, self->orig_bytes_);
          auto back = self->mem_.read(self->hook_addr_, static_cast<int>(self->orig_bytes_.size()));
          if (back == self->orig_bytes_) self->orig_bytes_.clear();  // 已还原
        }
      } catch (...) {
      }
      try {
        if (self->alloc_base_) self->mem_.free(self->alloc_base_);
      } catch (...) {
      }
      self->alloc_base_ = 0;
      self->cur_item_addr_ = 0;
    }
  } rollback{this, &committed};

  uint8_t disp = orig_bytes_[3];  // 原指令的位移字节(1E/30 等)
  const uint32_t alloc_size = 0x100;
  alloc_base_ = mem_.alloc(alloc_size, hook_addr_);
  cur_item_addr_ = alloc_base_;
  freeze_addr_ = alloc_base_ + 0x80;     // 冻结闸门 dword
  flag_last_addr_ = alloc_base_ + 0x84;  // 背包flag"上次值"持久化 dword
  flag_arm_addr_ = alloc_base_ + 0x88;   // 边缘后捕获臂计数 dword
  uintptr_t cave_addr = alloc_base_ + 4;
  mem_.write_u_int32(cur_item_addr_, 0);
  mem_.write_u_int32(freeze_addr_, 0);
  mem_.write_u_int32(flag_last_addr_, 0);
  mem_.write_u_int32(flag_arm_addr_, 0);
  uintptr_t return_addr = hook_addr_ + 5;
  // 捕获条件:未配置 capture 时按 weight 类型,用 hook.weight_offset/weight_max 兜底
  const HookCfg& hk = config_.current().hook;
  CaptureCfg cap = hk.capture ? *hk.capture : CaptureCfg{};
  if (cap.type == "weight") {
    cap.offset = hk.capture ? hk.capture->offset : hk.weight_offset;
    cap.max = hk.capture ? hk.capture->max : hk.weight_max;
  }
  // 背包拿起边缘门地址(steam: POQ.exe+0x261F84);模块解析不到则关闭该门。
  uintptr_t bag_flag_addr = 0;
  bag_flag_active_ = false;
  if (!hk.flag_module.empty() && hk.flag_offset) {
    auto fbase = ProcessMemory::find_module_base(pid_, hk.flag_module);
    if (fbase && *fbase) {
      bag_flag_addr = *fbase + hk.flag_offset;
      bag_flag_active_ = true;
      try {
        mem_.write_u_int32(flag_last_addr_, mem_.read_u_int32(bag_flag_addr));
      } catch (...) {
      }
    }
  }
  auto cave = build_cave(cur_item_addr_, cave_addr, return_addr, disp, cap, freeze_addr_,
                         bag_flag_addr, hk.flag_value, flag_last_addr_, flag_arm_addr_);
  mem_.write_code(cave_addr, cave);
  uint32_t jmp_rel = static_cast<uint32_t>(cave_addr - return_addr);
  std::vector<unsigned char> jmp;
  jmp.push_back(0xE9);
  append_le32(jmp, jmp_rel);
  // 最后一步才改执行流; 写完 flush, 保证目标 CPU 不会执行旧指令缓存
  mem_.write_code(hook_addr_, jmp);
  committed = true;  // 执行流已改: 后续异常必须先还原 hook 再释放 cave
}
}  // namespace poq
