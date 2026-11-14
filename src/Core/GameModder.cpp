// GameModder —— 内存 dump 等薄入口(主逻辑见 inject/Fields/Attrs)。
#include "Core/GameModder.h"

namespace poq {

std::vector<unsigned char> GameModder::read_memory_dump(uintptr_t addr, int size) {
  return mem_.read(addr, size);
}
}  // namespace poq
