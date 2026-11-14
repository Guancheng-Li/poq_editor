#pragma once

#include <cstdint>

namespace poq {

class ProcessMemory;

// 秦兵 scratch: 六级高地址尝试, 失败抛错(禁止 ANY 低地址 —— 会被游戏踩崩)。
uintptr_t alloc_summon_scratch(ProcessMemory& mem);

}  // namespace poq
