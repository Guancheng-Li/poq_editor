#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "Core/Summon130.h"  // WildBeastRow(与 v1.30 共用结构)
#include "Native/ProcessMemory.h"

namespace poq {
namespace summon_steam {

/// 执行一次命名参数召唤(对齐 data/py/summon_tool_steam.py 的 cmd_summon, Steam 版)。
///
/// 参数形式: --group 0|1 [--mon_type N | --assassin_type N | --soldier_type N
/// [--soldier_sub_type N] | --type_table_rec N](四类互斥,冲突回退 --mon_type 0=狼)。
///
/// 返回多行日志(与 python 工具 print 风格一致)。环境/参数错误抛
/// std::runtime_error;补丁写入失败仅记入日志(与 python patch_bytes 一致)。
std::string cmd_summon(ProcessMemory& mem, uintptr_t module_base,
                       const std::vector<std::string>& args);

/// 扫 Steam 版类型表, 返回全部记录(rec + field0/field1), 供"召唤其它"列表使用。
std::vector<summon_130::WildBeastRow> list_misc_type_table(ProcessMemory& mem,
                                                           uintptr_t module_base);

}  // namespace summon_steam
}  // namespace poq
