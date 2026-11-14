// GameModder —— 关闭 / 还原 hook(从 GameModderInject 拆出)。
#include <windows.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include "Core/GameModder.h"
#include "Core/GameModderInternal.h"

namespace poq {

void GameModder::reset_state() {
  hook_addr_ = 0;
  cur_item_addr_ = 0;
  alloc_base_ = 0;
  pool_base_ = 0;
  pool_size_ = 0;
  orig_bytes_.clear();
}

bool GameModder::is_hook_injected(unsigned int pid) {
  const HookCfg& cfg = config_.current().hook;
  auto base = ProcessMemory::find_module_base(pid, cfg.module);
  if (!base) return false;
  try {
    return mem_.read(*base + cfg.offset, 1)[0] == 0xE9;
  } catch (...) {
    return false;
  }
}

GameModder::CloseResult GameModder::close() {
  // P0 不变量: 只要 hook 仍指向 cave, cave 就绝不能释放 —— 否则游戏执行到 hook 点
  // 会跳进已释放内存 -> 崩游戏。所以"还原 + 读回校验"必须先成功。
  CloseResult r;
  last_close_error_.clear();
  last_close_failed_ = false;

  const bool self_injected = mem_.is_open() && hook_addr_ && !orig_bytes_.empty();
  if (self_injected) {
    // 1) 还原原始指令
    bool restored = false;
    std::string why;
    try {
      // 还原的是可执行代码 -> 走 write_code 顺带 flush 指令缓存
      mem_.write_code(hook_addr_, orig_bytes_);
      restored = true;
    } catch (const std::exception& ex) {
      why = std::string("hook 还原写入失败: ") + ex.what();
    }
    // 2) 读回校验(写成功不等于落到目标进程)
    if (restored) {
      try {
        auto back = mem_.read(hook_addr_, static_cast<int>(orig_bytes_.size()));
        if (back != orig_bytes_) {
          restored = false;
          why = "hook 还原后校验不一致(目标进程仍指向 cave)";
        }
      } catch (const std::exception& ex) {
        restored = false;
        why = std::string("hook 还原校验读取失败: ") + ex.what();
      }
    }
    r.hook_restored = restored;
    if (!restored) {
      // 3) 还原失败: 保留 cave 与恢复所需状态, 绝不释放
      r.cave_freed = false;
      r.error = why;
      last_close_error_ = why;
      last_close_failed_ = true;
      std::string extra;
      try {
        if (pool_base_) {
          mem_.free(pool_base_);
          r.pool_freed = true;
        }
      } catch (const std::exception& ex) {
        r.pool_freed = false;
        extra = std::string("; 节点池释放失败: ") + ex.what();
      }
      r.error += extra;
      // 清本地注入状态(地址/原始字节), 但**不**释放 cave;
      // 残留的 E9 无法再还原, 只能提示用户重启游戏。
      hook_addr_ = 0;
      cur_item_addr_ = 0;
      alloc_base_ = 0;
      pool_base_ = 0;
      pool_size_ = 0;
      orig_bytes_.clear();
      mem_.close();
      return r;
    }
    // 4) 还原成功 -> 可以安全释放 cave
    try {
      if (alloc_base_) {
        mem_.free(alloc_base_);
        r.cave_freed = true;
      }
    } catch (const std::exception& ex) {
      r.cave_freed = false;
      r.error = std::string("cave 释放失败: ") + ex.what();
      last_close_error_ = r.error;
      last_close_failed_ = true;
    }
  }

  // 动态节点池(steam/v1.30 用, 需在句柄关闭前)
  try {
    if (pool_base_) {
      mem_.free(pool_base_);
      r.pool_freed = true;
    }
  } catch (const std::exception& ex) {
    r.pool_freed = false;
    if (!r.error.empty()) r.error += "; ";
    r.error += std::string("节点池释放失败: ") + ex.what();
    last_close_error_ = r.error;
    last_close_failed_ = true;
  }
  reset_state();
  mem_.close();
  return r;
}

void GameModder::close_no_throw() {
  close_no_throw_result();
}

GameModder::CloseResult GameModder::close_no_throw_result() {
  try {
    return close();
  } catch (const std::exception& ex) {
    CloseResult r;
    r.hook_restored = false;
    r.cave_freed = false;
    r.pool_freed = false;
    r.error = std::string("关闭时发生异常: ") + ex.what();
    last_close_error_ = r.error;
    last_close_failed_ = true;
    return r;
  } catch (...) {
    CloseResult r;
    r.hook_restored = false;
    r.cave_freed = false;
    r.pool_freed = false;
    r.error = "关闭时发生未知异常";
    last_close_error_ = r.error;
    last_close_failed_ = true;
    return r;
  }
}
}  // namespace poq
