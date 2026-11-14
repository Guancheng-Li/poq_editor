// SummonSteam 内部共享(EasyRPG 偏移 / 秦兵表 / 工具函数)。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include "Native/ProcessMemory.h"

namespace poq {
namespace summon_steam {
namespace detail {

struct QinSub {
  const char* name;
  const char* long_name;
};
struct QinGroup {
  const char* name;
  const char* short_name;
  std::vector<QinSub> subs;
};

extern uintptr_t g_mb;
extern uint32_t g_flag;

constexpr uintptr_t kOffFlag = 0x4788CC;

inline uintptr_t rt(uintptr_t off) {
  return g_mb + off;
}
inline uint32_t flag_addr() {
  return g_flag ? g_flag : static_cast<uint32_t>(rt(kOffFlag));
}

extern const int kMonSlots[10];
extern const char* kMonNames[10];

constexpr uintptr_t kOffNameShanYi = 0x2371BC;

// 类型表(EasyRPG.dll, Steam 版)。已用反汇编核对:
//   0x104bab78 = 记录数; 记录 i 起始 0x104bab80 + i*0x140
//   对应 python 的 0x53bfab78 / +8 + i*0x140 (映像基址 0x53740000)
constexpr uintptr_t kTabCountOff = 0x4BAB78;
constexpr uintptr_t kTabRecOff = 0x4BAB80;
constexpr uint32_t kTabStride = 0x140;
constexpr int kTabMaxRec = 600;

const std::vector<QinGroup>& qin_groups();

bool patch_bytes(ProcessMemory& mem, uintptr_t addr, const std::vector<unsigned char>& data,
                 const std::string& label, uintptr_t mb, std::string& log);
void patch_type(ProcessMemory& mem, uintptr_t mb, int slot, std::string& log);
void patch_bound(ProcessMemory& mem, uintptr_t mb, int val, std::string& log);
void patch_control(ProcessMemory& mem, uintptr_t mb, int val, std::string& log);
void patch_friend(ProcessMemory& mem, uintptr_t mb, int val, std::string& log);
void patch_skip_member(ProcessMemory& mem, uintptr_t mb, bool on, std::string& log);
void patch_matcher(ProcessMemory& mem, uintptr_t mb, bool on, std::string& log);

void cmd_cike(ProcessMemory& mem, bool on, std::string& log);
bool restore_qin_backup(ProcessMemory& mem);
bool verify_runtime(ProcessMemory& mem, std::string& log);
bool summon_qin_field0_tag(ProcessMemory& mem, uintptr_t mb, int group_idx, int sub_idx,
                           std::string& log);
bool summon_qin_longname(ProcessMemory& mem, uintptr_t mb, int group_idx, int sub_idx,
                         std::string& log);
std::map<std::string, std::string> parse_args(const std::vector<std::string>& args);
int parse_int(const std::string& s);
std::string legal_subs(const QinGroup& g);

}  // namespace detail
}  // namespace summon_steam
}  // namespace poq
