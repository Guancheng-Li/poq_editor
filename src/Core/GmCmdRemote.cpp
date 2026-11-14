// GmCmd —— 权限字段 / 远程 dispatch / wizard.lst / silent_prepare。
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <utility>
#include "Core/GmCmdInternal.h"

namespace poq::gm_cmd::detail {

struct RemoteBufGuard {
  ProcessMemory* mem = nullptr;
  uintptr_t addr = 0;
  RemoteBufGuard(ProcessMemory& m, uintptr_t a) : mem(&m), addr(a) {}
  ~RemoteBufGuard() {
    if (addr) {
      try {
        mem->free(addr);
      } catch (...) {
      }
    }
  }
  RemoteBufGuard(const RemoteBufGuard&) = delete;
  RemoteBufGuard& operator=(const RemoteBufGuard&) = delete;
  void dismiss() { addr = 0; }
};

// ---------------------------------------------------------------------------
// 权限字段
// ---------------------------------------------------------------------------
uint32_t get_level(Ctx& ctx) {
  uintptr_t obj = 0;
  try {
    obj = ctx.mem.read_u_int32(ctx.rpg + ctx.ver->player_ptr);
  } catch (...) {
    return 0;
  }
  if (!obj) return 0;
  try {
    return ctx.mem.read_u_int32(obj + kOffPlayerLev);
  } catch (...) {
    return 0;
  }
}

bool set_level(Ctx& ctx, uint32_t lv) {
  uintptr_t obj = 0;
  try {
    obj = ctx.mem.read_u_int32(ctx.rpg + ctx.ver->player_ptr);
  } catch (...) {
    return false;
  }
  if (!obj) return false;
  ctx.mem.write_u_int32(obj + kOffPlayerLev, lv);
  return true;
}

uint32_t read_rights(Ctx& ctx) {
  try {
    return ctx.mem.read_u_int32(ctx.rpg + ctx.ver->user0 + ctx.ver->rights);
  } catch (...) {
    return 0;
  }
}

// ---------------------------------------------------------------------------
// 远程线程直调 dispatch(token, rest)(对齐 dispatch_direct / dispatch_direct_130)
// ---------------------------------------------------------------------------
DispatchResult dispatch_direct(Ctx& ctx, const std::string& token, const std::string& rest,
                               int wait_ms, bool signextend) {
  ProcessMemory& mem = ctx.mem;
  // 玩家对象未就绪 -> 硬错误(对齐 python sys.exit)
  uintptr_t player = 0;
  try {
    player = mem.read_u_int32(ctx.rpg + ctx.ver->player_ptr);
  } catch (...) {
  }
  if (!player) throw std::runtime_error("玩家对象未就绪 (读档进入游戏后再试)");
  // cur_ptr 会陈旧(读档后玩家对象重建)或为 0,必须每次用最新玩家对象刷新,
  // 否则门卫取到失效指针 -> handler 被拒。
  mem.write_u_int32(ctx.rpg + ctx.ver->cur_ptr, static_cast<uint32_t>(player));

  // 分配远程内存(RWX),布局:base+0x0 token GBK / +0x40 rest GBK /
  // +0x200 stub / +0x400 ret_slot。优先 near 分配(便于 32 位 rel32 落 ±2GB)。
  uintptr_t base = 0;
  try {
    base = mem.alloc(0x1000, ctx.rpg);
  } catch (...) {
    try {
      base = mem.alloc(0x1000);
    } catch (...) {
      throw std::runtime_error("VirtualAllocEx 失败");
    }
  }
  // 计划 260929 #16: 分配即接管, 之后任何 throw 都由 guard 自动释放。
  RemoteBufGuard base_guard(mem, base);
  try {
    // 计划 P1(#9): 远程缓冲是固定布局, 必须按 GBK 编码后校验边界 ——
    // UTF-8 长度 != GBK 长度, 只靠 UI 输入框长度挡不住越界(会覆盖 stub/ret_slot)。
    // 布局: base+0x000 token / +0x040 rest / +0x200 stub / +0x400 ret_slot
    constexpr size_t kTokenOff = 0x000, kRestOff = 0x040;
    constexpr size_t kStubOff = 0x200;
    std::string t = utf8_to_gbk(token);
    t.push_back('\0');
    std::string r = utf8_to_gbk(rest);
    r.push_back('\0');
    if (t.size() > kRestOff - kTokenOff) {
      throw std::runtime_error("GM 命令 token 过长: GBK " + std::to_string(t.size()) +
                               " 字节, 上限 " + std::to_string(kRestOff - kTokenOff - 1));
    }
    if (r.size() > kStubOff - kRestOff) {
      throw std::runtime_error("GM 命令参数过长: GBK " + std::to_string(r.size()) + " 字节, 上限 " +
                               std::to_string(kStubOff - kRestOff - 1));
    }
    mem.write(base + kTokenOff, to_bytes(t));
    mem.write(base + kRestOff, to_bytes(r));
    mem.write_u_int32(base + 0x400, 0);
    uintptr_t stub = base + 0x200;
    uintptr_t ret_slot = base + 0x400;
    // push rest(5) -> push token(5) -> call dispatch(5, rel) ->
    // add esp,8(3) -> mov [ret_slot],eax(5) -> ret(1)  (共 24 字节)
    std::vector<unsigned char> code;
    code.push_back(0x68);
    append_le32(code, static_cast<uint32_t>(base + 0x40));
    code.push_back(0x68);
    append_le32(code, static_cast<uint32_t>(base));
    int32_t rel = static_cast<int32_t>((ctx.rpg + ctx.ver->dispatch) - (stub + code.size() + 5));
    code.push_back(0xE8);
    append_le32(code, static_cast<uint32_t>(rel));
    code.push_back(0x83);
    code.push_back(0xC4);
    code.push_back(0x08);
    code.push_back(0xA3);
    append_le32(code, static_cast<uint32_t>(ret_slot));
    code.push_back(0xC3);
    mem.write(stub, code);

    auto rr = mem.call_remote_thread_detailed(stub, wait_ms, ret_slot);
    DispatchResult res;
    if (!is_process_alive(ctx.pid)) {
      res.process_gone = true;
    } else if (rr.status == ProcessMemory::RemoteCallStatus::kTimedOutStillRunning) {
      // 计划 P1/P2(#8): 超时强杀后目标进程状态不可信 -> 明确标记, 提示用户重启游戏,
      // 且**不能**释放仍可能被该线程访问的 stub/buffer(见下方 base 释放分支)。
      res.timed_out = true;
    } else if (!rr.ok()) {
      res.timed_out = false;
      res.failed = true;
    } else {
      // 1.30 返回值有符号化(uint32 -> int32);steam 保留原始无符号值。
      uint32_t u = static_cast<uint32_t>(rr.value);
      res.value =
          signextend ? static_cast<int64_t>(static_cast<int32_t>(u)) : static_cast<int64_t>(u);
    }
    // 正常/失败但线程已结束: guard 析构释放远程内存(对齐 python 每次 VirtualFreeEx)。
    // 超时强杀时**不释放** —— 线程可能仍在执行 stub 里的代码, 释放会让它跳进已释放内存。
    if (res.timed_out) base_guard.dismiss();
    return res;
  } catch (...) {
    throw;  // base_guard 析构统一释放
  }
}

DispatchResult send_command(Ctx& ctx, const std::string& token, const std::string& rest) {
  if (ctx.ver->is_steam) return dispatch_direct(ctx, token, rest, 3000, false);
  return dispatch_direct(ctx, token, rest, 5000, true);
}

// ---------------------------------------------------------------------------
// wizard.lst 维护(对齐 read_first_alias / fix_wizard_lst / get_module_path)
// ---------------------------------------------------------------------------
std::string read_first_alias(Ctx& ctx) {
  // alias_style == "edi":槽1 EDI=[rpg+char1_edi],结构体=EDI+0x28,别名指针=+0x04(GBK)
  uintptr_t edi = 0;
  try {
    edi = ctx.mem.read_u_int32(ctx.rpg + ctx.ver->char1_edi);
  } catch (...) {
  }
  if (edi && edi > 0x10000 && edi < 0xFFFFFF00) {
    uintptr_t p = 0;
    try {
      p = ctx.mem.read_u_int32(edi + 0x28 + 0x04);
    } catch (...) {
    }
    if (p && p > 0x10000 && p < 0xFFFFFF00) {
      std::string s = read_gbk_ptr(ctx.mem, p, 48);
      if (!s.empty()) return s;
    }
  }
  // 通用回退:主玩家对象 + name_off(显示名,聊胜于无)
  uintptr_t obj = 0;
  try {
    obj = ctx.mem.read_u_int32(ctx.rpg + ctx.ver->player_ptr);
  } catch (...) {
  }
  if (obj && obj > 0x10000 && obj < 0xFFFFFF00) {
    uintptr_t p = 0;
    try {
      p = ctx.mem.read_u_int32(obj + ctx.ver->name_off);
    } catch (...) {
    }
    if (p && p > 0x10000 && p < 0xFFFFFF00) {
      std::string s = read_gbk_ptr(ctx.mem, p, 48);
      if (!s.empty()) return s;
    }
  }
  return "";
}

// 静默确保 <游戏目录>\System\wizard\wizard.lst 当前用户权限为 9。
// 返回:1=已写入/已改9, 0=已含9未动, -1=失败/跳过。
int fix_wizard_lst(Ctx& ctx, bool verbose, std::string& log) {
  auto say = [&](const std::string& msg) {
    if (verbose) log += "[wizard] " + msg + "\n";
  };
  try {
    std::string alias = read_first_alias(ctx);
    if (alias.empty()) {
      say("未能读到第一个角色别名 (游戏可能未读档)");
      return -1;
    }
    auto exe_path = ProcessMemory::find_module_path(ctx.pid, "POQ.exe");
    if (!exe_path) {
      say("未能获取 POQ.exe 模块路径");
      return -1;
    }
    std::wstring exe_dir = utf8_to_wide(*exe_path);
    size_t pos = exe_dir.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
      say("模块路径无效");
      return -1;
    }
    std::wstring lst = exe_dir.substr(0, pos) + L"\\System\\wizard\\wizard.lst";
    // 读原文件(GBK,二进制)
    std::string raw;
    if (::GetFileAttributesW(lst.c_str()) != INVALID_FILE_ATTRIBUTES) {
      FILE* f = _wfopen(lst.c_str(), L"rb");
      if (f) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
        fclose(f);
      }
    }
    std::string text = gbk_to_utf8_replace(raw);
    std::vector<std::string> lines = split_lines(text);
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    // 已含 "9 别名" 行 -> 无需改动(严格匹配权限号为 9,低权限行不算)
    for (const auto& line : lines) {
      std::vector<std::string> parts = split_ws(line);
      if (parts.size() >= 2 && parts[0] == "9")
        for (size_t k = 1; k < parts.size(); ++k)
          if (parts[k] == alias) {
            say("wizard.lst 已含 '9 " + alias + "', 不修改");
            return 0;
          }
    }
    // 换行风格:原文件 \r\n 则沿用,新建默认 \r\n
    bool crlf = raw.find("\r\n") != std::string::npos;
    std::string nl = (raw.empty() || crlf) ? "\r\n" : "\n";
    bool changed = false;
    for (auto& line : lines) {
      std::vector<std::string> parts = split_ws(line);
      if (parts.size() >= 2)
        for (size_t k = 1; k < parts.size(); ++k)
          if (parts[k] == alias) {
            std::string rest;
            for (size_t x = 1; x < parts.size(); ++x) rest += (x > 1 ? " " : "") + parts[x];
            line = "9 " + rest;
            changed = true;
            break;
          }
      if (changed) break;
    }
    if (!changed) lines.push_back("9 " + alias);
    // 写回(GBK,保证每行含末行都以换行结尾)
    std::filesystem::path dir = std::filesystem::path(lst).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string data;
    for (size_t i = 0; i < lines.size(); ++i) {
      if (i) data += nl;
      data += lines[i];
    }
    data += nl;
    std::string gbk = utf8_to_gbk(data);
    FILE* f = _wfopen(lst.c_str(), L"wb");
    if (!f) {
      say("写文件失败");
      return -1;
    }
    fwrite(gbk.data(), 1, gbk.size(), f);
    fclose(f);
    if (changed)
      say("已将 " + alias + " 的权限改为 9");
    else
      say("已追加 '9 " + alias + "'");
    return 1;
  } catch (const std::exception& e) {
    say(std::string("失败: ") + e.what());
    return -1;
  }
}

// ---------------------------------------------------------------------------
// 静默准备(对齐 silent_prepare)
// ---------------------------------------------------------------------------
void ensure_gm(Ctx& ctx) {
  if (!is_gm_ready(ctx)) {
    // activate_gm_by_mem_write(方案A)
    set_level(ctx, 9);
    ctx.mem.write_u_int32(ctx.rpg + ctx.ver->user0 + ctx.ver->rights, 9);
    if (ctx.ver->wizard)
      ctx.mem.write_u_int32(ctx.exe + ctx.ver->wizard,
                            static_cast<uint32_t>(ctx.rpg + ctx.ver->user0));
    if (ctx.ver->console) ctx.mem.write_u_int32(ctx.exe + ctx.ver->console, 1);
  }
}

void silent_prepare(Ctx& ctx) {
  try {
    ensure_gm(ctx);
  } catch (...) {
  }
  try {
    std::string log;
    fix_wizard_lst(ctx, false, log);
  } catch (...) {
  }
}
}  // namespace poq::gm_cmd::detail
