// Steam 召唤 —— 内存补丁段(写后回读校验)。
#include "Core/SummonSteamInternal.h"
#include <cstdio>
#include <string>
#include <vector>
#include "Core/TextCodec.h"

namespace poq {
namespace summon_steam {
namespace detail {

namespace {

constexpr uintptr_t kOffFriendPush = 0x35BAA;
constexpr uintptr_t kOffBound = 0x3586D;
constexpr uintptr_t kOffControlCmp = 0x35C32;
constexpr uintptr_t kOffType = 0x3585F;
constexpr uintptr_t kOffMatcherImm = 0x6FAC6;

const char* kBeastSlots[11] = {"狼", "虎",   "熊",   "鸟",   "白狼", "木偶",
                               "狗", "虎蛟", "狍鸮", "朱厌", "善翼"};

std::string hex8_local(uintptr_t a) {
  char b[32];
  std::snprintf(b, sizeof(b), "%08llX", static_cast<unsigned long long>(a));
  return b;
}

std::string hex8_bytes(const std::vector<unsigned char>& b) {
  std::string out;
  for (unsigned char c : b) {
    char t[4];
    std::snprintf(t, sizeof(t), "%02x ", c);
    out += t;
  }
  if (!out.empty()) out.pop_back();
  return out;
}

std::vector<unsigned char> make_control_cmp(unsigned char imm) {
  uint32_t f = flag_addr();
  return {0x83,
          0x3d,
          static_cast<unsigned char>(f & 0xFF),
          static_cast<unsigned char>((f >> 8) & 0xFF),
          static_cast<unsigned char>((f >> 16) & 0xFF),
          static_cast<unsigned char>((f >> 24) & 0xFF),
          imm};
}

int get_control_val(ProcessMemory& mem, uintptr_t mb) {
  try {
    std::vector<unsigned char> op = mem.read(mb + kOffControlCmp, 1);
    if (op[0] == 0x83) {
      std::vector<unsigned char> b = mem.read(mb + kOffControlCmp + 6, 1);
      return b[0];
    }
  } catch (...) {
  }
  return 0x0a;
}

}  // namespace

bool patch_bytes(ProcessMemory& mem, uintptr_t addr, const std::vector<unsigned char>& data,
                 const std::string& label, uintptr_t mb, std::string& log) {
  std::string off = mb ? " (MB+0x" + hex8_local(addr - mb) + ")" : "";
  try {
    std::vector<unsigned char> old = mem.read(addr, static_cast<int>(data.size()));
    mem.write(addr, data);
    std::vector<unsigned char> nw = mem.read(addr, static_cast<int>(data.size()));
    if (nw == data) {
      log += "  [OK] " + label + " @0x" + hex8_local(addr) + off;
      log += ": " + hex8_bytes(old) + " -> " + hex8_bytes(data) + "\n";
      return true;
    }
    log +=
        "  [!!] " + label + " 写入后校验失败: " + hex8_bytes(nw) + " != " + hex8_bytes(data) + "\n";
  } catch (const std::exception& e) {
    log += "  [!!] " + label + " 写入异常: " + e.what() + "\n";
  }
  return false;
}

void patch_type(ProcessMemory& mem, uintptr_t mb, int slot, std::string& log) {
  std::vector<unsigned char> data;
  uint32_t v = static_cast<uint32_t>(slot + 1);
  data.push_back(0xb8);
  data.push_back(static_cast<unsigned char>(v & 0xFF));
  data.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
  data.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
  data.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
  const char* nm = (slot >= 0 && slot < 11) ? kBeastSlots[slot] : "?";
  patch_bytes(mem, mb + kOffType, data,
              "强制召唤类型 slot=" + std::to_string(slot) + "(" + nm + ")", mb, log);
}

void patch_bound(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
  std::vector<unsigned char> data = {0x83, 0xf8, static_cast<unsigned char>(val & 0xFF)};
  patch_bytes(mem, mb + kOffBound, data, "type bound=0x" + hex8_local(val), mb, log);
}

void patch_control(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
  patch_bytes(mem, mb + kOffControlCmp, make_control_cmp(static_cast<unsigned char>(val & 0xFF)),
              "可控性 cmp 0x" + hex8_local(val), mb, log);
}

void patch_friend(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
  std::vector<unsigned char> data = {static_cast<unsigned char>(val & 0xFF)};
  patch_bytes(mem, mb + kOffFriendPush, data, "召唤强制 Friend imm", mb, log);
}

void patch_skip_member(ProcessMemory& mem, uintptr_t mb, bool on, std::string& log) {
  if (on) {
    std::vector<unsigned char> data = {0xEB, 0x20, 0x90, 0x90, 0x90, 0x90};
    patch_bytes(mem, mb + kOffControlCmp, data, "跳过 AddMember 注册(不可控)", mb, log);
  } else {
    int ctrl = get_control_val(mem, mb);
    patch_bytes(mem, mb + kOffControlCmp, make_control_cmp(static_cast<unsigned char>(ctrl & 0xFF)),
                "恢复 AddMember 注册", mb, log);
  }
}

void patch_matcher(ProcessMemory& mem, uintptr_t mb, bool on, std::string& log) {
  std::vector<unsigned char> data = {on ? static_cast<unsigned char>(1)
                                        : static_cast<unsigned char>(0)};
  patch_bytes(mem, mb + kOffMatcherImm, data,
              on ? "匹配器 -> field[1](短名)" : "匹配器 -> field[0](长名/原版)", mb, log);
}

}  // namespace detail
}  // namespace summon_steam
}  // namespace poq
