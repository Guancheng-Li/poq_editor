// GM 命令 —— 双版本参数化模块。
//
// 1:1 移植 data/py/gm_cmd.py 的全部 tab 所需逻辑(激活/还原/执行/捕获/wizard.lst)。
// steam / v1.30 的区别集中在 GmVersion 常量(对齐 gm_cmd.py 的 VERSIONS 字典),
// 运行时一律 rpg/MB + offset,不拆双文件(与召唤修改的文件级隔离不同,对齐
// gm_cmd.py 的"单模块双版本参数化"形态)。
//
// 纯 C++/Win32 实现,不调用 python。
#pragma once

#include <cstdint>
#include <string>

#include "Native/ProcessMemory.h"

namespace poq {
namespace gm_cmd {

enum class Version { Steam, V130 };
enum class Method { Memory, R9 };  // 内存修改 / R9注入

/// 单版本偏移表(对齐 gm_cmd.py VERSIONS[name])。
struct GmVersion {
    std::string mod;  // EasyRPG.dll / EasyRPG116.dll
    uintptr_t player_ptr = 0;
    uintptr_t cur_ptr = 0;
    uintptr_t char1_edi = 0;
    uintptr_t user0 = 0;
    uintptr_t rights = 0;
    uintptr_t wizard = 0;   // 0 = 未定位
    uintptr_t console = 0;  // 0 = 未定位
    uintptr_t dispatch = 0;
    uintptr_t gate = 0;
    uintptr_t name_off = 0;
    bool is_steam = true;
};

const GmVersion& SteamVersion();
const GmVersion& V130Version();

/// 一次调用上下文:mem 已打开;rpg = 版本模块基址(EasyRPG*.dll);exe = POQ.exe 基址。
struct Ctx {
    ProcessMemory& mem;
    unsigned int pid = 0;
    uintptr_t rpg = 0;
    uintptr_t exe = 0;
    const GmVersion* ver = nullptr;
};

/// GM 是否已激活:玩家+0x538 >= 5 且用户表 rights >= 8。静默,不抛异常。
bool IsGmReady(Ctx& ctx);

/// 激活 GM(方案A=内存直写,自动维护 wizard.lst;方案B=R9 注入 Dispatch('r','9'))。
/// 返回工具日志(与 python print 风格一致)。玩家对象未就绪等硬错误抛
/// std::runtime_error。
std::string Activate(Ctx& ctx, Method method);

/// 还原 GM 权限(等级=1 rights=1,wizard/console 置 0)。返回工具日志。
std::string Disable(Ctx& ctx);

/// 执行一条 GM 命令(零回车,远程线程直调 dispatch)。内部静默激活 + 维护 wizard.lst。
/// 返回工具日志;命令为空 / 玩家未就绪 / 游戏进程退出抛 std::runtime_error。
std::string Send(Ctx& ctx, const std::string& text);

/// 捕获屏幕输出文本(kind: "look" / "dir" / "objlist"),返回输出栏文本;
/// 未捕获到返回空串(不抛)。命令被拒(ret!=1)也返回空串(对齐 python 返回 None)。
std::string Capture(Ctx& ctx, const char* kind);

/// 手动维护 wizard.lst(当前用户权限 9)。返回结果提示文本。
std::string FixWizard(Ctx& ctx);

}  // namespace gm_cmd
}  // namespace poq
