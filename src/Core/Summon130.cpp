// 召唤兽修改 —— v1.30 版(EasyRPG116.dll)独立逻辑。
//
// 1:1 移植 data/py/summon_tool_130.py 的 cmd_summon 路径(任务 task7 只用到
// summon 命令)。与 Steam 版(SummonSteam.cpp)完全隔离,不共享任何召唤逻辑。
//
// 机制(对齐 python, 偏移全相对模块基址):
// - 召唤结果 = CC 槽决定(槽表固定 1-10): CC1狼 CC2虎 CC3熊 CC5白狼 CC6木偶
//   CC7狗 CC8虎蛟 CC9狍鸮 CC10善翼;朱厌无独立槽 -> 复用槽10+写槽10名。
// - 槽10 = MB+0x213190(只读 .rdata, 写前翻页); 8 种兽槽名游戏固定不写。
// - 秦兵 1-6 大类走 field[0] 单命中必中(长名<=10B 直接放槽10, >10B 重定向
//   field[0] 到 scratch 唯一标签 Q###)。
// - 秦兵类型表指针临时改写需备份:写 %TEMP%\poq_summon_qin_backup_130.json,
//   下次激活/刷新前还原(发布目录不写任何文件)。

#include "Core/Summon130.h"

#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "Core/Json.h"

namespace poq {
namespace summon_130 {

namespace {

// ---------------------------------------------------------------------------
// 常量(对齐 summon_tool_130.py)
// ---------------------------------------------------------------------------
constexpr uintptr_t kOffFriend = 0x20CD6;   // 强制 Friend: push 1; imm 在 +1
constexpr uintptr_t kOffControl = 0x20D69;  // 可控性: cmp eax,imm
constexpr uintptr_t kOffSkip = 0x20D6C;     // 跳过注册: jg 7f 19 -> jmp eb 19
constexpr uintptr_t kOffBound = 0x20B39;    // 召唤类型上限: cmp eax,imm
constexpr uintptr_t kOffType = 0x20687;     // 强制类型: call -> mov eax,<slot>
constexpr uintptr_t kSlotOff = 0x213190;    // 名字槽10 (只读 .rdata)
constexpr uintptr_t kTabOff = 0x3B45F0;     // 类型表计数
constexpr uintptr_t kTabRecOff = 0x3B45F8;  // 记录起点 = TAB+8
constexpr uint32_t kStride = 0x140;
constexpr uint32_t kMaxRec = 600;

// mon_type 用户映射 -> (CC召唤槽, 类型表长名, 需写槽10名?)
// (用户ID≠程序真实ID, 跳过'鸟'; 朱厌复用槽10+写槽10名='召唤兽朱厌')
struct MonMapEntry {
    int cc;
    const char* name;  // 槽10 名字(need_slot 时写)
    bool need_slot;
};
const MonMapEntry kMonMap[10] = {
    {1, "召唤兽狼", false},   {2, "召唤兽老虎", false},
    {3, "召唤兽熊", false},   {5, "召唤兽白狼", false},
    {6, "召唤兽木偶", false}, {7, "召唤兽狗", false},
    {8, "召唤兽虎蛟", false}, {9, "召唤兽狍鸮", false},
    {10, "召唤兽朱厌", true},  // 朱厌: 复用槽10 + 写槽10名
    {10, "召唤兽善翼", true},  // 善翼: CC10 + 写槽10名(原值)
};

// CC 槽 -> 显示名 (patch_type 提示用)  索引=CC, 槽0空
const char* kBeastSlots[12] = {"",     "狼", "老虎", "熊",   "鸟",   "白狼",
                               "木偶", "狗", "虎蛟", "狍鸮", "善翼", "朱厌"};

// 六大类 (soldier_type 1-6):(大类名, 短名, [(子类名, 长名包含子串)...]).
// 1.30 队长有 '旧重剑'(steam 无); UI 层不显示旧重剑(对齐 task7 #4 备注)。
struct QinSub {
    const char* name;
    const char* long_name;
};
struct QinGroup {
    const char* name;
    const char* short_name;
    std::vector<QinSub> subs;
};
const std::vector<QinGroup>& QinGroups() {
    static const std::vector<QinGroup> g = {
        {"普通兵",
         "秦兵",
         {{"刀盾", "秦军士兵刀盾"}, {"长戈", "秦军士兵长戈"}, {"弓箭", "秦军士兵弓箭"}}},
        {"队长",
         "秦兵队长",
         {{"长斧", "秦军头目长斧"}, {"旧重剑", "秦军头目旧重剑"}, {"重剑", "秦军头目重剑"}}},
        {"大将",
         "秦兵大将",
         {{"A", "秦兵大将A"},
          {"B", "秦兵大将B"},
          {"C", "秦兵大将C"},
          {"D", "秦兵大将D"},
          {"E", "秦兵大将E"}}},
        {"官员", "秦军官员", {{"官员", "秦军官员"}}},
        {"法师",
         "秦军法师",
         {{"A", "秦军法师A"},
          {"B", "秦军法师B"},
          {"C", "秦军法师C"},
          {"D", "秦军法师D"},
          {"E", "秦军法师E"}}},
        {"大法师",
         "秦军大法师",
         {{"A", "秦军大法师A"},
          {"B", "秦军大法师B"},
          {"C", "秦军大法师C"},
          {"D", "秦军大法师D"},
          {"E", "秦军大法师E"}}},
    };
    return g;
}

// ---------------------------------------------------------------------------
// 小工具(模块自包含)
// ---------------------------------------------------------------------------
std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string Hex8(uintptr_t a) {
    char b[32];
    std::snprintf(b, sizeof(b), "%08llX", static_cast<unsigned long long>(a));
    return b;
}

std::string Hex8Bytes(const std::vector<unsigned char>& b) {
    std::string out;
    for (unsigned char c : b) {
        char t[4];
        std::snprintf(t, sizeof(t), "%02x ", c);
        out += t;
    }
    if (!out.empty()) out.pop_back();
    return out;
}

// 读 GBK 字符串(UTF-8 返回)。与 python rstr 等价:失败返回 "?",全 0 返回 ""。
std::string RStr(ProcessMemory& mem, uintptr_t a, int n) {
    try {
        std::vector<unsigned char> b = mem.Read(a, n);
        size_t z = 0;
        while (z < b.size() && b[z] != 0) ++z;
        if (z == 0) return "";
        int wn = ::MultiByteToWideChar(936, 0, reinterpret_cast<const char*>(b.data()),
                                       static_cast<int>(z), nullptr, 0);
        if (wn <= 0) return "?";
        std::wstring w(static_cast<size_t>(wn), L'\0');
        ::MultiByteToWideChar(936, 0, reinterpret_cast<const char*>(b.data()), static_cast<int>(z),
                              &w[0], wn);
        int un = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
        if (un <= 0) return "?";
        std::string out(static_cast<size_t>(un), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &out[0], un, nullptr, nullptr);
        return out;
    } catch (...) {
        return "?";
    }
}

// UTF-8 -> CP936(GBK) 字节,失败返回空串。
std::string Utf8ToGbk(const std::string& s) {
    if (s.empty()) return "";
    int wn = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wn <= 0) return "";
    std::wstring w(static_cast<size_t>(wn), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], wn);
    int bn = ::WideCharToMultiByte(936, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    if (bn <= 0) return "";
    std::string out(static_cast<size_t>(bn), '\0');
    ::WideCharToMultiByte(936, 0, w.data(), wn, &out[0], bn, nullptr, nullptr);
    return out;
}

std::vector<unsigned char> GbkBytes(const std::string& s) {
    std::string g = Utf8ToGbk(s);
    return std::vector<unsigned char>(g.begin(), g.end());
}

// %TEMP% 下的备份文件路径(发布目录不写文件)。
std::wstring BackupPath() {
    wchar_t tmp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"poq_summon_qin_backup_130.json";
}

bool WriteTextFileUtf8(const std::wstring& path, const std::string& text) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

std::string ReadTextFileUtf8(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// ---------------------------------------------------------------------------
// 补丁(写后回读校验,失败仅记日志,与 python patch_bytes 一致)
// ---------------------------------------------------------------------------
bool PatchBytes(ProcessMemory& mem, uintptr_t addr, const std::vector<unsigned char>& data,
                const std::string& label, uintptr_t mb, std::string& log) {
    std::string off = mb ? " (MB+0x" + Hex8(addr - mb) + ")" : "";
    try {
        std::vector<unsigned char> old = mem.Read(addr, static_cast<int>(data.size()));
        mem.Write(addr, data);
        std::vector<unsigned char> nw = mem.Read(addr, static_cast<int>(data.size()));
        if (nw == data) {
            log += "  [OK] " + label + " @0x" + Hex8(addr) + off;
            log += ": " + Hex8Bytes(old) + " -> " + Hex8Bytes(data) + "\n";
            return true;
        }
        log += "  [!!] " + label + " 写入后校验失败: " + Hex8Bytes(nw) + " != " + Hex8Bytes(data) +
               "\n";
    } catch (const std::exception& e) {
        log += "  [!!] " + label + " 写入异常: " + e.what() + "\n";
    }
    return false;
}

void PatchType(ProcessMemory& mem, uintptr_t mb, int slot, std::string& log) {
    // 强制召唤类型: call -> mov eax,<slot> (CC setter 直接写 CC=slot, 不 dec)
    std::vector<unsigned char> data;
    uint32_t v = static_cast<uint32_t>(slot & 0xFFFFFFFF);
    data.push_back(0xb8);
    data.push_back(static_cast<unsigned char>(v & 0xFF));
    data.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    data.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
    data.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
    const char* nm = (slot >= 0 && slot < 12) ? kBeastSlots[slot] : "?";
    PatchBytes(mem, mb + kOffType, data,
               "强制召唤类型 slot=" + std::to_string(slot) + "(" + nm + ")", mb, log);
}

void PatchBound(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
    std::vector<unsigned char> data = {0x83, 0xf8, static_cast<unsigned char>(val & 0xFF)};
    PatchBytes(mem, mb + kOffBound, data, "type bound=0x" + Hex8(val), mb, log);
}

void PatchControl(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
    std::vector<unsigned char> data = {0x83, 0xf8, static_cast<unsigned char>(val & 0xFF)};
    PatchBytes(mem, mb + kOffControl, data, "可控性 cmp 0x" + Hex8(val), mb, log);
}

void PatchFriend(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
    // push 1 的 imm 在 opcode 0x6a 后 +1
    std::vector<unsigned char> data = {static_cast<unsigned char>(val & 0xFF)};
    PatchBytes(mem, mb + kOffFriend + 1, data, "召唤强制 Friend imm", mb, log);
}

void PatchSkip(ProcessMemory& mem, uintptr_t mb, bool on, std::string& log) {
    if (on) {
        PatchBytes(mem, mb + kOffSkip, {0xeb, 0x19}, "跳过 AddMember 注册(不可控)", mb, log);
    } else {
        PatchBytes(mem, mb + kOffSkip, {0x7f, 0x19}, "恢复 AddMember 注册", mb, log);
    }
}

// 写槽10(只读 .rdata, ProcessMemory::Write 自动翻页)。写后回读校验。
// 结尾 \0 必须 append——`+ "\x00"` 会按 C 字符串 strlen 截断丢失终止符(见 SummonSteam).
bool WriteSlot(ProcessMemory& mem, uintptr_t mb, const std::string& name, std::string& log) {
    std::string g = Utf8ToGbk(name);
    g.append(4, '\0');
    uintptr_t addr = mb + kSlotOff;
    try {
        mem.Write(addr, std::vector<unsigned char>(g.begin(), g.end()));
        std::string s = RStr(mem, addr, 12);
        bool ok = (s == name);
        log += "  [" + std::string(ok ? "OK" : "!!") + "] 名字槽 0x" + Hex8(addr) +
               " (槽10) = " + name + (ok ? " ✓" : "  (读回 " + s + ")") + "\n";
        return ok;
    } catch (const std::exception& e) {
        log += std::string("  [!!] 名字槽写入异常: ") + e.what() + "\n";
        return false;
    }
}

// 清空槽10(只读区, 必须翻页后写 0; 残留上次召唤名会召唤出错误野兽)。
bool ClearSlot(ProcessMemory& mem, uintptr_t mb, std::string& log) {
    uintptr_t addr = mb + kSlotOff;
    try {
        mem.Write(addr, std::vector<unsigned char>(12, 0));
        std::string s = RStr(mem, addr, 12);
        bool ok = s.empty();
        log += "  [" + std::string(ok ? "OK" : "!!") + "] 槽10 0x" + Hex8(addr) + " 清空 " +
               (ok ? "✓" : "  (读回 " + s + ")") + "\n";
        return ok;
    } catch (const std::exception& e) {
        log += std::string("  [!!] 槽10 清空异常: ") + e.what() + "\n";
        return false;
    }
}

// 刺客名字槽(1.30 机制未验证): on=写 '召唤兽刺客弓', off=写回 '召唤兽善翼'。
void CmdCike(ProcessMemory& mem, uintptr_t mb, bool on, std::string& log) {
    if (on) {
        WriteSlot(mem, mb, "召唤兽刺客弓", log);
        log += "  用法: type 10 -> 刺客弓 (1.30 刺客机制未验证)\n";
    } else {
        WriteSlot(mem, mb, "召唤兽善翼", log);
        log += "  刺客名字槽已还原 (槽10=召唤兽善翼)\n";
    }
}

// ---------------------------------------------------------------------------
// 秦兵类型表备份(写 %TEMP%\poq_summon_qin_backup_130.json)
// ---------------------------------------------------------------------------
uintptr_t g_mb = 0;  // 模块基址(相对 MB 偏移计算需要; CmdSummon 入口设置)

void SaveQinBackup(ProcessMemory& mem, const std::vector<std::tuple<int, int, uintptr_t>>& mutated,
                   uintptr_t scratch) {
    (void)mem;
    json::Value root = json::Value::MakeObject();
    json::Value recs = json::Value::MakeObject();
    for (const auto& m : mutated) {
        int r, off;
        uintptr_t v;
        std::tie(r, off, v) = m;
        json::Value item = json::Value::MakeObject();
        item.Set("rec", json::Value::MakeNumber(r));
        item.Set("off", json::Value::MakeNumber(off));
        item.Set("v", json::Value::MakeNumber(static_cast<double>(v)));
        // 键含 off: 同 rec 多偏移不互覆(对齐 python f"{r}_{off}")
        recs.Set(std::to_string(r) + "_" + std::to_string(off), std::move(item));
    }
    root.Set("recs", std::move(recs));
    root.Set("scratch", scratch ? json::Value::MakeNumber(static_cast<double>(scratch))
                                : json::Value::MakeNull());
    WriteTextFileUtf8(BackupPath(), json::Write(root, false));
}

bool RestoreQinBackup(ProcessMemory& mem) {
    std::string text = ReadTextFileUtf8(BackupPath());
    if (text.empty()) return false;
    try {
        json::Value root = json::Parse(text);
        const json::Value* recs = root.Find("recs");
        if (!recs || !recs->IsObject()) return false;
        uintptr_t scratch = 0;
        if (const json::Value* s = root.Find("scratch"))
            if (s->IsNumber()) scratch = static_cast<uintptr_t>(s->number);
        int n = 0;
        for (const auto& kv : recs->object) {
            double rec = 0, off = 4, v = 0;
            if (kv.second.IsObject()) {
                if (const json::Value* x = kv.second.Find("rec")) rec = x->number;
                if (const json::Value* x = kv.second.Find("off")) off = x->number;
                if (const json::Value* x = kv.second.Find("v")) v = x->number;
            } else {
                rec = std::atof(kv.first.c_str());  // 旧格式: 裸指针 = field[1]
                off = 4;
                v = kv.second.number;
            }
            try {
                mem.WriteUInt32(g_mb + kTabRecOff + static_cast<uintptr_t>(rec) * kStride +
                                    static_cast<uintptr_t>(off),
                                static_cast<uint32_t>(v));
                ++n;
            } catch (...) {
            }
        }
        if (scratch) {
            try {
                mem.Free(scratch);
            } catch (...) {
            }
        }
        ::DeleteFileW(BackupPath().c_str());
        return n > 0;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// scratch 分配(写唯一短名标签用, 需高地址: 低位会被游戏意外访问而崩溃)
// ---------------------------------------------------------------------------
uintptr_t AllocScratch(ProcessMemory& mem) {
    try {
        return mem.Alloc(0x4000, 0x70000000);
    } catch (...) {
        return mem.Alloc(0x4000);
    }
}

class QinScratch {
public:
    explicit QinScratch(ProcessMemory& mem) : mem_(mem), base_(AllocScratch(mem)) {}
    // ★ 析构不释放 scratch:游戏召唤时才读 field[0] -> 标签,立即释放会导致游戏
    // 访问已释放内存而卡死。scratch 驻留到下次召唤命令的 RestoreQinBackup
    // (读备份文件里的 scratch 地址统一释放,与 python 行为一致)。
    ~QinScratch() {}
    QinScratch(const QinScratch&) = delete;
    QinScratch& operator=(const QinScratch&) = delete;

    uintptr_t Put(const std::string& gbk_name) {
        std::vector<unsigned char> data = GbkBytes(gbk_name);
        data.push_back(0);
        mem_.Write(base_ + off_, data);
        uintptr_t p = base_ + off_;
        off_ += data.size();
        return p;
    }
    uintptr_t Base() const { return base_; }

private:
    ProcessMemory& mem_;
    uintptr_t base_ = 0;
    size_t off_ = 0;
};

// ---------------------------------------------------------------------------
// 秦兵子类匹配(扫类型表: 短名==大类短名 且 长名包含子类串)
// ---------------------------------------------------------------------------
std::optional<std::vector<int>> ResolveQinSub(ProcessMemory& mem, int group_idx, int sub_idx) {
    const auto& groups = QinGroups();
    if (group_idx < 0 || group_idx >= static_cast<int>(groups.size())) return std::nullopt;
    const QinGroup& g = groups[static_cast<size_t>(group_idx)];
    if (sub_idx < 0 || sub_idx >= static_cast<int>(g.subs.size())) return std::nullopt;
    const char* sub_str = g.subs[static_cast<size_t>(sub_idx)].long_name;
    std::vector<int> recs;
    uint32_t cnt = 0;
    try {
        cnt = mem.ReadUInt32(g_mb + kTabOff);
    } catch (...) {
        cnt = 0;
    }
    uint32_t limit = cnt < kMaxRec ? cnt : kMaxRec;
    for (uint32_t i = 0; i < limit; ++i) {
        uintptr_t base = g_mb + kTabRecOff + static_cast<uintptr_t>(i) * kStride;
        uint32_t p0 = 0, p1 = 0;
        try {
            p0 = mem.ReadUInt32(base);
            p1 = mem.ReadUInt32(base + 4);
        } catch (...) {
            continue;
        }
        if (!p0 || !p1 || p0 == 0xFFFFFFFF || p1 == 0xFFFFFFFF) continue;
        if (p0 < 0x200000 || p1 < 0x200000) continue;
        std::string s1 = RStr(mem, p1, 16);
        if (s1 != g.short_name) continue;
        std::string s0 = RStr(mem, p0, 32);
        if (s0.find(sub_str) != std::string::npos) recs.push_back(static_cast<int>(i));
    }
    if (recs.empty()) return std::nullopt;
    return recs;
}

std::string LegalSubs(const QinGroup& g) {
    std::string out;
    for (size_t i = 0; i < g.subs.size(); ++i) {
        if (!out.empty()) out += " / ";
        out += std::to_string(i) + "=" + g.subs[i].name;
    }
    return out;
}

// 1/2/6 大类 (长名>10B): 目标子类第一条 field[0] 重定向 scratch 标签 Q###, 槽10=Q###.
bool SummonQinField0Tag(ProcessMemory& mem, int group_idx, int sub_idx, std::string& log) {
    // 先还原上次 field[0] 残留,否则本次 resolve 扫表会读到上次的标签指针(或已释放
    // scratch)导致"无匹配记录"、召唤仍是上次的怪。(python 仅无匹配时才 restore,这里
    // 提前到开头更稳健:连续召唤同子类也能命中)
    RestoreQinBackup(mem);
    auto recs = ResolveQinSub(mem, group_idx, sub_idx);
    const auto& groups = QinGroups();
    const QinGroup& g = groups[static_cast<size_t>(group_idx)];
    if (!recs) {
        RestoreQinBackup(mem);
        log += "  大类[" + std::to_string(group_idx) + "]=" + g.name +
               " 无匹配记录, 或子类越界 (该版本子类: " + LegalSubs(g) + ")\n";
        return false;
    }
    int rec = (*recs)[0];
    char tagbuf[16];
    std::snprintf(tagbuf, sizeof(tagbuf), "Q%03d", rec);
    std::string tag = tagbuf;
    QinScratch scr(mem);
    uintptr_t tp = scr.Put(tag);
    uintptr_t base = g_mb + kTabRecOff + static_cast<uintptr_t>(rec) * kStride;
    uint32_t orig = mem.ReadUInt32(base);
    std::string s0 = RStr(mem, orig, 32);
    mem.WriteUInt32(base, static_cast<uint32_t>(tp));  // field[0] -> 标签
    WriteSlot(mem, g_mb, tag, log);                    // 槽10 = 标签
    SaveQinBackup(mem, {{rec, 0, orig}}, scr.Base());
    // 自校验
    std::string s10;
    try {
        s10 = RStr(mem, g_mb + kSlotOff, 8);
    } catch (const std::exception& e) {
        log += std::string("  [!!] 自校验读取异常: ") + e.what() + "\n";
    }
    bool ok = (s10 == tag);
    if (!ok) {
        log += "  [!!] 自校验失败 槽10=" + s10 + " (期望 " + tag + ")\n";
        return false;
    }
    log += std::string("  [秦兵·") + g.name + "/" + g.subs[static_cast<size_t>(sub_idx)].name +
           "] rec" + std::to_string(rec) + " (" + s0 + ") -> field[0]=" + tag + ", 槽10=" + tag +
           " ✓ 单命中必中\n";
    return true;
}

// 3/4/5 大类 (长名<=10B): 长名单直接放槽10, 前缀匹配单命中.
// 长名>10B 返回 false(调用方直接 return, 与 python 返回 None 行为一致)。
bool SummonQinLongname(ProcessMemory& mem, int group_idx, int sub_idx, std::string& log) {
    auto recs = ResolveQinSub(mem, group_idx, sub_idx);
    const auto& groups = QinGroups();
    const QinGroup& g = groups[static_cast<size_t>(group_idx)];
    if (!recs) {
        RestoreQinBackup(mem);
        log += "  大类[" + std::to_string(group_idx) + "]=" + g.name +
               " 无匹配记录, 或子类越界 (该版本子类: " + LegalSubs(g) + ")\n";
        return false;
    }
    int rec = (*recs)[0];
    uint32_t p0 = mem.ReadUInt32(g_mb + kTabRecOff + static_cast<uintptr_t>(rec) * kStride);
    std::string long_name = RStr(mem, p0, 32);
    if (Utf8ToGbk(long_name).size() > 10) return false;  // 长名放不进槽10
    RestoreQinBackup(mem);
    WriteSlot(mem, g_mb, long_name, log);
    std::string s10 = RStr(mem, g_mb + kSlotOff, 12);
    bool ok = (s10 == long_name);
    if (!ok) {
        log += "  [!!] 自校验失败 槽10=" + s10 + " (期望 " + long_name + ")\n";
        return false;
    }
    log += std::string("  [秦兵·") + g.name + "/" + g.subs[static_cast<size_t>(sub_idx)].name +
           "] rec" + std::to_string(rec) + " 长名=" + long_name + " -> 槽10 ✓ 单命中必中\n";
    return true;
}

std::map<std::string, std::string> ParseArgs(const std::vector<std::string>& args) {
    std::map<std::string, std::string> d;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
            std::string k = a.substr(2);
            std::string v;
            size_t eq = k.find('=');
            if (eq != std::string::npos) {
                v = k.substr(eq + 1);
                k = k.substr(0, eq);
            } else if (i + 1 < args.size()) {
                v = args[++i];
            }
            d[ToLower(k)] = v;
        }
    }
    return d;
}

int ParseInt(const std::string& s) {
    return static_cast<int>(std::strtol(s.c_str(), nullptr, 0));
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开接口:cmd_summon(对齐 python)
// ---------------------------------------------------------------------------
std::string CmdSummon(ProcessMemory& mem, uintptr_t mb, const std::vector<std::string>& args) {
    std::string log;
    if (mb == 0) {
        throw std::runtime_error("v1.30 模块基址无效(EasyRPG116.dll 未找到?)");
    }
    g_mb = mb;

    std::map<std::string, std::string> d = ParseArgs(args);
    // 冲突检测: 三类互斥, 冲突回退 mon_type=0
    int given = 0;
    for (const char* k : {"mon_type", "assassin_type", "soldier_type"})
        if (d.count(k)) ++given;
    if (given >= 2) {
        log += "  [!!] 冲突: 同时指定多类, 按规则回退到 --mon_type 0 (狼)\n";
        std::string g = d.count("group") ? d["group"] : "1";
        d.clear();
        d["group"] = g;
    }
    int group = d.count("group") ? ParseInt(d["group"]) : 1;  // 1=盟友不可控(默认)
    if (group != 0 && group != 1) {
        log += "  --group 须 0=队友可控 / 1=盟友不可控\n";
        return log;
    }

    int slot = 0;
    std::string label;
    if (d.count("soldier_type")) {
        int st = ParseInt(d["soldier_type"]);
        if (st < 0 || st > 6) {
            log += "  --soldier_type 须 0-6: 0=随机秦兵, 1-6=普通兵/队长/大将/官员/法师/大法师\n";
            return log;
        }
        if (st == 0) {
            if (d.count("soldier_sub_type")) {
                log += "  [!!] 随机秦兵(soldier_type=0) 忽略 --soldier_sub_type (随机无子类)\n";
                return log;
            }
            // 随机秦兵: 清空槽10(名无匹配 -> 类别11秦兵随机) + slot=10(CC=10 善翼位置)
            RestoreQinBackup(mem);
            CmdCike(mem, mb, false, log);  // 槽10 -> '召唤兽善翼' (WriteSlot 带翻页)
            ClearSlot(mem, mb, log);       // 槽10 -> 全 0 -> 槽名无匹配 -> 秦兵随机
            slot = 10;
            label = "随机秦兵 (游戏系统随机)";
        } else {
            int sub_idx = d.count("soldier_sub_type") ? ParseInt(d["soldier_sub_type"]) : 0;
            const QinGroup& g = QinGroups()[static_cast<size_t>(st - 1)];
            if (d.count("soldier_sub_type") &&
                (sub_idx < 0 || sub_idx >= static_cast<int>(g.subs.size()))) {
                log += "  --soldier_sub_type " + std::to_string(sub_idx) + " 非法: 大类[" +
                       std::to_string(st) + "]=" + g.name + " 该版本子类须 " + LegalSubs(g) + "\n";
                return log;
            }
            label = std::string(g.name) + "/" + g.subs[static_cast<size_t>(sub_idx)].name;
            bool ok;
            if (st == 3 || st == 4 || st == 5)
                ok = SummonQinLongname(mem, st - 1, sub_idx, log);
            else
                ok = SummonQinField0Tag(mem, st - 1, sub_idx, log);
            if (!ok) return log;
            slot = 10;
        }
    } else if (d.count("assassin_type")) {
        int at = ParseInt(d["assassin_type"]);
        if (at != 0 && at != 1) {
            log += "  --assassin_type 须 0=弓箭 / 1=带刀\n";
            return log;
        }
        RestoreQinBackup(mem);
        CmdCike(mem, mb, true, log);
        slot = 10;
        label = "刺客 (1.30 机制未验证)";
    } else {
        int mt = d.count("mon_type") ? ParseInt(d["mon_type"]) : 0;
        if (mt < 0 || mt > 9) {
            log += "  --mon_type 须 0-9 (1.30 仅 0-7 可用, 8朱厌/9善翼 复用槽10): ";
            log += "0=狼 1=虎 2=熊 3=白狼 4=木偶 5=狗 6=虎蛟 7=狍鸮\n";
            return log;
        }
        const MonMapEntry& e = kMonMap[mt];
        RestoreQinBackup(mem);
        // 8 种兽槽名游戏固定不写; 朱厌/善翼 复用槽10(CC=10), 必须写槽10名
        if (e.need_slot) WriteSlot(mem, mb, e.name, log);
        slot = e.cc;
        label = std::string(e.name).substr(3);  // 去掉 "召唤兽" 前缀
    }

    // ---- 召唤补丁(游戏召唤时读取)----
    PatchType(mem, mb, slot, log);
    PatchBound(mem, mb, 0x7f, log);
    PatchControl(mem, mb, 0x7f, log);
    PatchFriend(mem, mb, 1, log);
    PatchSkip(mem, mb, group == 1, log);
    log += "\n  [召唤] 目标=" + label + ", group=" + std::to_string(group) + " (" +
           (group == 0 ? "队友·可控" : "盟友·不可控·被动攻击") + ")\n";
    log += "  现在去游戏里按召唤键即可 (补丁对下次召唤生效)\n";
    return log;
}

}  // namespace summon_130
}  // namespace poq
