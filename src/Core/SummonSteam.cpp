// 召唤兽修改 —— Steam 版(EasyRPG.dll)独立逻辑。
//
// 1:1 移植 data/py/summon_tool_steam.py 的 cmd_summon 路径(任务 task7 只用到
// summon 命令)。与 v1.30 版(Summon130.cpp)完全隔离,不共享任何召唤逻辑。
//
// 机制(对齐 python):
// - 补丁点 = 模块基址 + 固定偏移;名字槽/类型表/刺客记录为 CE 实测绝对地址。
// - 召唤按"显示名"查野兽表:野兽/刺客/随机秦兵走匹配器 field[0] 长名;
//   秦兵 1-6 大类走 field[0] 单命中必中(长名<=10B 直接放槽10,>10B 重定向
//   field[0] 到 scratch 唯一标签 Q###)。
// - 秦兵类型表指针临时改写需备份:写 %TEMP%\poq_summon_qin_backup.json,
//   下次激活/刷新前还原(发布目录不写任何文件)。

#include "Core/SummonSteam.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <windows.h>

#include "Core/Json.h"

namespace poq {
namespace summon_steam {

namespace {

// ---------------------------------------------------------------------------
// 常量(对齐 summon_tool_steam.py)
// ---------------------------------------------------------------------------
constexpr uintptr_t kOffFriendPush = 0x35BAA;   // 召唤强制 Friend imm(push 1 的 01)
constexpr uintptr_t kOffBound = 0x3586D;        // 召唤类型上限 cmp eax,imm
constexpr uintptr_t kOffControlCmp = 0x35C32;   // 可控性 cmp [0x53bb88cc],imm / 跳过注册
constexpr uintptr_t kOffType = 0x3585F;         // 强制召唤类型 mov eax,slot+1
constexpr uintptr_t kOffMatcherImm = 0x6FAC6;   // 名字匹配器 push 0/1
constexpr uintptr_t kNameZhuYan = 0x539771B0;   // 名字槽 slot9(朱厌源)
constexpr uintptr_t kNameShanYi = 0x539771BC;   // 名字槽 slot10(善翼源)
constexpr uintptr_t kRecBow = 0x53C1F5C0;       // 刺客弓记录
constexpr uintptr_t kRecKnife = 0x53C1F700;     // 刺客刀记录
constexpr uintptr_t kQinTable = 0x53bfab78;     // 秦兵类型表
constexpr uint32_t kQinStride = 0x140;
constexpr uint32_t kQinNullPtr = 0x53bfa410;    // 表记录的空指针哨兵
constexpr int kMaxRec = 600;

// mon_type 映射ID -> 程序槽位(映射ID 3=白狼 跳过鸟=槽3,即 [0,1,2,4,5,6,7,8,9,10])
const int kMonSlots[10] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10};
const char* kMonNames[10] = {"狼",   "虎",   "熊", "白狼", "木偶",
                             "狗",   "虎蛟", "狍鸮", "朱厌", "善翼"};
// 野兽槽位名(slot 0-10)
const char* kBeastSlots[11] = {"狼",   "虎",   "熊", "鸟", "白狼", "木偶",
                               "狗",   "虎蛟", "狍鸮", "朱厌", "善翼"};

// 六大类 (soldier_type 1-6):(大类名, 短名, [(子类名, 长名包含子串)...]).
// 队长大类的 '旧重剑' 不支持召唤:工具层数据完整保留(UI 层不显示,对齐 task7 #3.3.1)。
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
        {"普通兵", "秦兵",
         {{"刀盾", "秦军士兵刀盾"}, {"长戈", "秦军士兵长戈"}, {"弓箭", "秦军士兵弓箭"}}},
        {"队长", "秦兵队长",
         {{"长斧", "秦军头目长斧"}, {"旧重剑", "秦军头目旧重剑"}, {"重剑", "秦军头目重剑"}}},
        {"大将", "秦兵大将",
         {{"A", "秦兵大将A"}, {"B", "秦兵大将B"}, {"C", "秦兵大将C"}, {"D", "秦兵大将D"},
          {"E", "秦兵大将E"}}},
        {"官员", "秦军官员", {{"官员", "秦军官员"}}},
        {"法师", "秦军法师",
         {{"A", "秦军法师A"}, {"B", "秦军法师B"}, {"C", "秦军法师C"}, {"D", "秦军法师D"},
          {"E", "秦军法师E"}}},
        {"大法师", "秦军大法师",
         {{"A", "秦军大法师A"}, {"B", "秦军大法师B"}, {"C", "秦军大法师C"}, {"D", "秦军大法师D"},
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
        ::MultiByteToWideChar(936, 0, reinterpret_cast<const char*>(b.data()),
                              static_cast<int>(z), &w[0], wn);
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
    return std::wstring(tmp) + L"poq_summon_qin_backup.json";
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
        log += "  [!!] " + label + " 写入后校验失败: " + Hex8Bytes(nw) + " != " +
               Hex8Bytes(data) + "\n";
    } catch (const std::exception& e) {
        log += "  [!!] " + label + " 写入异常: " + e.what() + "\n";
    }
    return false;
}

void PatchType(ProcessMemory& mem, uintptr_t mb, int slot, std::string& log) {
    // mov eax, slot+1 (管理器会自减: 槽 0-10)
    std::vector<unsigned char> data;
    uint32_t v = static_cast<uint32_t>(slot + 1);
    data.push_back(0xb8);
    data.push_back(static_cast<unsigned char>(v & 0xFF));
    data.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    data.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
    data.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
    const char* nm = (slot >= 0 && slot < 11) ? kBeastSlots[slot] : "?";
    PatchBytes(mem, mb + kOffType, data,
               "强制召唤类型 slot=" + std::to_string(slot) + "(" + nm + ")", mb, log);
}

void PatchBound(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
    std::vector<unsigned char> data = {0x83, 0xf8, static_cast<unsigned char>(val & 0xFF)};
    PatchBytes(mem, mb + kOffBound, data, "type bound=0x" + Hex8(val), mb, log);
}

void PatchControl(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
    // cmp dword [0x53bb88cc], imm
    std::vector<unsigned char> data = {0x83, 0x3d, 0xcc, 0x88, 0xbb, 0x53,
                                       static_cast<unsigned char>(val & 0xFF)};
    PatchBytes(mem, mb + kOffControlCmp, data, "可控性 cmp 0x" + Hex8(val), mb, log);
}

void PatchFriend(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
    std::vector<unsigned char> data = {static_cast<unsigned char>(val & 0xFF)};
    PatchBytes(mem, mb + kOffFriendPush, data, "召唤强制 Friend imm", mb, log);
}

int GetControlVal(ProcessMemory& mem, uintptr_t mb) {
    try {
        std::vector<unsigned char> op = mem.Read(mb + kOffControlCmp, 1);
        if (op[0] == 0x83) {
            std::vector<unsigned char> b = mem.Read(mb + kOffControlCmp + 6, 1);
            return b[0];
        }
    } catch (...) {
    }
    return 0x0a;
}

void PatchSkipMember(ProcessMemory& mem, uintptr_t mb, bool on, std::string& log) {
    if (on) {
        std::vector<unsigned char> data = {0xEB, 0x20, 0x90, 0x90, 0x90, 0x90};
        PatchBytes(mem, mb + kOffControlCmp, data, "跳过 AddMember 注册(不可控)", mb, log);
    } else {
        int ctrl = GetControlVal(mem, mb);
        std::vector<unsigned char> data = {0x83, 0x3d, 0xcc, 0x88, 0xbb, 0x53,
                                           static_cast<unsigned char>(ctrl & 0xFF)};
        PatchBytes(mem, mb + kOffControlCmp, data, "恢复 AddMember 注册", mb, log);
    }
}

void PatchMatcher(ProcessMemory& mem, uintptr_t mb, bool on, std::string& log) {
    std::vector<unsigned char> data = {on ? static_cast<unsigned char>(1)
                                          : static_cast<unsigned char>(0)};
    PatchBytes(mem, mb + kOffMatcherImm, data,
               on ? "匹配器 -> field[1](短名)" : "匹配器 -> field[0](长名/原版)", mb, log);
}

// 刺客名字槽: on=克隆刺客长名写进朱厌/善翼槽; off=写回原版野兽长名。
void CmdCike(ProcessMemory& mem, bool on, std::string& log) {
    if (on) {
        std::vector<unsigned char> bow = mem.Read(mem.ReadUInt32(kRecBow), 12);
        std::vector<unsigned char> knife = mem.Read(mem.ReadUInt32(kRecKnife), 12);
        mem.Write(kNameShanYi, bow);    // 善翼槽 -> 克隆刺客弓
        mem.Write(kNameZhuYan, knife);  // 朱厌槽 -> 克隆刺客刀
        log += "  刺客名字槽已打: 0x" + Hex8(kNameShanYi) + "=克隆刺客弓, 0x" +
               Hex8(kNameZhuYan) + "=克隆刺客刀\n";
        log += "  用法: type 10 -> cike_bow(弓); type 9 -> cike_knife(刀)\n";
    } else {
        // 直接写回原版野兽长名(不依赖备份文件): 只写 3 个槽地址, 不碰槽外数据
        struct SlotName {
            uintptr_t addr;
            const char* name;
        };
        const SlotName kRestore[] = {{0x539771A4, "召唤兽狍鸮"},
                                     {kNameZhuYan, "召唤兽朱厌"},
                                     {kNameShanYi, "召唤兽善翼"}};
        for (const auto& s : kRestore) {
            std::string g = Utf8ToGbk(s.name);
            g.push_back('\0');
            g.push_back('\0');  // 见 WriteSlot10:不可用 +"\x00\x00" 拼接
            mem.Write(s.addr, std::vector<unsigned char>(g.begin(), g.end()));
        }
        log += "  刺客名字槽已还原: slot8=狍鸮 slot9=朱厌 slot10=善翼 (原版野兽长名)\n";
    }
}

// ---------------------------------------------------------------------------
// 秦兵类型表备份(写 %TEMP%\poq_summon_qin_backup.json)
// ---------------------------------------------------------------------------
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
        item.Set("off", json::Value::MakeNumber(off));
        item.Set("v", json::Value::MakeNumber(static_cast<double>(v)));
        recs.Set(std::to_string(r), std::move(item));  // 键 = rec(steam 版)
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
            double off = 4, v = 0;
            if (kv.second.IsObject()) {
                if (const json::Value* x = kv.second.Find("off")) off = x->number;
                if (const json::Value* x = kv.second.Find("v")) v = x->number;
            } else if (kv.second.IsNumber()) {
                v = kv.second.number;  // 旧格式: 裸指针 = field[1]
            }
            int rec = std::atoi(kv.first.c_str());
            try {
                mem.WriteUInt32(kQinTable + 8 + static_cast<uintptr_t>(rec) * kQinStride +
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
        // 从 0x70000000 向下扫空闲页(等价 python 依次尝试固定高地址基址)
        return mem.Alloc(0x4000, 0x70000000);
    } catch (...) {
        return mem.Alloc(0x4000);  // ANY 兜底
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
        cnt = mem.ReadUInt32(kQinTable);
    } catch (...) {
        cnt = 0;
    }
    uint32_t limit = cnt < kMaxRec ? cnt : kMaxRec;
    for (uint32_t i = 0; i < limit; ++i) {
        uintptr_t base = kQinTable + 8 + static_cast<uintptr_t>(i) * kQinStride;
        uint32_t p0 = 0, p1 = 0;
        try {
            p0 = mem.ReadUInt32(base);
            p1 = mem.ReadUInt32(base + 4);
        } catch (...) {
            continue;
        }
        if (!p0 || !p1 || p0 == kQinNullPtr || p1 == kQinNullPtr) continue;
        if (!(0x20000000 <= p0 && p0 < 0x90000000) || !(0x20000000 <= p1 && p1 < 0x90000000))
            continue;
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

// 写槽10(善翼源)名字。
// 注意:结尾 \0 必须用 push_back/append 追加——`std::string + "\x00"` 会把 C 字符串
// "\x00" 按 strlen(=0) 拼接,导致终止符丢失,槽10 残留旧名导致自校验失败/召唤错怪。
void WriteSlot10(ProcessMemory& mem, const std::string& name, std::string& log) {
    std::string g = Utf8ToGbk(name);
    g.push_back('\0');
    mem.Write(kNameShanYi, std::vector<unsigned char>(g.begin(), g.end()));
}

// 1/2/6 大类(长名>10B): 目标子类第一条 field[0] 重定向 scratch 标签 Q###, 槽10=Q###.
bool SummonQinField0Tag(ProcessMemory& mem, uintptr_t mb, int group_idx, int sub_idx,
                        std::string& log) {
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
    uintptr_t base = kQinTable + 8 + static_cast<uintptr_t>(rec) * kQinStride;
    uint32_t orig = mem.ReadUInt32(base);
    std::string s0 = RStr(mem, orig, 32);
    mem.WriteUInt32(base, static_cast<uint32_t>(tp));  // field[0] -> 唯一标签
    PatchMatcher(mem, mb, false, log);                 // 匹配器保持 field[0] (0)
    WriteSlot10(mem, tag, log);                        // 槽10 = 标签
    SaveQinBackup(mem, {{rec, 0, orig}}, scr.Base());
    // 自校验 (防 restore 残留导致 0 命中)
    uint8_t imm = 0;
    std::string s10, s0now;
    try {
        imm = mem.Read(mb + kOffMatcherImm, 1)[0];
        s10 = RStr(mem, kNameShanYi, 8);
        s0now = RStr(mem, mem.ReadUInt32(base), 32);
    } catch (const std::exception& e) {
        log += std::string("  [!!] 自校验读取异常: ") + e.what() + "\n";
    }
    if (imm != 0 || s10 != tag || s0now != tag) {
        log += "  [!!] 自校验失败 matcher=" + std::to_string(imm) + " 槽10=" + s10 + " f0=" +
               s0now + " (期望 0/" + tag + ")";
        // 附原始字节,便于定位写入是否落盘
        try {
            auto b10 = mem.Read(kNameShanYi, 8);
            auto bf = mem.Read(mem.ReadUInt32(base), 8);
            log += " 槽10hex=" + Hex8Bytes(b10) + " f0hex=" + Hex8Bytes(bf);
        } catch (...) {
        }
        log += "\n";
        return false;
    }
    log += std::string("  [秦兵·") + g.name + "/" +
           g.subs[static_cast<size_t>(sub_idx)].name + "] rec" + std::to_string(rec) + " (" +
           s0 + ") -> field[0]=" + tag + ", 槽10=" + tag + " ✓ 单命中必中 (匹配器保持0)\n";
    return true;
}

// 3/4/5 大类(长名<=10B): 长名单直接放槽10, 前缀匹配单命中.
// 长名>10B 返回 false(调用方直接 return, 与 python 返回 None 行为一致)。
bool SummonQinLongname(ProcessMemory& mem, uintptr_t mb, int group_idx, int sub_idx,
                       std::string& log) {
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
    uint32_t p0 = mem.ReadUInt32(kQinTable + 8 + static_cast<uintptr_t>(rec) * kQinStride);
    std::string long_name = RStr(mem, p0, 32);
    if (Utf8ToGbk(long_name).size() > 10) return false;  // 长名放不进槽10
    RestoreQinBackup(mem);
    PatchMatcher(mem, mb, false, log);
    WriteSlot10(mem, long_name, log);
    uint8_t imm = 0;
    std::string s10;
    try {
        imm = mem.Read(mb + kOffMatcherImm, 1)[0];
        s10 = RStr(mem, kNameShanYi, 12);
    } catch (...) {
    }
    if (imm != 0 || s10 != long_name) {
        log += "  [!!] 自校验失败 matcher=" + std::to_string(imm) + " 槽10=" + s10 +
               " (期望 0/" + long_name + ")\n";
        return false;
    }
    log += std::string("  [秦兵·") + g.name + "/" +
           g.subs[static_cast<size_t>(sub_idx)].name + "] rec" + std::to_string(rec) + " 长名=" +
           long_name + " -> 槽10 ✓ 单命中必中\n";
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
        throw std::runtime_error("Steam 版模块基址无效(EasyRPG.dll 未找到?)");
    }

    std::map<std::string, std::string> d = ParseArgs(args);
    // 冲突检测: 三类互斥, 冲突回退 mon_type=0
    int given = 0;
    for (const char* k : {"mon_type", "assassin_type", "soldier_type"})
        if (d.count(k)) ++given;
    if (given >= 2) {
        log += "  [!!] 冲突: 同时指定多类, 按规则回退到 --mon_type 0 (狼)\n";
        std::string g = d.count("group") ? d["group"] : "0";
        d.clear();
        d["group"] = g;
    }
    int group = d.count("group") ? ParseInt(d["group"]) : 0;  // 0=队友可控(默认)
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
                log += "  [!!] 随机秦兵(soldier_type=0) 不接受 --soldier_sub_type (无大类子类)\n";
                return log;
            }
            // 复刻类别11路径: cmd_cike off + 槽10, 匹配器 field[0] (游戏系统随机)
            RestoreQinBackup(mem);
            PatchMatcher(mem, mb, false, log);
            CmdCike(mem, false, log);
            mem.Write(kNameShanYi, std::vector<unsigned char>(12, 0));  // 清空善翼槽
            slot = 10;
            label = "随机秦兵 (类别11路径, 游戏系统随机)";
        } else {
            int sub_idx = d.count("soldier_sub_type") ? ParseInt(d["soldier_sub_type"]) : 0;
            const QinGroup& g = QinGroups()[static_cast<size_t>(st - 1)];
            if (d.count("soldier_sub_type") &&
                (sub_idx < 0 || sub_idx >= static_cast<int>(g.subs.size()))) {
                log += "  --soldier_sub_type " + std::to_string(sub_idx) + " 非法: 大类[" +
                       std::to_string(st) + "]=" + g.name + " 子类须 " + LegalSubs(g) + "\n";
                return log;
            }
            label = std::string(g.name) + "/" + g.subs[static_cast<size_t>(sub_idx)].name;
            // 全部 1-6 都走 field[0] 匹配: 长名<=10B(3/4/5)直接放槽; >10B(1/2/6)重定向标签
            bool ok;
            if (st == 3 || st == 4 || st == 5)
                ok = SummonQinLongname(mem, mb, st - 1, sub_idx, log);
            else
                ok = SummonQinField0Tag(mem, mb, st - 1, sub_idx, log);
            if (!ok) return log;
            slot = 10;
        }
    } else if (d.count("assassin_type")) {
        int at = ParseInt(d["assassin_type"]);
        if (at != 0 && at != 1) {
            log += "  --assassin_type 须 0=弓箭刺客 / 1=带刀刺客\n";
            return log;
        }
        RestoreQinBackup(mem);
        PatchMatcher(mem, mb, false, log);
        CmdCike(mem, true, log);
        slot = (at == 0) ? 10 : 9;  // 弓箭=善翼槽10, 带刀=朱厌槽9
        label = (at == 0) ? "弓箭刺客 (cike_bow)" : "带刀刺客 (cike_knife)";
    } else {
        int mt = d.count("mon_type") ? ParseInt(d["mon_type"]) : 0;
        if (mt < 0 || mt > 9) {
            log += "  --mon_type 须 0-9: 0=狼 1=虎 2=熊 3=白狼 4=木偶 5=狗 6=虎蛟 7=狍鸮 "
                   "8=朱厌 9=善翼\n";
            return log;
        }
        RestoreQinBackup(mem);
        PatchMatcher(mem, mb, false, log);
        CmdCike(mem, false, log);
        slot = kMonSlots[mt];
        label = kMonNames[mt];
    }

    // ---- 召唤补丁(游戏召唤时读取);全部类型放行 = enable 最大召唤数量9(task7 #8)----
    PatchType(mem, mb, slot, log);
    PatchBound(mem, mb, 0x7f, log);
    PatchControl(mem, mb, 0x7f, log);
    PatchFriend(mem, mb, 1, log);
    PatchSkipMember(mem, mb, group == 1, log);
    log += "\n  [召唤] 目标=" + label + ", group=" + std::to_string(group) + " (" +
           (group == 0 ? "队友·可控" : "盟友·不可控·被动攻击敌人") + ")\n";
    log += "  现在去游戏里按召唤键即可 (补丁对下次召唤生效)\n";
    return log;
}

}  // namespace summon_steam
}  // namespace poq
