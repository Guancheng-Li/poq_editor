#include "Core/SummonScratch.h"
#include <stdexcept>
#include "Native/ProcessMemory.h"

namespace poq {

uintptr_t alloc_summon_scratch(ProcessMemory& mem) {
  const uintptr_t kBases[] = {0x70000000, 0x60000000, 0x50000000,
                              0x40000000, 0x30000000, 0x20000000};
  for (uintptr_t b : kBases) {
    try {
      if (uintptr_t a = mem.alloc(0x4000, b)) return a;
    } catch (...) {
    }
  }
  throw std::runtime_error(
      "秦兵 scratch 分配失败: 0x20000000~0x70000000 六级高地址全部被占用。"
      "为避免崩溃已放弃本次召唤(不落低地址), 请重启游戏后再试。");
}

}  // namespace poq
