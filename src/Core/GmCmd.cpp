// GmCmd —— 公开 API(版本表 / activate / send / capture / fix_wizard)。
#include "Core/GmCmd.h"
#include "Core/GmCmdInternal.h"

namespace poq::gm_cmd {

using detail::capture130_text;
using detail::capture_obj_list;
using detail::capture_steam_text;
using detail::dispatch_direct;
using detail::DispatchResult;
using detail::fix_wizard_lst;
using detail::get_level;
using detail::is_process_alive;
using detail::read_rights;
using detail::send_command;
using detail::set_level;
using detail::silent_prepare;

const GmVersion& steam_version() {
  static const GmVersion v{"EasyRPG.dll", 0x478D38, 0x478D34, 0x4797FC, 0x3B5334, 0xEC,
                           0x5409F8,      0x5491B8, 0x6AF60,  0x566B0,  0x28,     true};
  return v;
}

const GmVersion& v130_version() {
  static const GmVersion v{"EasyRPG116.dll", 0x3B45D0, 0x3B45CC, 0x37459C, 0x2F773C, 0x1C4, 0, 0,
                           0x6530,           0x3CDC0,  0x28,     false};
  return v;
}

bool is_gm_ready(Ctx& ctx) {
  uint32_t lv = get_level(ctx);
  uint32_t rights = read_rights(ctx);
  return lv >= 5 && rights >= 8;
}

std::string activate(Ctx& ctx, Method method) {
  std::string log;
  if (method == Method::R9) {
    // 方案B:R9 注入 = Dispatch('r','9');顺带维护 wizard.lst(对齐 python
    // _do_activate:先 fix_wizard_lst(verbose),再 activate_gm_by_cmd)。
    fix_wizard_lst(ctx, true, log);
    DispatchResult res = dispatch_direct(ctx, "r", "9", 3000, false);
    if (res.process_gone) throw std::runtime_error("游戏进程已退出 (命令可能崩了游戏)");
    if (res.timed_out) log += "[!!] 远程线程超时未返回\n";
  } else {
    // 方案A:内存直写权限字段 + 静默建立 wizard.lst(对齐 cmd_enable)
    bool ok1 = set_level(ctx, 9);
    ctx.mem.write_u_int32(ctx.rpg + ctx.ver->user0 + ctx.ver->rights, 9);
    if (ctx.ver->wizard)
      ctx.mem.write_u_int32(ctx.exe + ctx.ver->wizard,
                            static_cast<uint32_t>(ctx.rpg + ctx.ver->user0));
    if (ctx.ver->console) ctx.mem.write_u_int32(ctx.exe + ctx.ver->console, 1);
    log += "[enable] 等级=" + std::to_string(get_level(ctx)) +
           " rights=" + std::to_string(read_rights(ctx)) + " " + (ok1 ? "OK" : "(玩家对象未就绪)") +
           "\n";
    if (!ok1) log += "  [!!] 玩家对象指针未定位 (游戏可能未进入主界面)\n";
    fix_wizard_lst(ctx, true, log);
  }
  return log;
}

std::string disable(Ctx& ctx) {
  set_level(ctx, 1);
  ctx.mem.write_u_int32(ctx.rpg + ctx.ver->user0 + ctx.ver->rights, 1);
  if (ctx.ver->wizard) ctx.mem.write_u_int32(ctx.exe + ctx.ver->wizard, 0);
  if (ctx.ver->console) ctx.mem.write_u_int32(ctx.exe + ctx.ver->console, 0);
  return "[disable] 已还原: 等级=1 rights=1";
}

std::string send(Ctx& ctx, const std::string& text) {
  if (text.empty()) throw std::runtime_error("命令为空");
  silent_prepare(ctx);
  std::string token = text;
  std::string rest;
  size_t sp = text.find(' ');
  if (sp != std::string::npos) {
    token = text.substr(0, sp);
    rest = text.substr(sp + 1);
  }
  std::string log = "[send] 直调 '" + token + "' '" + rest + "'... (版本 " +
                    (ctx.ver->is_steam ? "steam" : "130") + ")\n";
  DispatchResult res = send_command(ctx, token, rest);
  if (res.process_gone) {
    if (!is_process_alive(ctx.pid)) throw std::runtime_error("游戏进程已退出 (命令可能崩了游戏)");
    log += "=> 返回 -1 (未找到命令或被拒. 1.30: clear/r 等控制台命令不在 GM 表)\n";
    return log;
  }
  if (res.timed_out) {
    log += "[!!] 远程线程超时未返回 - 命令可能挂住, 游戏可能不稳定\n";
    return log;
  }
  log += "=> 返回 " + std::to_string(res.value) + " (" + (res.value ? "成功" : "失败/被拒") + ")\n";
  return log;
}

std::string capture(Ctx& ctx, const char* kind) {
  std::string k = kind ? kind : "";
  if (k == "objlist") return capture_obj_list(ctx);
  bool is_look = (k == "look");
  silent_prepare(ctx);
  DispatchResult res = send_command(ctx, k, "");
  if (res.value != 1) return "";  // 命令被拒 -> 无输出(对齐 python 返回 None)
  if (!ctx.ver->is_steam) {
    auto t = capture130_text(ctx, is_look, 4.0);
    return t ? *t : "";
  }
  return capture_steam_text(ctx, is_look);
}

std::string fix_wizard(Ctx& ctx) {
  std::string log;
  int r = fix_wizard_lst(ctx, true, log);
  if (r == 1) return "已写入/已把权限改为 9 (重启游戏后生效)\n" + log;
  if (r == 0) return "已含 '9 <昵称>', 无需改动\n" + log;
  return "未修改 (见上方原因)\n" + log;
}
}  // namespace poq::gm_cmd
