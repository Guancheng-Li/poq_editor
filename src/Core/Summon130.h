#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Native/ProcessMemory.h"

namespace poq {
namespace summon_130 {

/// 执行一次命名参数召唤(对齐 data/py/summon_tool_130.py 的 cmd_summon, v1.30 版)。
///
/// 参数形式与 Steam 版一致;v1.30 仅支持 mon_type(0-7) 与 soldier_type(0=随机秦兵 /
/// 1-6 大类),不支持刺客;朱厌(8)/善翼(9) 映射保留但 UI 不显示。
///
/// 返回多行日志(与 python 工具 print 风格一致)。环境/参数错误抛
/// std::runtime_error;补丁写入失败仅记入日志(与 python patch_bytes 一致)。
std::string CmdSummon(ProcessMemory& mem, uintptr_t module_base,
                      const std::vector<std::string>& args);

}  // namespace summon_130
}  // namespace poq
