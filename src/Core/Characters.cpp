// 队伍角色属性结构体定义与读写(对应 Python poqeditor_py/characters.py)。
//
// 仅实现 UI 用到的固定偏移定位(LocateByOffsets)与字段读写,不包含全内存扫描
// (locate_members)与 CharCapture AOB hook--Python 的 main_window 也只用固定偏移定位。

#include "Core/Characters.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>
#include <windows.h>

namespace poq {

// ---------------------------------------------------------------------------
// GBK 编码辅助(与 GameModder.cpp 内同名函数等价,这里为保持 Characters 自包含)
// ---------------------------------------------------------------------------
namespace {

// 把一个 Unicode 码点编码为 GBK(CP936)字节。失败用 "?" 替代。
std::string GbkEncodeOne(uint32_t cp) {
    wchar_t wbuf[2];
    int n = 0;
    if (cp > 0xFFFF) {
        wbuf[0] = L'?';
        n = 1;
    } else {
        wbuf[0] = static_cast<wchar_t>(cp);
        n = 1;
    }
    char buf[8];
    int len = ::WideCharToMultiByte(936, 0, wbuf, n, buf, sizeof(buf), nullptr, nullptr);
    if (len <= 0) return "?";
    return std::string(buf, static_cast<size_t>(len));
}

// 把 UTF-8 字符串解码为 Unicode 码点序列(非法字节跳过)。
std::vector<uint32_t> Utf8ToCodepoints(const std::string& s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        size_t need = 0;
        if (c < 0x80) {
            cp = c;
            need = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            need = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            need = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            need = 4;
        } else {
            ++i;
            continue;
        }
        if (i + need > s.size()) break;
        bool ok = true;
        for (size_t k = 1; k < need; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) {
            ++i;
            continue;
        }
        out.push_back(cp);
        i += need;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 字段表
// ---------------------------------------------------------------------------
// 全部可选队友白名单(名字 -> 职业显示名,共 16 人)。
// 定位时槽内实际名字必须命中白名单才认(排除 NPC/杂物/脏数据);
// 下拉显示格式 = "职业-名字"(如 刺客-荆无忌)。
const std::map<std::string, std::string>& KnownMembers() {
    static const std::map<std::string, std::string> kMembers = {
        {"荆无忌", "刺客"}, {"田玥琳", "刺客"}, {"赵茜", "刺客"},
        {"孔刚", "力士"},   {"冒顿", "力士"},   {"黥布", "力士"},
        {"方仲", "术士"},   {"昆蒲", "术士"},   {"廖异", "术士"},   {"张良", "术士"},
        {"淳于秋", "巫师"}, {"瑶彤", "巫师"},   {"茵珠", "巫师"},
        {"扶苏", "游侠"},   {"韩信", "游侠"},   {"李由", "游侠"},
    };
    return kMembers;
}

const std::vector<CharFieldCfg>& GlobalFields() {
    static const std::vector<CharFieldCfg> kFields = {
        CharFieldCfg{"金钱", 0x2C, 0, CharKind::Dword, {}, 0, 99999999, std::nullopt},
    };
    return kFields;
}

const std::vector<CharFieldCfg>& CharFields() {
    static const std::vector<CharFieldCfg> kFields = {
        CharFieldCfg{"姓名", 0x00, 0, CharKind::StringPtr, {}, std::nullopt, std::nullopt, std::nullopt},
        CharFieldCfg{"别名", 0x04, 0, CharKind::StringPtr, {}, std::nullopt, std::nullopt, std::nullopt},
        CharFieldCfg{"职业", 0x398, 0, CharKind::Enum, {{1, "游侠"}, {2, "力士"}, {3, "刺客"}, {4, "术士"}, {5, "巫师"}}, std::nullopt, std::nullopt, std::string("谨慎修改")},
        CharFieldCfg{"性别", 0x10, 0, CharKind::Enum, {{0, "男"}, {1, "女"}}, std::nullopt, std::nullopt, std::nullopt},
        CharFieldCfg{"等级", 0x0C, 0, CharKind::Dword, {}, 1, 99, std::nullopt},
        CharFieldCfg{"年龄", 0x14, 0, CharKind::Dword, {}, 1, 99, std::nullopt},
        // 经验/下一级 合成一对(UI 一个字段两个框, 中间斜杠; 与生命/精气同款)
        CharFieldCfg{"经验", 0x1C, 0x20, CharKind::Pair, {}, 0, std::nullopt, std::nullopt},
        CharFieldCfg{"声望", 0x28, 0, CharKind::Word, {}, 0, 65535, std::nullopt},
        CharFieldCfg{"生命", 0x30, 0x34, CharKind::Pair, {}, 0, 999999, std::nullopt},
        CharFieldCfg{"精气", 0x38, 0x3C, CharKind::Pair, {}, 0, 999999, std::nullopt},
        CharFieldCfg{"力量", 0x48, 0, CharKind::Dword, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"体质", 0x4C, 0, CharKind::Dword, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"敏捷", 0x50, 0, CharKind::Dword, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"悟性", 0x58, 0, CharKind::Dword, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"魅力", 0x5C, 0, CharKind::Dword, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"智慧", 0x60, 0, CharKind::Dword, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"普通攻击", 0x64, 0x68, CharKind::Pair, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"金攻击", 0x6C, 0x70, CharKind::Pair, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"木攻击", 0x74, 0x78, CharKind::Pair, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"水攻击", 0x7C, 0x80, CharKind::Pair, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"火攻击", 0x84, 0x88, CharKind::Pair, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"土攻击", 0x8C, 0x90, CharKind::Pair, {}, 0, 9999, std::nullopt},
        CharFieldCfg{"普通防御", 0x94, 0, CharKind::Dword, {}, 0, 99999, std::nullopt},
        CharFieldCfg{"金防御", 0x98, 0, CharKind::Dword, {}, 0, 99999, std::nullopt},
        CharFieldCfg{"木防御", 0x9C, 0, CharKind::Dword, {}, 0, 99999, std::nullopt},
        CharFieldCfg{"水防御", 0xA0, 0, CharKind::Dword, {}, 0, 99999, std::nullopt},
        CharFieldCfg{"火防御", 0xA4, 0, CharKind::Dword, {}, 0, 99999, std::nullopt},
        CharFieldCfg{"土防御", 0xA8, 0, CharKind::Dword, {}, 0, 99999, std::nullopt},
        CharFieldCfg{"闪避率", 0xAC, 0, CharKind::Dword, {}, 0, 99999, std::nullopt},
        CharFieldCfg{"命中率", 0xB0, 0, CharKind::Dword, {}, 0, 99999, std::nullopt},
        // 视野: +0x198, 装"视野 +X"装备时该字段 +(X)(2026-09-24 差分实测: 175 -> 200)。
        // 注: 实测直接 WriteProcessMemory 改此字段会导致游戏崩溃(原因待查, 见 docs/260929_todo_list.md
        // 第四节)。按用户要求**保持可编辑**(崩溃机制要靠实验定位, 不能先用只读掩盖),
        // 因此这里 writable 仍为 true。
        CharFieldCfg{"视野", 0x198, 0, CharKind::Dword, {}, 0, 99999, std::nullopt},
        CharFieldCfg{"剩余属性点", 0x274, 0, CharKind::Word, {}, 0, 65535, std::nullopt},
        CharFieldCfg{"剩余技能点", 0x278, 0, CharKind::Word, {}, 0, 65535, std::nullopt},
    };
    return kFields;
}

const std::map<std::string, std::map<std::string, uint32_t>>& MemberOffsets() {
    static const std::map<std::string, std::map<std::string, uint32_t>> kOffsets = {
        // Steam 版:EasyRPG.dll
        {"EasyRPG.dll", {
            {"扶苏", 0x4797FC}, {"孔刚", 0x4799A4}, {"田玥琳", 0x479AE8},
            {"张良", 0x479C14}, {"瑶彤", 0x479D68},
        }},
        // v1.30 版:EasyRPG116.dll(CE 实测)
        {"EasyRPG116.dll", {
            {"扶苏", 0x37459C}, {"孔刚", 0x374744}, {"田玥琳", 0x374888},
            {"张良", 0x3749B4}, {"瑶彤", 0x374B08},
        }},
    };
    return kOffsets;
}

// ---------------------------------------------------------------------------
// 固定偏移定位
// ---------------------------------------------------------------------------
namespace {
// 去掉首尾空白(名字字段常带尾部空格/制表填充)。
std::string TrimName(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
}  // namespace

std::vector<std::pair<std::string, uintptr_t>> LocateByOffsets(ProcessMemory& mem, unsigned int pid,
                                                 const std::string& module, bool strict_filter) {
    std::vector<std::pair<std::string, uintptr_t>> result;
    auto base_opt = ProcessMemory::FindModuleBase(pid, module);
    if (!base_opt) return result;
    uintptr_t base = *base_opt;
    auto it = MemberOffsets().find(module);
    if (it == MemberOffsets().end()) return result;

    // 固定偏移只作"窗口范围"用;实际槽位通过窗口内扫描识别(更稳)。
    // 已知偏移之间的地址可能指向非成员(如技能表),不逐档写死。
    std::vector<uint32_t> offsets;
    for (const auto& kv : it->second) offsets.push_back(kv.second);
    std::sort(offsets.begin(), offsets.end());
    const uint32_t kScanMargin = 0x500;  // 首尾各留余量,保证覆盖真实槽位
    uint32_t scan_lo = offsets.front();
    uint32_t scan_hi = offsets.back() + kScanMargin;

    std::set<std::string> seen;  // 同名槽位合并(保留首次)
    for (uint32_t off = scan_lo; off <= scan_hi; off += 4) {
        try {
            unsigned int edi = mem.ReadUInt32(base + off);
            if (!(edi > 0x10000 && edi < 0xFFFFFF00)) continue;
            uintptr_t struct_base = edi + 0x28;  // 结构体 = EDI + 0x28
            unsigned int np = mem.ReadUInt32(struct_base);  // 姓名指针 @ +0x00
            if (!(np > 0x10000 && np < 0xFFFFFF00)) continue;
            auto s_opt = mem.ReadStringGbk(np, 40, 936);
            if (!s_opt) continue;
            std::string name = TrimName(*s_opt);  // 名字字段常带尾部空格,先 Trim
            if (name.empty()) continue;
            // 白名单过滤(开关控制):名字必须是 16 个可选队友之一
            // (排除 NPC/杂物/脏数据;游戏槽位会随存档换人,不能按偏移写死身份)。
            // 关闭时不过滤(可能识别出其他角色,UI 上有开关+警告)。
            if (strict_filter && !KnownMembers().count(name)) continue;
            // 有效角色校验:等级 1-99(排除空槽残留)。
            // 生命/精气不校验:阵亡(hp=0)或被修改器改大(>99999)都是正常状态,
            // 之前的上下限过滤会把这类成员从列表里挤掉(如只剩 3 人)。
            if (strict_filter) {
                unsigned int lvl = mem.ReadUInt32(struct_base + 0x0C);
                if (!(lvl >= 1 && lvl <= 99)) continue;
            }
            // 同名槽位合并(保留首次)
            if (seen.count(name)) continue;
            seen.insert(name);
            result.emplace_back(name, struct_base);
        } catch (...) {
            continue;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// 调试辅助:不过滤地按槽位顺序读名字 + 校验详情
// ---------------------------------------------------------------------------
std::vector<std::string> ProbeSlotNames(ProcessMemory& mem, unsigned int pid,
                                        const std::string& module) {
    std::vector<std::string> out;
    auto base_opt = ProcessMemory::FindModuleBase(pid, module);
    if (!base_opt) return out;
    uintptr_t base = *base_opt;
    auto it = MemberOffsets().find(module);
    if (it == MemberOffsets().end()) return out;

    std::vector<uint32_t> offsets;
    for (const auto& kv : it->second) offsets.push_back(kv.second);
    std::sort(offsets.begin(), offsets.end());

    // 与定位一致:扫描整个窗口(而非逐档),显示每个槽位读到的内容。
    const uint32_t kScanMargin = 0x500;
    const uint32_t scan_lo = offsets.front();
    const uint32_t scan_hi = offsets.back() + kScanMargin;

    for (uint32_t off = scan_lo; off <= scan_hi; off += 4) {
        try {
            unsigned int edi = mem.ReadUInt32(base + off);
            if (!(edi > 0x10000 && edi < 0xFFFFFF00)) {
                out.emplace_back("槽无效");
                continue;
            }
            uintptr_t struct_base = edi + 0x28;
            unsigned int np = mem.ReadUInt32(struct_base);
            if (!(np > 0x10000 && np < 0xFFFFFF00)) {
                out.emplace_back("名字指针无效");
                continue;
            }
            auto s_opt = mem.ReadStringGbk(np, 40, 936);
            if (!s_opt) {
                out.emplace_back("名字读失败");
                continue;
            }
            std::string name = TrimName(*s_opt);
            unsigned int lvl = 0;
            try {
                lvl = mem.ReadUInt32(struct_base + 0x0C);
            } catch (...) {
            }
            std::string line;
            if (name.empty()) {
                line = "(空名字)";
            } else {
                line = "\"" + name + "\" 等级=" + std::to_string(lvl);
                bool in_wl = KnownMembers().count(name) != 0;
                bool lvl_ok = lvl >= 1 && lvl <= 99;
                line += std::string("\n  白名单:") + (in_wl ? "过" : "×不认") +
                        " 等级:" + (lvl_ok ? "过" : "×") +
                        (in_wl && lvl_ok ? " → 可收" : " → 被过滤");
            }
            out.push_back(line);
        } catch (...) {
            out.emplace_back("读取异常");
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 字段读写
// ---------------------------------------------------------------------------
std::optional<CharValue> ReadCharField(ProcessMemory& mem, uintptr_t base, const CharFieldCfg& f) {
    try {
        CharValue v;
        if (f.kind == CharKind::Pair) {
            v.is_pair = true;
            v.lo = static_cast<int>(mem.ReadUInt32(base + f.offset_lo));
            v.hi = static_cast<int>(mem.ReadUInt32(base + f.offset_hi));
            return v;
        }
        if (f.kind == CharKind::StringPtr) {
            uintptr_t ptr = mem.ReadUInt32(base + f.offset_lo);
            if (ptr == 0 || ptr == 0xFFFFFFFF) {
                v.str = "未知";
                return v;
            }
            auto s = mem.ReadStringGbk(ptr, 100, 936);
            v.str = (s && !s->empty()) ? *s : "未知";
            return v;
        }
        if (f.kind == CharKind::Word) {
            v.lo = static_cast<int>(mem.ReadWord(base + f.offset_lo));
            return v;
        }
        // dword / enum
        v.lo = static_cast<int>(mem.ReadUInt32(base + f.offset_lo));
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

bool WriteCharField(ProcessMemory& mem, uintptr_t base, const CharFieldCfg& f, const CharValue& v) {
    try {
        if (f.kind == CharKind::Pair) {
            mem.WriteUInt32(base + f.offset_lo, static_cast<unsigned int>(v.lo));
            mem.WriteUInt32(base + f.offset_hi, static_cast<unsigned int>(v.hi));
            return true;
        }
        if (f.kind == CharKind::StringPtr) {
            // 字符串原地写。计划 P1/P2(#10): "上限 50 字符"不代表目标 buffer 有 50 字符容量 ——
            // 旧实现无条件写最多 101 字节, 旧字符串只分配了很小空间时会覆盖相邻堆对象。
            // 保守策略: 只写"不超过当前字符串实际长度"的内容(装得下才写), 硬上限 100 字节。
            uintptr_t ptr = mem.ReadUInt32(base + f.offset_lo);
            if (ptr == 0 || ptr == 0xFFFFFFFF) return false;
            const size_t kMaxStringBytes = 100;  // 50 个汉字
            size_t curLen = 0;
            try {
                auto opt = mem.ReadStringGbk(ptr, kMaxStringBytes + 1, 936);
                curLen = opt ? opt->size() : 0;
            } catch (...) {
                curLen = 0;  // 读不到现值 -> 按最小容量处理
            }
            const size_t limit = std::min(curLen, kMaxStringBytes);
            std::string data;
            for (uint32_t cp : Utf8ToCodepoints(v.str)) {
                std::string chb = GbkEncodeOne(cp);
                if (data.size() + chb.size() > limit) break;
                data += chb;
            }
            data.push_back('\0');
            mem.Write(ptr, std::vector<unsigned char>(data.begin(), data.end()));
            return true;
        }
        if (f.kind == CharKind::Word) {
            mem.WriteWord(base + f.offset_lo, static_cast<uint16_t>(v.lo));
            return true;
        }
        // dword / enum
        mem.WriteUInt32(base + f.offset_lo, static_cast<unsigned int>(v.lo));
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<std::string> ValidateValue(const CharFieldCfg& f, const CharValue& v) {
    if (f.kind == CharKind::StringPtr) {
        if (v.str.size() > 50) return f.label + ":字符串长度不能超过 50";
        return std::nullopt;
    }
    if (f.kind == CharKind::Enum) {
        if (!f.enum_map.count(v.lo)) return f.label + ":无效选项";
        return std::nullopt;
    }
    if (f.kind == CharKind::Pair) {
        if (f.min && (v.lo < *f.min || v.hi < *f.min)) return f.label + ":不能小于 " + std::to_string(*f.min);
        if (f.max && (v.lo > *f.max || v.hi > *f.max)) return f.label + ":不能大于 " + std::to_string(*f.max);
        if (v.lo > v.hi) return f.label + ":下限不能大于上限";
        return std::nullopt;
    }
    // dword / word
    if (f.min && v.lo < *f.min) return f.label + ":不能小于 " + std::to_string(*f.min);
    if (f.max && v.lo > *f.max) return f.label + ":不能大于 " + std::to_string(*f.max);
    return std::nullopt;
}

std::map<std::string, CharValue> ReadAllChar(ProcessMemory& mem, uintptr_t base) {
    std::map<std::string, CharValue> out;
    for (const auto& f : CharFields()) {
        auto v = ReadCharField(mem, base, f);
        if (v) out[f.label] = *v;
    }
    return out;
}

std::map<std::string, CharValue> ReadGlobalChar(ProcessMemory& mem, uintptr_t base) {
    std::map<std::string, CharValue> out;
    for (const auto& f : GlobalFields()) {
        auto v = ReadCharField(mem, base, f);
        if (v) out[f.label] = *v;
    }
    return out;
}

}  // namespace poq
