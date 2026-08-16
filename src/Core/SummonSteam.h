#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Native/ProcessMemory.h"

namespace poq {
namespace summon_steam {

/// 执行一次命名参数召唤(对齐 data/py/summon_tool_steam.py 的 cmd_summon, Steam 版)。
///
/// 参数形式: --group 0|1 [--mon_type N | --assassin_type N | --soldier_type N
/// [--soldier_sub_type N]](三类互斥,冲突回退 --mon_type 0=狼)。
///
/// 返回多行日志(与 python 工具 print 风格一致)。环境/参数错误抛
/// std::runtime_error;补丁写入失败仅记入日志(与 python patch_bytes 一致)。
std::string CmdSummon(ProcessMemory& mem, uintptr_t module_base,
                      const std::vector<std::string>& args);

}  // namespace summon_steam
}  // namespace poq
