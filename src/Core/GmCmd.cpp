// GM 命令工具 —— steam / v1.30 双版本参数化(1:1 移植 data/py/gm_cmd.py)。
//
// 机制(对齐 python):
// - 远程线程直调★真分发器 dispatch(token, rest) 零回车执行 GM 命令(两版一致;
//   1.30 的短名 exp/c/c_c 由分发器四轮匹配原生支持)。
// - 激活方案A=内存直写权限字段(玩家+0x538/用户表 rights/wizard/console),
//   方案B=Dispatch('r','9');send 前自动静默激活 + 维护 wizard.lst。
// - 输出捕获:steam 是连续 GBK 串,scan_gbk_strings 全内存扫取;
//   1.30 是散行(NUL 结尾短行+\t 前缀),capture_130_text 按地址锚点重组。
// - wizard.lst 维护:读首角色别名(槽1 EDI),定位 POQ.exe 路径,写
//   <游戏目录>\System\wizard\wizard.lst(GBK 编解码,中文目录宽路径)。

#include "Core/GmCmd.h"

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
#include <vector>

namespace poq {
namespace gm_cmd {

namespace {

// ---------------------------------------------------------------------------
// 常量(对齐 gm_cmd.py)
// ---------------------------------------------------------------------------
constexpr uintptr_t kOffPlayerLev = 0x538;  // 玩家对象 +0x538 = 权限等级
constexpr uintptr_t kOffMoney = 0x54;       // 玩家对象 +0x54 = 金钱镜像(未用到)

const char* kMarkExits = "这里的出口有";
const char* kMarkObjs = "的人和物品有";
const char* kObjIdPrefix = "(16777";  // 对象 ID 前缀字节(0x1000000 起)

// 1.30 屏幕输出重组(对齐 gm_cmd.py capture_130_text 的正则,std::regex 默认 ECMAScript)。
const std::regex& ReScene() {
    static const std::regex re(R"(^(.{2,20})\s*\(([^()\s:]+):(\d+)\)\s*$)");
    return re;
}
const std::regex& ReExit() {
    static const std::regex re(R"(^\t?(\w+)\s*:\s*(.+?)<(\d+)>(\s*<\d+>)?\s*$)");
    return re;
}
const std::regex& ReObjHead() {
    static const std::regex re(R"(这里的人和物品有\s*:?\s*\((\d+)\))");
    return re;
}
const std::regex& ReObj() {
    static const std::regex re(R"(^\t?(.+?)\s*\(([^()\s]+)\)\s*$)");
    return re;
}
// objlist 条目行(对齐 build_objdb)。
const std::regex& ReObjEntry() {
    static const std::regex re(R"(([^\n\t:()]+?)\(([^()\n:]*)\)\s*:\s*([^()\n]*?)\((\d+)\))");
    return re;
}
// look 统计(对齐 _look_stats): "这里的人和物品有 (N)" 之后的 "名字 (别名)" 行。
const std::regex& ReLookCount() {
    static const std::regex re(std::string(kMarkObjs) + R"(\s*:?\s*\((\d+)\))");
    return re;
}
const std::regex& ReLookObj() {
    static const std::regex re(R"(^[^\s()]+\s*\([^()\s]+\)$)");
    return re;
}

// ---------------------------------------------------------------------------
// 小工具(模块自包含)
// ---------------------------------------------------------------------------
std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// CP936(GBK) -> UTF-8,失败返回空串。
std::string GbkToUtf8(const char* bytes, size_t n) {
    if (n == 0) return "";
    int wlen = ::MultiByteToWideChar(936, 0, bytes, static_cast<int>(n), nullptr, 0);
    if (wlen <= 0) return "";
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(936, 0, bytes, static_cast<int>(n), &w[0], wlen);
    int ulen = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return "";
    std::string out(static_cast<size_t>(ulen), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, &out[0], ulen, nullptr, nullptr);
    return out;
}

// UTF-8 -> CP936(GBK),失败返回空串。
std::string Utf8ToGbk(const std::string& s) {
    if (s.empty()) return "";
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wlen <= 0) return "";
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], wlen);
    int blen = ::WideCharToMultiByte(936, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (blen <= 0) return "";
    std::string out(static_cast<size_t>(blen), '\0');
    ::WideCharToMultiByte(936, 0, w.data(), wlen, &out[0], blen, nullptr, nullptr);
    return out;
}

// GBK(含替换) -> UTF-8,供 wizard.lst 等宽松解码用。
std::string GbkToUtf8Replace(const std::string& s) {
    if (s.empty()) return "";
    int wlen = ::MultiByteToWideChar(936, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wlen <= 0) return s;  // 完全无法解码:原样返回
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(936, 0, s.data(), static_cast<int>(s.size()), &w[0], wlen);
    int ulen = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return s;
    std::string out(static_cast<size_t>(ulen), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, &out[0], ulen, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring out(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], wlen);
    return out;
}

// 计划 260929 #16: 远程缓冲 RAII —— 出作用域(含异常路径)自动释放, 免得每次
// throw 前手工 mem.Free。超时路径必须 Dismiss(): 线程可能仍在执行 stub, 释放
// 会让它跳进已释放内存。
struct RemoteBufGuard {
    ProcessMemory* mem = nullptr;
    uintptr_t addr = 0;
    RemoteBufGuard(ProcessMemory& m, uintptr_t a) : mem(&m), addr(a) {}
    ~RemoteBufGuard() {
        if (addr) {
            try {
                mem->Free(addr);
            } catch (...) {
            }
        }
    }
    RemoteBufGuard(const RemoteBufGuard&) = delete;
    RemoteBufGuard& operator=(const RemoteBufGuard&) = delete;
    void Dismiss() { addr = 0; }
};

void AppendLe32(std::vector<unsigned char>& v, uint32_t x) {
    v.push_back(static_cast<unsigned char>(x & 0xFF));
    v.push_back(static_cast<unsigned char>((x >> 8) & 0xFF));
    v.push_back(static_cast<unsigned char>((x >> 16) & 0xFF));
    v.push_back(static_cast<unsigned char>((x >> 24) & 0xFF));
}

// 进程是否还活着(按 pid)。
bool IsProcessAlive(unsigned int pid) {
    for (const auto& p : ProcessMemory::EnumerateProcesses())
        if (p.pid == pid) return true;
    return false;
}

// 读 NUL 结尾 GBK 字符串(UTF-8 返回)。读不到/空返回 ""。
std::string ReadGbkPtr(ProcessMemory& mem, uintptr_t a, int n) {
    std::vector<unsigned char> b;
    try {
        b = mem.Read(a, n);
    } catch (...) {
        return "";
    }
    size_t z = 0;
    while (z < b.size() && b[z] != 0) ++z;
    std::string s = GbkToUtf8(reinterpret_cast<const char*>(b.data()), z);
    return Trim(s);
}

std::vector<unsigned char> ToBytes(const std::string& s) {
    return std::vector<unsigned char>(s.begin(), s.end());
}

// 在 data 里从 off 起找 kw(字节序列),返回位置或 npos。
size_t FindBytes(const std::vector<unsigned char>& data, const std::string& kw, size_t off) {
    if (kw.empty() || off >= data.size()) return std::string::npos;
    auto it = std::search(data.begin() + static_cast<std::ptrdiff_t>(off), data.end(), kw.begin(),
                          kw.end());
    if (it == data.end()) return std::string::npos;
    return static_cast<size_t>(it - data.begin());
}

// [start, end) 内最后一个 NUL 的位置(找不到返回 end 之前的哨兵 start-1)。
size_t RFindNul(const std::vector<unsigned char>& data, size_t start, size_t end) {
    if (end == 0 || start >= end) return start - 1;
    size_t i = end;
    while (i > start) {
        --i;
        if (data[i] == 0) return i;
    }
    return start - 1;
}

// 从 off 起第一个 NUL 的位置(找不到返回 data.size()。
size_t FindNulFrom(const std::vector<unsigned char>& data, size_t off) {
    for (size_t i = off; i < data.size(); ++i)
        if (data[i] == 0) return i;
    return data.size();
}

// 按行拆分(处理 \r\n / \r / \n)。
std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            out.push_back(cur);
            cur.clear();
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() || text.empty()) out.push_back(cur);
    return out;
}

// 按空白切词。
std::vector<std::string> SplitWs(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a < s.size()) {
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
        size_t b = a;
        while (b < s.size() && s[b] != ' ' && s[b] != '\t') ++b;
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 权限字段
// ---------------------------------------------------------------------------
uint32_t GetLevel(Ctx& ctx) {
    uintptr_t obj = 0;
    try {
        obj = ctx.mem.ReadUInt32(ctx.rpg + ctx.ver->player_ptr);
    } catch (...) {
        return 0;
    }
    if (!obj) return 0;
    try {
        return ctx.mem.ReadUInt32(obj + kOffPlayerLev);
    } catch (...) {
        return 0;
    }
}

bool SetLevel(Ctx& ctx, uint32_t lv) {
    uintptr_t obj = 0;
    try {
        obj = ctx.mem.ReadUInt32(ctx.rpg + ctx.ver->player_ptr);
    } catch (...) {
        return false;
    }
    if (!obj) return false;
    ctx.mem.WriteUInt32(obj + kOffPlayerLev, lv);
    return true;
}

uint32_t ReadRights(Ctx& ctx) {
    try {
        return ctx.mem.ReadUInt32(ctx.rpg + ctx.ver->user0 + ctx.ver->rights);
    } catch (...) {
        return 0;
    }
}

// ---------------------------------------------------------------------------
// 远程线程直调 dispatch(token, rest)(对齐 dispatch_direct / dispatch_direct_130)
// ---------------------------------------------------------------------------
struct DispatchResult {
    bool timed_out = false;     // 超时且远程线程被强杀(目标进程状态不可信)
    bool process_gone = false;  // 调用期间目标进程退出
    bool failed = false;        // 线程正常结束但没拿到结果
    int64_t value = 0;
};

DispatchResult DispatchDirect(Ctx& ctx, const std::string& token, const std::string& rest,
                              int wait_ms, bool signextend) {
    ProcessMemory& mem = ctx.mem;
    // 玩家对象未就绪 -> 硬错误(对齐 python sys.exit)
    uintptr_t player = 0;
    try {
        player = mem.ReadUInt32(ctx.rpg + ctx.ver->player_ptr);
    } catch (...) {
    }
    if (!player) throw std::runtime_error("玩家对象未就绪 (读档进入游戏后再试)");
    // cur_ptr 会陈旧(读档后玩家对象重建)或为 0,必须每次用最新玩家对象刷新,
    // 否则门卫取到失效指针 -> handler 被拒。
    mem.WriteUInt32(ctx.rpg + ctx.ver->cur_ptr, static_cast<uint32_t>(player));

    // 分配远程内存(RWX),布局:base+0x0 token GBK / +0x40 rest GBK /
    // +0x200 stub / +0x400 ret_slot。优先 near 分配(便于 32 位 rel32 落 ±2GB)。
    uintptr_t base = 0;
    try {
        base = mem.Alloc(0x1000, ctx.rpg);
    } catch (...) {
        try {
            base = mem.Alloc(0x1000);
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
        std::string t = Utf8ToGbk(token);
        t.push_back('\0');
        std::string r = Utf8ToGbk(rest);
        r.push_back('\0');
        if (t.size() > kRestOff - kTokenOff) {
            throw std::runtime_error("GM 命令 token 过长: GBK " + std::to_string(t.size()) +
                                     " 字节, 上限 " + std::to_string(kRestOff - kTokenOff - 1));
        }
        if (r.size() > kStubOff - kRestOff) {
            throw std::runtime_error("GM 命令参数过长: GBK " + std::to_string(r.size()) +
                                     " 字节, 上限 " + std::to_string(kStubOff - kRestOff - 1));
        }
        mem.Write(base + kTokenOff, ToBytes(t));
        mem.Write(base + kRestOff, ToBytes(r));
        mem.WriteUInt32(base + 0x400, 0);
        uintptr_t stub = base + 0x200;
        uintptr_t ret_slot = base + 0x400;
        // push rest(5) -> push token(5) -> call dispatch(5, rel) ->
        // add esp,8(3) -> mov [ret_slot],eax(5) -> ret(1)  (共 24 字节)
        std::vector<unsigned char> code;
        code.push_back(0x68);
        AppendLe32(code, static_cast<uint32_t>(base + 0x40));
        code.push_back(0x68);
        AppendLe32(code, static_cast<uint32_t>(base));
        int32_t rel =
            static_cast<int32_t>((ctx.rpg + ctx.ver->dispatch) - (stub + code.size() + 5));
        code.push_back(0xE8);
        AppendLe32(code, static_cast<uint32_t>(rel));
        code.push_back(0x83);
        code.push_back(0xC4);
        code.push_back(0x08);
        code.push_back(0xA3);
        AppendLe32(code, static_cast<uint32_t>(ret_slot));
        code.push_back(0xC3);
        mem.Write(stub, code);

        auto rr = mem.CallRemoteThreadDetailed(stub, wait_ms, ret_slot);
        DispatchResult res;
        if (!IsProcessAlive(ctx.pid)) {
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
            res.value = signextend ? static_cast<int64_t>(static_cast<int32_t>(u))
                                   : static_cast<int64_t>(u);
        }
        // 正常/失败但线程已结束: guard 析构释放远程内存(对齐 python 每次 VirtualFreeEx)。
        // 超时强杀时**不释放** —— 线程可能仍在执行 stub 里的代码, 释放会让它跳进已释放内存。
        if (res.timed_out) base_guard.Dismiss();
        return res;
    } catch (...) {
        throw;  // base_guard 析构统一释放
    }
}

DispatchResult SendCommand(Ctx& ctx, const std::string& token, const std::string& rest) {
    if (ctx.ver->is_steam) return DispatchDirect(ctx, token, rest, 3000, false);
    return DispatchDirect(ctx, token, rest, 5000, true);
}

// ---------------------------------------------------------------------------
// wizard.lst 维护(对齐 read_first_alias / fix_wizard_lst / get_module_path)
// ---------------------------------------------------------------------------
std::string ReadFirstAlias(Ctx& ctx) {
    // alias_style == "edi":槽1 EDI=[rpg+char1_edi],结构体=EDI+0x28,别名指针=+0x04(GBK)
    uintptr_t edi = 0;
    try {
        edi = ctx.mem.ReadUInt32(ctx.rpg + ctx.ver->char1_edi);
    } catch (...) {
    }
    if (edi && edi > 0x10000 && edi < 0xFFFFFF00) {
        uintptr_t p = 0;
        try {
            p = ctx.mem.ReadUInt32(edi + 0x28 + 0x04);
        } catch (...) {
        }
        if (p && p > 0x10000 && p < 0xFFFFFF00) {
            std::string s = ReadGbkPtr(ctx.mem, p, 48);
            if (!s.empty()) return s;
        }
    }
    // 通用回退:主玩家对象 + name_off(显示名,聊胜于无)
    uintptr_t obj = 0;
    try {
        obj = ctx.mem.ReadUInt32(ctx.rpg + ctx.ver->player_ptr);
    } catch (...) {
    }
    if (obj && obj > 0x10000 && obj < 0xFFFFFF00) {
        uintptr_t p = 0;
        try {
            p = ctx.mem.ReadUInt32(obj + ctx.ver->name_off);
        } catch (...) {
        }
        if (p && p > 0x10000 && p < 0xFFFFFF00) {
            std::string s = ReadGbkPtr(ctx.mem, p, 48);
            if (!s.empty()) return s;
        }
    }
    return "";
}

// 静默确保 <游戏目录>\System\wizard\wizard.lst 当前用户权限为 9。
// 返回:1=已写入/已改9, 0=已含9未动, -1=失败/跳过。
int FixWizardLst(Ctx& ctx, bool verbose, std::string& log) {
    auto say = [&](const std::string& msg) {
        if (verbose) log += "[wizard] " + msg + "\n";
    };
    try {
        std::string alias = ReadFirstAlias(ctx);
        if (alias.empty()) {
            say("未能读到第一个角色别名 (游戏可能未读档)");
            return -1;
        }
        auto exe_path = ProcessMemory::FindModulePath(ctx.pid, "POQ.exe");
        if (!exe_path) {
            say("未能获取 POQ.exe 模块路径");
            return -1;
        }
        std::wstring exe_dir = Utf8ToWide(*exe_path);
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
        std::string text = GbkToUtf8Replace(raw);
        std::vector<std::string> lines = SplitLines(text);
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        // 已含 "9 别名" 行 -> 无需改动(严格匹配权限号为 9,低权限行不算)
        for (const auto& line : lines) {
            std::vector<std::string> parts = SplitWs(line);
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
            std::vector<std::string> parts = SplitWs(line);
            if (parts.size() >= 2)
                for (size_t k = 1; k < parts.size(); ++k)
                    if (parts[k] == alias) {
                        std::string rest;
                        for (size_t x = 1; x < parts.size(); ++x)
                            rest += (x > 1 ? " " : "") + parts[x];
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
        std::string gbk = Utf8ToGbk(data);
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
void EnsureGm(Ctx& ctx) {
    if (!IsGmReady(ctx)) {
        // activate_gm_by_mem_write(方案A)
        SetLevel(ctx, 9);
        ctx.mem.WriteUInt32(ctx.rpg + ctx.ver->user0 + ctx.ver->rights, 9);
        if (ctx.ver->wizard)
            ctx.mem.WriteUInt32(ctx.exe + ctx.ver->wizard,
                                static_cast<uint32_t>(ctx.rpg + ctx.ver->user0));
        if (ctx.ver->console) ctx.mem.WriteUInt32(ctx.exe + ctx.ver->console, 1);
    }
}

void SilentPrepare(Ctx& ctx) {
    try {
        EnsureGm(ctx);
    } catch (...) {
    }
    try {
        std::string log;
        FixWizardLst(ctx, false, log);
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// 屏幕输出捕获(steam 连续 GBK 串)
// ---------------------------------------------------------------------------
struct StrHit {
    uintptr_t addr = 0;
    std::string text;
};

// 全内存扫描所有包含 kw(GBK 字节)的 NUL 结尾 GBK 字符串(对齐 scan_gbk_strings)。
std::vector<StrHit> ScanGbkStrings(ProcessMemory& mem, const std::string& kw) {
    std::vector<StrHit> out;
    for (const auto& r : mem.EnumerateRegions()) {
        std::vector<unsigned char> data;
        try {
            data = mem.Read(r.base, static_cast<int>(std::min<size_t>(r.size, 0x4000000)));
        } catch (...) {
            continue;
        }
        size_t off = 0;
        while (true) {
            size_t i = FindBytes(data, kw, off);
            if (i == std::string::npos) break;
            // 对齐 python:rfind 窗口 [max(0,i-0x2000), i),未找到 -> s=0(从 region 头起)
            size_t win = (i > 0x2000) ? (i - 0x2000) : 0;
            size_t nul = RFindNul(data, win, i);
            size_t s = (nul == win - 1) ? 0 : nul + 1;
            size_t j = FindNulFrom(data, i);
            std::string text = GbkToUtf8(reinterpret_cast<const char*>(data.data()) + s, j - s);
            if (!text.empty()) out.push_back({r.base + s, text});
            off = i + 1;
        }
    }
    return out;
}

// look 统计的"完整"判定(对齐 _look_stats[0]):对象头含 (N) 且其后对象行数 >= N。
bool LookStatsComplete(const std::string& text) {
    std::smatch m;
    if (!std::regex_search(text, m, ReLookCount())) return false;
    int want = std::stoi(m[1].str());
    std::string tail = text.substr(m.position(0) + m.length(0));
    int got = 0;
    for (const auto& line : SplitLines(tail)) {
        std::string s = Trim(line);
        if (!s.empty() && std::regex_match(s, ReLookObj())) ++got;
    }
    return got >= want && want > 0;
}

// ---------------------------------------------------------------------------
// 1.30 屏幕输出重组(散行 -> 完整块,对齐 capture_130_text)
// ---------------------------------------------------------------------------
struct Class130 {
    std::string kind;  // scene / exhead / objhead / exit / obj / sep / junk
    std::string text;
    int count = 0;
};

Class130 Classify130(const std::string& line) {
    std::string s = Trim(line);
    std::smatch m;
    if (std::regex_match(s, m, ReScene())) return {"scene", s, 0};
    if (std::regex_search(s, m, ReObjHead()) && s.find("%d") == std::string::npos)
        return {"objhead", s, std::stoi(m[1].str())};
    if (s.find("这里的出口有") != std::string::npos) return {"exhead", s, 0};
    if (s.find("这里的人和物品有") != std::string::npos) return {"junk", s, 0};
    if (std::regex_match(s, m, ReExit())) return {"exit", s, 0};
    if (std::regex_match(s, m, ReObj()) && s.find("out :") == std::string::npos &&
        s.find("场景") == std::string::npos && s.find("%s") == std::string::npos &&
        s.find("%d") == std::string::npos && s.find("/ROOM") == std::string::npos)
        return {"obj", s, 0};
    if (s.find("--------") != std::string::npos) return {"sep", s, 0};
    return {"junk", s, 0};
}

// 归一化出口行:去掉尾部 " <N>" 元数据,避免同出口重复。
std::string NormalizeExit(const std::string& val) {
    std::string t = Trim(val);
    std::smatch m;
    if (std::regex_match(t, m, ReExit()))
        return m[1].str() + " : " + m[2].str() + "<" + m[3].str() + ">";
    return val;
}

// 所有含 kb 的 region 内全部 NUL 结尾短行(addr, text),按地址升序(对齐 _gather_130_region_lines)。
std::vector<std::pair<uintptr_t, std::string>> Gather130Lines(ProcessMemory& mem,
                                                              const std::string& kb) {
    std::vector<std::pair<uintptr_t, std::string>> out;
    for (const auto& r : mem.EnumerateRegions()) {
        std::vector<unsigned char> data;
        try {
            data = mem.Read(r.base, static_cast<int>(std::min<size_t>(r.size, 0x4000000)));
        } catch (...) {
            continue;
        }
        if (FindBytes(data, kb, 0) == std::string::npos) continue;
        size_t i = 0;
        while (i < data.size()) {
            size_t j = FindNulFrom(data, i);
            size_t seg_len = j - i;
            if (seg_len >= 3) {
                std::string t = GbkToUtf8(reinterpret_cast<const char*>(data.data()) + i, seg_len);
                if (t.size() >= 2 && t.size() <= 150) out.push_back({r.base + i, t});
            }
            i = j + 1;
        }
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

std::optional<std::string> Capture130Text(Ctx& ctx, bool want_objs, double timeout_sec) {
    std::string mark = want_objs ? "这里的人和物品有" : "这里的出口有";
    std::string kb = Utf8ToGbk(mark);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(static_cast<int>(timeout_sec * 1000.0));
    while (std::chrono::steady_clock::now() < deadline) {
        auto lines = Gather130Lines(ctx.mem, kb);
        const char* head_kind = want_objs ? "objhead" : "exhead";
        std::vector<std::pair<uintptr_t, std::string>> heads;
        for (const auto& [a, t] : lines)
            if (Classify130(t).kind == head_kind) heads.push_back({a, t});
        if (heads.empty()) {
            ::Sleep(300);
            continue;
        }
        // 地址最大的 head = 最近一次写入的块
        uintptr_t head_addr = heads.back().first;
        std::string scene;
        bool scene_set = false;
        std::vector<std::string> exits, objs;
        std::set<std::string> seen_e, seen_o;
        int objcount = 0;
        bool have_count = false;
        for (const auto& [a, t] : lines) {
            if (std::llabs(static_cast<long long>(a) - static_cast<long long>(head_addr)) >= 0x8000)
                continue;
            Class130 c = Classify130(t);
            if (c.kind == "scene") {
                if (!scene_set) {
                    scene = c.text;
                    scene_set = true;
                }
            } else if (c.kind == "objhead") {
                if (!have_count) {
                    objcount = c.count;
                    have_count = true;
                }
            } else if (c.kind == "exit") {
                std::string core = NormalizeExit(c.text);
                if (seen_e.insert(core).second) exits.push_back(core);
            } else if (c.kind == "obj") {
                if (seen_o.insert(c.text).second) objs.push_back(c.text);
            }
        }
        if (want_objs) {
            if (scene_set && have_count && static_cast<int>(objs.size()) >= objcount) {
                std::string out = scene + "\n--------\n这里的出口有: ";
                for (const auto& e : exits) out += "\n" + e;
                out += "\n这里的人和物品有: (" + std::to_string(objcount) + ")";
                for (int i = 0; i < objcount && i < static_cast<int>(objs.size()); ++i)
                    out += "\n" + objs[static_cast<size_t>(i)];
                return out;
            }
        } else {
            if (scene_set && !exits.empty()) {
                std::string out = scene + "\n--------\n这里的出口有: ";
                for (const auto& e : exits) out += "\n" + e;
                return out;
            }
        }
        ::Sleep(300);
    }
    return std::nullopt;
}

// objlist:直调后全内存扫条目行,按 ID 排序输出(对齐 gm_tab 的 build_objdb 用法)。
std::string CaptureObjList(Ctx& ctx) {
    SilentPrepare(ctx);
    DispatchResult res = SendCommand(ctx, "objlist", "");
    if (res.value != 1) return "";
    ::Sleep(800);
    struct ObjEntry {
        std::string name, alias, path;
        int64_t id = 0;
    };
    std::map<int64_t, ObjEntry> entries;
    for (const auto& hit : ScanGbkStrings(ctx.mem, kObjIdPrefix)) {
        auto begin = std::sregex_iterator(hit.text.begin(), hit.text.end(), ReObjEntry());
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            const std::smatch& m = *it;
            std::string name = Trim(m[1].str());
            std::string alias = Trim(m[2].str());
            std::string path = Trim(m[3].str());
            int64_t oid = std::stoll(m[4].str());
            if (name.empty() || alias.empty()) continue;
            entries[oid] = {name, alias, path, oid};
        }
    }
    if (entries.empty()) return "";
    std::string out = "物体个数 : " + std::to_string(entries.size());
    for (const auto& kv : entries)
        out += "\n\t" + kv.second.name + "(" + kv.second.alias + ") : " + kv.second.path + "(" +
               std::to_string(kv.second.id) + ")";
    return out;
}

// 通用捕获路径(对齐 capture_cmd_text,steam 扫连续串)。
std::string CaptureSteamText(Ctx& ctx, bool is_look) {
    std::string kw = is_look ? kMarkObjs : kMarkExits;
    std::string kw_gbk = Utf8ToGbk(kw);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    std::string best;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& hit : ScanGbkStrings(ctx.mem, kw_gbk)) {
            if (is_look) {
                if (hit.text.size() > best.size()) best = hit.text;
                if (LookStatsComplete(hit.text)) return hit.text;
            } else {
                return hit.text;  // dir 无校验:取第一个命中
            }
        }
        ::Sleep(300);
    }
    return best;  // look 兜底:最长命中(可能为空)
}

}  // namespace

// ============================================================================
// 公开接口
// ============================================================================
const GmVersion& SteamVersion() {
    static const GmVersion v{"EasyRPG.dll", 0x478D38, 0x478D34, 0x4797FC, 0x3B5334, 0xEC,
                             0x5409F8,      0x5491B8, 0x6AF60,  0x566B0,  0x28,     true};
    return v;
}

const GmVersion& V130Version() {
    static const GmVersion v{"EasyRPG116.dll", 0x3B45D0, 0x3B45CC, 0x37459C, 0x2F773C, 0x1C4, 0, 0,
                             0x6530,           0x3CDC0,  0x28,     false};
    return v;
}

bool IsGmReady(Ctx& ctx) {
    uint32_t lv = GetLevel(ctx);
    uint32_t rights = ReadRights(ctx);
    return lv >= 5 && rights >= 8;
}

std::string Activate(Ctx& ctx, Method method) {
    std::string log;
    if (method == Method::R9) {
        // 方案B:R9 注入 = Dispatch('r','9');顺带维护 wizard.lst(对齐 python
        // _do_activate:先 fix_wizard_lst(verbose),再 activate_gm_by_cmd)。
        FixWizardLst(ctx, true, log);
        DispatchResult res = DispatchDirect(ctx, "r", "9", 3000, false);
        if (res.process_gone) throw std::runtime_error("游戏进程已退出 (命令可能崩了游戏)");
        if (res.timed_out) log += "[!!] 远程线程超时未返回\n";
    } else {
        // 方案A:内存直写权限字段 + 静默建立 wizard.lst(对齐 cmd_enable)
        bool ok1 = SetLevel(ctx, 9);
        ctx.mem.WriteUInt32(ctx.rpg + ctx.ver->user0 + ctx.ver->rights, 9);
        if (ctx.ver->wizard)
            ctx.mem.WriteUInt32(ctx.exe + ctx.ver->wizard,
                                static_cast<uint32_t>(ctx.rpg + ctx.ver->user0));
        if (ctx.ver->console) ctx.mem.WriteUInt32(ctx.exe + ctx.ver->console, 1);
        log += "[enable] 等级=" + std::to_string(GetLevel(ctx)) +
               " rights=" + std::to_string(ReadRights(ctx)) + " " +
               (ok1 ? "OK" : "(玩家对象未就绪)") + "\n";
        if (!ok1) log += "  [!!] 玩家对象指针未定位 (游戏可能未进入主界面)\n";
        FixWizardLst(ctx, true, log);
    }
    return log;
}

std::string Disable(Ctx& ctx) {
    SetLevel(ctx, 1);
    ctx.mem.WriteUInt32(ctx.rpg + ctx.ver->user0 + ctx.ver->rights, 1);
    if (ctx.ver->wizard) ctx.mem.WriteUInt32(ctx.exe + ctx.ver->wizard, 0);
    if (ctx.ver->console) ctx.mem.WriteUInt32(ctx.exe + ctx.ver->console, 0);
    return "[disable] 已还原: 等级=1 rights=1";
}

std::string Send(Ctx& ctx, const std::string& text) {
    if (text.empty()) throw std::runtime_error("命令为空");
    SilentPrepare(ctx);
    std::string token = text;
    std::string rest;
    size_t sp = text.find(' ');
    if (sp != std::string::npos) {
        token = text.substr(0, sp);
        rest = text.substr(sp + 1);
    }
    std::string log = "[send] 直调 '" + token + "' '" + rest + "'... (版本 " +
                      (ctx.ver->is_steam ? "steam" : "130") + ")\n";
    DispatchResult res = SendCommand(ctx, token, rest);
    if (res.process_gone) {
        if (!IsProcessAlive(ctx.pid)) throw std::runtime_error("游戏进程已退出 (命令可能崩了游戏)");
        log += "=> 返回 -1 (未找到命令或被拒. 1.30: clear/r 等控制台命令不在 GM 表)\n";
        return log;
    }
    if (res.timed_out) {
        log += "[!!] 远程线程超时未返回 - 命令可能挂住, 游戏可能不稳定\n";
        return log;
    }
    log +=
        "=> 返回 " + std::to_string(res.value) + " (" + (res.value ? "成功" : "失败/被拒") + ")\n";
    return log;
}

std::string Capture(Ctx& ctx, const char* kind) {
    std::string k = kind ? kind : "";
    if (k == "objlist") return CaptureObjList(ctx);
    bool is_look = (k == "look");
    SilentPrepare(ctx);
    DispatchResult res = SendCommand(ctx, k, "");
    if (res.value != 1) return "";  // 命令被拒 -> 无输出(对齐 python 返回 None)
    if (!ctx.ver->is_steam) {
        auto t = Capture130Text(ctx, is_look, 4.0);
        return t ? *t : "";
    }
    return CaptureSteamText(ctx, is_look);
}

std::string FixWizard(Ctx& ctx) {
    std::string log;
    int r = FixWizardLst(ctx, true, log);
    if (r == 1) return "已写入/已把权限改为 9 (重启游戏后生效)\n" + log;
    if (r == 0) return "已含 '9 <昵称>', 无需改动\n" + log;
    return "未修改 (见上方原因)\n" + log;
}

}  // namespace gm_cmd
}  // namespace poq
