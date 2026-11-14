#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "Native/ProcessMemory.h"

namespace poq {
namespace summon_130 {

/// 执行一次命名参数召唤(对齐 data/py/summon_tool_130.py 的 cmd_summon, v1.30 版)。
///
/// 参数形式与 Steam 版一致; 支持 mon_type(0-9)、assassin、秦兵; 朱厌/善翼可选
/// --beast_wild 1 从类型表 field[0] 召唤(见 list_beast_type_table)。
///
/// 返回多行日志(与 python 工具 print 风格一致)。环境/参数错误抛
/// std::runtime_error;补丁写入失败仅记入日志(与 python patch_bytes 一致)。
std::string cmd_summon(ProcessMemory& mem, uintptr_t module_base,
                       const std::vector<std::string>& args);

/// 取消激活: 仅还原 python 补丁点, 不再写入 mov eax/slot.
std::string cmd_restore_vanilla(ProcessMemory& mem, uintptr_t module_base);

struct WildBeastRow {
  int rec = 0;
  std::string field0;
  std::string field1;
  int gbk_len = 0;
};

/// mon_type 8=朱厌 / 9=善翼: 扫类型表 field[0] 长名含关键字的所有 rec (需已 OpenProcess)。
std::vector<WildBeastRow> list_beast_type_table(ProcessMemory& mem, uintptr_t module_base,
                                                int mon_type);

/// 类型表 NPC/怪物 (运行时 MB+类型表); 排除召唤兽/野生兽系(动物·怪兽)/秦兵/刺客/朱厌善翼/占位行; 长名+短名去重。
std::vector<WildBeastRow> list_misc_type_table(ProcessMemory& mem, uintptr_t module_base);

}  // namespace summon_130
}  // namespace poq
