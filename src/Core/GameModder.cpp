#include "Core/GameModder.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include <windows.h>

namespace poq {

namespace {

std::string AddrHex8(uintptr_t addr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08llX", static_cast<unsigned long long>(addr));
    return buf;
}

std::string HexBytes(const std::vector<unsigned char>& data) {
    std::string out;
    char buf[8];
    for (size_t i = 0; i < data.size(); ++i) {
        if (i) out += ' ';
        std::snprintf(buf, sizeof(buf), "%02X", data[i]);
        out += buf;
    }
    return out;
}

void AppendLe32(std::vector<unsigned char>& v, uint32_t x) {
    v.push_back(static_cast<unsigned char>(x & 0xFF));
    v.push_back(static_cast<unsigned char>((x >> 8) & 0xFF));
    v.push_back(static_cast<unsigned char>((x >> 16) & 0xFF));
    v.push_back(static_cast<unsigned char>((x >> 24) & 0xFF));
}

// cmp dword [esi+off], imm(imm8 用 83 BE,否则 81 BE)——item_type 捕获条件用。
void AppendCmpEsiImm(std::vector<unsigned char>& v, uint32_t off, int32_t imm) {
    if (imm >= -128 && imm <= 127) {
        v.push_back(0x83);
        v.push_back(0xBE);
        AppendLe32(v, off);
        v.push_back(static_cast<unsigned char>(imm & 0xFF));
    } else {
        v.push_back(0x81);
        v.push_back(0xBE);
        AppendLe32(v, off);
        AppendLe32(v, static_cast<uint32_t>(imm));
    }
}

// 把一个 Unicode 码点编码为 GBK(CP936)字节。失败时用 "?" 替代(近似 Python 的 errors="replace")。
std::string GbkEncodeOne(uint32_t cp) {
    wchar_t wbuf[2];
    int n = 0;
    if (cp > 0xFFFF) {
        uint32_t v = cp - 0x10000;
        wbuf[0] = static_cast<wchar_t>(0xD800 + (v >> 10));
        wbuf[1] = static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
        n = 2;
    } else {
        wbuf[0] = static_cast<wchar_t>(cp);
        n = 1;
    }
    char buf[8];
    int len = ::WideCharToMultiByte(936, 0, wbuf, n, buf, 8, nullptr, nullptr);
    if (len <= 0) return "?";
    return std::string(buf, len);
}

// 把 UTF-8 字符串解码为 Unicode 码点序列(非法字节跳过)。
std::vector<uint32_t> Utf8ToCodepoints(const std::string& s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(c);
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            out.push_back(((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            out.push_back(((c & 0x0F) << 12) |
                          ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 2]) & 0x3F));
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            uint32_t cp = ((c & 0x07) << 18) |
                          ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                          ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 3]) & 0x3F);
            if (cp >= 0x10000 && cp <= 0x10FFFF) out.push_back(cp);
            i += 4;
        } else {
            ++i;
        }
    }
    return out;
}

}  // namespace

// ============================================================================
// 初始化:自注入(若已注入则复用)
// ============================================================================

uintptr_t GameModder::Initialize(unsigned int pid) {
    if (pid_ != pid) ResetState();
    if (!mem_.Open(pid, ProcessMemory::kProcessAllAccess))
        throw std::runtime_error("OpenProcess 失败(请以管理员身份运行;若游戏刚重启请先刷新进程)");
    pid_ = pid;
    try {
        const HookCfg& cfg = config_.Current().hook;
        auto base = ProcessMemory::FindModuleBase(pid, cfg.module);
        if (!base)
            throw std::runtime_error("未找到模块 " + cfg.module +
                                     ",请确认游戏正在运行(偏移配置可改 hook.module)。");
        hook_addr_ = *base + cfg.offset;
        EnsureInjection();
        return GetItemPointer();
    } catch (...) {
        mem_.Close();
        throw;
    }
}

void GameModder::EnsureInjection() {
    auto orig = mem_.Read(hook_addr_, 5);
    if (orig[0] == 0xE9) {
        // 已注入(CT / 上次残留):复用,不重复 patch
        uintptr_t slot = FindCurItemSlot(hook_addr_);
        if (!slot)
            throw std::runtime_error(
                "检测到 hook 已注入(0xE9),但无法从 cave 解析 cur_item 槽。"
                "请确认注入脚本/cave 结构正常。");
        cur_item_addr_ = slot;
    } else if (IsHookOriginal(orig)) {
        // 原指令:自注入(用实际位移重建 cave)
        orig_bytes_ = orig;
        Inject();
    } else {
        throw std::runtime_error(
            "hook 处字节未识别:" + HexBytes(orig) +
            "\n期望已注入(E9 jmp)或原指令模式 8B 44 86 ?? 5E"
            "(mov eax,[esi+eax*4+disp8]; pop esi)。"
            "游戏版本可能不同,请在偏移配置中调整 hook。");
    }
}

bool GameModder::IsHookOriginal(const std::vector<unsigned char>& orig) {
    // mov eax,[esi+eax*4+disp8]; pop esi(8B 44 86 ?? 5E,注意是 0x44 不是 0x04)
    return orig.size() >= 5 && orig[0] == 0x8B && orig[1] == 0x44 && orig[2] == 0x86 &&
           orig[4] == 0x5E;
}

void GameModder::Inject() {
    uint8_t disp = orig_bytes_[3];  // 原指令的位移字节(1E/30 等)
    const uint32_t alloc_size = 0x80;
    alloc_base_ = mem_.Alloc(alloc_size, hook_addr_);
    cur_item_addr_ = alloc_base_;
    uintptr_t cave_addr = alloc_base_ + 4;
    mem_.WriteUInt32(cur_item_addr_, 0);
    uintptr_t return_addr = hook_addr_ + 5;
    // 捕获条件:未配置 capture 时按 weight 类型,用 hook.weight_offset/weight_max 兜底
    const HookCfg& hk = config_.Current().hook;
    CaptureCfg cap = hk.capture ? *hk.capture : CaptureCfg{};
    if (cap.type == "weight") {
        cap.offset = hk.capture ? hk.capture->offset : hk.weight_offset;
        cap.max = hk.capture ? hk.capture->max : hk.weight_max;
    }
    auto cave = BuildCave(cur_item_addr_, cave_addr, return_addr, disp, cap);
    mem_.Write(cave_addr, cave);
    uint32_t jmp_rel = static_cast<uint32_t>(cave_addr - return_addr);
    std::vector<unsigned char> jmp;
    jmp.push_back(0xE9);
    AppendLe32(jmp, jmp_rel);
    mem_.Write(hook_addr_, jmp);
}

std::map<std::string, GameModder::CaptureBuilder> GameModder::CaptureBuilders() {
    // 捕获条件注册表:新增条件类型只需在此注册 + 配置 capture.type,无 if-else。
    static const std::map<std::string, CaptureBuilder> kBuilders = {
        {"weight", &GameModder::BuildCapWeight},
        {"item_type", &GameModder::BuildCapItemType},
    };
    return kBuilders;
}

std::vector<unsigned char> GameModder::BuildCapWeight(uintptr_t cur_item_addr,
                                                      const CaptureCfg& cap) {
    // 捕获条件 weight(v1.30 默认):重量条件,占用的寄存器入栈保护。
    //   push ecx
    //   mov ecx,[esi+offset]      ; 重量
    //   cmp ecx, max
    //   ja nocap
    //   mov [cur_item], esi       ; 捕获物品指针
    // nocap:
    //   pop ecx
    std::vector<unsigned char> cave;
    cave.push_back(0x51);                                       // push ecx
    cave.push_back(0x8B); cave.push_back(0x8E);                 // mov ecx,[esi+offset]
    AppendLe32(cave, cap.offset);
    cave.push_back(0x81); cave.push_back(0xF9);                 // cmp ecx, max
    AppendLe32(cave, cap.max);
    cave.push_back(0x77); cave.push_back(0x06);                 // ja +6(跳过 mov [cur_item],esi)
    cave.push_back(0x89); cave.push_back(0x35);                 // mov [cur_item],esi
    AppendLe32(cave, static_cast<uint32_t>(cur_item_addr));
    cave.push_back(0x59);                                       // pop ecx (nocap)
    return cave;
}

std::vector<unsigned char> GameModder::BuildCapItemType(uintptr_t cur_item_addr,
                                                        const CaptureCfg& cap) {
    // 捕获条件 item_type(steam/web.ct):类型条件,不占寄存器。
    //   cmp [esi+mark_offset], mark_value
    //   jne code
    //   cmp [esi+type_offset], type_min
    //   jb code
    //   cmp [esi+type_offset], type_max
    //   ja code
    //   mov [cur_item], esi
    // code:
    std::vector<unsigned char> conds;
    AppendCmpEsiImm(conds, cap.mark_offset, static_cast<int32_t>(cap.mark_value));
    size_t j1 = conds.size();
    conds.push_back(0x75); conds.push_back(0x00);               // jne code
    AppendCmpEsiImm(conds, cap.type_offset, static_cast<int32_t>(cap.type_min));
    size_t j2 = conds.size();
    conds.push_back(0x72); conds.push_back(0x00);               // jb code
    AppendCmpEsiImm(conds, cap.type_offset, static_cast<int32_t>(cap.type_max));
    size_t j3 = conds.size();
    conds.push_back(0x77); conds.push_back(0x00);               // ja code
    conds.push_back(0x89); conds.push_back(0x35);               // mov [cur_item],esi
    AppendLe32(conds, static_cast<uint32_t>(cur_item_addr));
    size_t code_pos = conds.size();
    // 回填三条条件跳转的目标(code 标签)
    conds[j1 + 1] = static_cast<unsigned char>((code_pos - (j1 + 2)) & 0xFF);
    conds[j2 + 1] = static_cast<unsigned char>((code_pos - (j2 + 2)) & 0xFF);
    conds[j3 + 1] = static_cast<unsigned char>((code_pos - (j3 + 2)) & 0xFF);
    return conds;
}

std::vector<unsigned char> GameModder::BuildCave(uintptr_t cur_item_addr, uintptr_t cave_addr,
                                                 uintptr_t return_addr, uint8_t disp,
                                                 const CaptureCfg& capture) {
    // "判断 + 捕获"段由捕获条件注册表按 capture.type 生成(无 if-else);
    // 这里只负责拼接原指令段(与 CT 等价)和 jmp 回跳。
    auto it = CaptureBuilders().find(capture.type);
    if (it == CaptureBuilders().end())
        throw std::runtime_error("未知捕获条件类型: " + capture.type + "(配置 hook.capture.type)");
    std::vector<unsigned char> cave = it->second(cur_item_addr, capture);
    cave.push_back(0x8B); cave.push_back(0x44);                 // mov eax,[esi+eax*4+disp]
    cave.push_back(0x86); cave.push_back(disp & 0xFF);
    cave.push_back(0x5E);                                       // pop esi
    uint32_t rel = static_cast<uint32_t>(return_addr - (cave_addr + cave.size() + 5));
    cave.push_back(0xE9);                                       // jmp returnhere
    AppendLe32(cave, rel);
    return cave;
}

uintptr_t GameModder::FindCurItemSlot(uintptr_t hook_addr) {
    // 从已注入的 jmp 解析 cave,定位 "mov [cur_item],esi"(89 35 <addr>)中的 cur_item 槽地址。
    auto jmp = mem_.Read(hook_addr, 5);
    if (jmp[0] != 0xE9) return 0;
    int32_t rel = static_cast<int32_t>(jmp[1]) |
                  (static_cast<int32_t>(jmp[2]) << 8) |
                  (static_cast<int32_t>(jmp[3]) << 16) |
                  (static_cast<int32_t>(jmp[4]) << 24);
    uintptr_t cave_addr = static_cast<uintptr_t>((hook_addr + 5 + rel) & 0xFFFFFFFFULL);
    std::vector<unsigned char> cave;
    try {
        cave = mem_.Read(cave_addr, 64);
    } catch (...) {
        return 0;
    }
    for (size_t i = 0; i + 6 <= cave.size(); ++i) {
        if (cave[i] == 0x89 && cave[i + 1] == 0x35) {
            uint32_t addr = static_cast<uint32_t>(cave[i + 2]) |
                            (static_cast<uint32_t>(cave[i + 3]) << 8) |
                            (static_cast<uint32_t>(cave[i + 4]) << 16) |
                            (static_cast<uint32_t>(cave[i + 5]) << 24);
            if (addr > 0x10000 && addr < 0xFFFFFF00) return addr;
        }
    }
    return 0;
}

void GameModder::ResetState() {
    hook_addr_ = 0;
    cur_item_addr_ = 0;
    alloc_base_ = 0;
    pool_base_ = 0;
    pool_size_ = 0;
    orig_bytes_.clear();
}

bool GameModder::IsHookInjected(unsigned int pid) {
    const HookCfg& cfg = config_.Current().hook;
    auto base = ProcessMemory::FindModuleBase(pid, cfg.module);
    if (!base) return false;
    try {
        return mem_.Read(*base + cfg.offset, 1)[0] == 0xE9;
    } catch (...) {
        return false;
    }
}

void GameModder::Close() {
    // 若是本工具自注入,则还原原始字节并释放 cave;复用外部注入则不还原。
    if (mem_.IsOpen() && hook_addr_ && !orig_bytes_.empty()) {
        try {
            mem_.Write(hook_addr_, orig_bytes_);
        } catch (...) {
        }
        try {
            if (alloc_base_) mem_.Free(alloc_base_);
        } catch (...) {
        }
    }
    // 释放动态节点池(steam/v1.30 用,需在句柄关闭前)
    try {
        if (pool_base_) mem_.Free(pool_base_);
    } catch (...) {
    }
    ResetState();
    mem_.Close();
}

// ============================================================================
// 物品指针
// ============================================================================

uintptr_t GameModder::GetItemPointer() {
    if (!cur_item_addr_) return 0;
    return static_cast<uintptr_t>(mem_.ReadUInt32(cur_item_addr_));
}

uintptr_t GameModder::GetItemBase() {
    uintptr_t p = GetItemPointer();
    if (p == 0 || p == 0xFFFFFFFF)
        throw std::runtime_error(
            "当前无手持物品:请在游戏中拾起物品并保持在光标上,再读取/保存。");
    return p + static_cast<uintptr_t>(
                   static_cast<long long>(config_.Current().item_base_adjust));
}

// ============================================================================
// 基础属性:读
// ============================================================================

BasicState GameModder::ReadBasicFields(uintptr_t item_base) {
    BasicState result;
    const ModeCfg& mode = config_.Current();
    // 读"类型"字段值,用于条件/强制判定
    const BasicFieldCfg* type_field = nullptr;
    for (const auto& f : mode.basic_fields)
        if (f.label == "类型") {
            type_field = &f;
            break;
        }
    std::optional<int> type_val;
    if (type_field)
        type_val = static_cast<int>(mem_.ReadUInt32(item_base + type_field->offset));

    for (const auto& f : mode.basic_fields) {
        if (f.type == "string_ptr") {
            result.strs[f.label] = ReadStringField(item_base, f);
        } else if (f.type == "word") {
            result.ints[f.label] = static_cast<int>(mem_.ReadWord(item_base + f.offset));
        } else {  // dword / enum
            result.ints[f.label] = static_cast<int>(mem_.ReadUInt32(item_base + f.offset));
        }
    }
    // force_when:类型为箭/矢时,孔数/耐久度/耐久度上限强制为指定值
    for (const auto& f : mode.basic_fields) {
        if (f.force_when && f.IsForced(type_val)) result.ints[f.label] = f.force_when->value;
    }
    // 弓箭数量:非箭/矢显示 -1
    for (const auto& f : mode.basic_fields) {
        if (f.conditional && f.conditional->field == "类型") {
            bool in = type_val &&
                      std::find(f.conditional->values.begin(), f.conditional->values.end(),
                                *type_val) != f.conditional->values.end();
            if (!in) result.ints[f.label] = -1;
        }
    }
    return result;
}

std::string GameModder::ReadStringField(uintptr_t item_base, const BasicFieldCfg& f) {
    try {
        uintptr_t ptr = mem_.ReadUInt32(item_base + f.offset);
        if (ptr == 0 || ptr == 0xFFFFFFFF) return "未知物品";
        auto s = mem_.ReadStringGbk(ptr, 100, f.codepage);
        return s ? *s : std::string("未知物品");
    } catch (...) {
        return "未知物品";
    }
}

void GameModder::WriteStringField(uintptr_t item_base, const BasicFieldCfg& f,
                                  const std::string& text) {
    // 把字符串(武器名字)原地写入 [item+offset] 指向的字符串位置。
    // 按 GBK 编码 + 结尾 0。最多 50 个字符(按字符边界截断,不截断多字节汉字),
    // 字节上限 100(50 个汉字正好 100 字节)。不改指针。
    try {
        uintptr_t ptr = mem_.ReadUInt32(item_base + f.offset);
        if (ptr == 0 || ptr == 0xFFFFFFFF) return;
        std::string data;
        int count = 0;
        for (uint32_t cp : Utf8ToCodepoints(text)) {
            if (count >= 50) break;
            std::string chb = GbkEncodeOne(cp);
            if (data.size() + chb.size() > 100) break;
            data += chb;
            ++count;
        }
        data.push_back('\0');
        mem_.Write(ptr, std::vector<unsigned char>(data.begin(), data.end()));
    } catch (...) {
    }
}

// ============================================================================
// 基础属性:写(含校验/clamp)
// ============================================================================

void GameModder::WriteBasicFields(uintptr_t item_base, const BasicState& values) {
    const ModeCfg& mode = config_.Current();
    // 类型值(force_when/conditional 都引用它):优先输入值,否则读内存
    const BasicFieldCfg* type_field = nullptr;
    for (const auto& f : mode.basic_fields)
        if (f.label == "类型") {
            type_field = &f;
            break;
        }
    std::optional<int> type_val;
    auto tv_it = values.ints.find("类型");
    if (tv_it != values.ints.end()) {
        type_val = tv_it->second;
    } else if (type_field) {
        try {
            type_val = static_cast<int>(mem_.ReadUInt32(item_base + type_field->offset));
        } catch (...) {
            type_val = std::nullopt;
        }
    }

    // 耐久度上限写入值(强制或 clamp),供耐久度 max_ref 使用
    const BasicFieldCfg* cap_field = nullptr;
    for (const auto& f : mode.basic_fields)
        if (f.label == "耐久度上限") {
            cap_field = &f;
            break;
        }
    std::optional<int> cap_val;
    if (cap_field && values.ints.count(cap_field->label) && !cap_field->readonly) {
        if (cap_field->IsForced(type_val))
            cap_val = cap_field->force_when->value;
        else
            cap_val = cap_field->Clamp(values.ints.at(cap_field->label));
    }

    // 快照当前耐久度/上限(补满判定用;强制时不需)
    const BasicFieldCfg* dur_field = nullptr;
    for (const auto& f : mode.basic_fields)
        if (f.max_ref) {
            dur_field = &f;
            break;
        }
    std::optional<int> mem_dur, mem_cap;
    if (dur_field && !dur_field->IsForced(type_val)) {
        try {
            mem_dur = static_cast<int>(mem_.ReadUInt32(item_base + dur_field->offset));
        } catch (...) {
            mem_dur = std::nullopt;
        }
    }
    if (cap_field) {
        try {
            mem_cap = static_cast<int>(mem_.ReadUInt32(item_base + cap_field->offset));
        } catch (...) {
            mem_cap = std::nullopt;
        }
    }

    // 第一遍:写无依赖、无强制的字段(含 类型、武器名字)
    for (const auto& f : mode.basic_fields) {
        bool has = values.ints.count(f.label) > 0 || values.strs.count(f.label) > 0;
        if (!has || f.readonly) continue;
        if (f.type == "string_ptr") {
            // 字符串字段(武器名字)直接写
            auto sit = values.strs.find(f.label);
            if (sit != values.strs.end())
                WriteStringField(item_base, f, sit->second);
            continue;
        }
        auto it = values.ints.find(f.label);
        if (it == values.ints.end()) continue;
        if (f.max_ref || f.conditional || f.force_when) continue;
        WriteField(item_base, f, f.Clamp(it->second));
    }

    // 第二遍:写依赖/强制字段
    for (const auto& f : mode.basic_fields) {
        auto it = values.ints.find(f.label);
        if (it == values.ints.end() || f.readonly) continue;
        if (f.force_when && f.IsForced(type_val)) {
            // 强制值(箭矢:孔数=0/耐久度=-1/耐久度上限=-1),bypass clamp
            WriteField(item_base, f, f.force_when->value);
        } else if (f.max_ref) {
            // 耐久度:有效上限 = min(254, 上限);改上限且新上限>原耐久度时补满
            int eff_max = cap_val ? *cap_val : (f.max ? *f.max : 254);
            int v = it->second;
            if (cap_val && mem_cap && mem_dur && *cap_val != *mem_cap && *cap_val > *mem_dur) {
                v = *cap_val;
            } else {
                if (f.min && v < *f.min) v = *f.min;
                if (v > eff_max) v = eff_max;
            }
            WriteField(item_base, f, v);
        } else if (f.conditional) {
            // 弓箭数量:仅类型为箭/矢时写入
            if (type_val &&
                std::find(f.conditional->values.begin(), f.conditional->values.end(),
                          *type_val) != f.conditional->values.end())
                WriteField(item_base, f, f.Clamp(it->second));
            // 否则不写入
        } else if (f.force_when) {
            // 有 force_when 但当前未强制:按普通字段 clamp 写入
            WriteField(item_base, f, f.Clamp(it->second));
        }
    }
}

void GameModder::WriteField(uintptr_t item_base, const BasicFieldCfg& f, int v) {
    if (f.type == "word")
        mem_.WriteWord(item_base + f.offset, static_cast<uint16_t>(v & 0xFFFF));
    else
        mem_.WriteUInt32(item_base + f.offset, static_cast<uint32_t>(v));
}

// ============================================================================
// 非固定属性:读(链表遍历)
// ============================================================================

std::vector<uintptr_t> GameModder::ReadNodeAddrs(uintptr_t item_base) {
    const AttrListCfg& cfg = config_.Current().attr_list;
    uintptr_t head = mem_.ReadUInt32(item_base + cfg.head_offset);
    std::vector<uintptr_t> addrs;
    uintptr_t addr = head;
    for (uint32_t i = 0; i < cfg.max_nodes; ++i) {
        if (addr == 0 || addr == 0xFFFFFFFF || addr < 0x10000) break;
        if (std::find(addrs.begin(), addrs.end(), addr) != addrs.end()) break;  // 环保护
        addrs.push_back(addr);
        addr = mem_.ReadUInt32(addr + cfg.next_offset);
    }
    return addrs;
}

std::vector<ItemProperty> GameModder::ReadAttributes(uintptr_t item_base) {
    const AttrListCfg& cfg = config_.Current().attr_list;
    std::vector<ItemProperty> result;
    for (uintptr_t addr : ReadNodeAddrs(item_base)) {
        try {
            uint32_t aid = mem_.ReadUInt32(addr + cfg.id_offset);
            uint32_t color = mem_.ReadUInt32(addr + cfg.color_offset);
            // precent 在 CT 中是 Float 类型,按 float 读再转 0/1
            float prec_f = mem_.ReadFloat(addr + cfg.precent_offset);
            uint32_t prec = prec_f != 0.0f ? 1 : 0;
            float val = mem_.ReadFloat(addr + cfg.value_offset);
            result.push_back(ItemProperty(addr, aid, color, prec, val));
        } catch (...) {
            continue;
        }
    }
    return result;
}

// ============================================================================
// 非固定属性:写(改字段值 + 需要时用静态节点池追加)
// ============================================================================

std::pair<int, int> GameModder::WriteAttributes(uintptr_t item_base,
                                                const std::vector<ItemProperty>& props) {
    const AttrListCfg& cfg = config_.Current().attr_list;
    auto existing = ReadNodeAddrs(item_base);
    int k = static_cast<int>(existing.size());
    int m = static_cast<int>(props.size());
    int written = 0;

    // 1) 写前 min(m,k) 行到现有节点(并自愈:哨兵/标志/前驱指针)
    int minmk = std::min(m, k);
    for (int i = 0; i < minmk; ++i) {
        WriteNode(existing[static_cast<size_t>(i)], props[static_cast<size_t>(i)]);
        NormalizeNode(existing[static_cast<size_t>(i)]);
        mem_.WriteUInt32(existing[static_cast<size_t>(i)] + cfg.prev_offset,
                         i > 0 ? existing[static_cast<size_t>(i - 1)] : 0);
        ++written;
    }

    // 2) 行数 < 节点数:真正从链表移除多余节点(新末尾 next=0;空列表则 head=0)
    if (m < k) {
        if (m > 0) {
            mem_.WriteUInt32(existing[static_cast<size_t>(m - 1)] + cfg.next_offset, 0);
        } else {
            mem_.WriteUInt32(item_base + cfg.head_offset, 0);
        }
        // 被移除的池节点清零(脱离链表,可被后续新增复用);堆节点不动
        for (int i = m; i < k; ++i) {
            uintptr_t addr = existing[static_cast<size_t>(i)];
            if (IsPoolNode(addr)) {
                try {
                    mem_.Write(addr, std::vector<unsigned char>(cfg.static_slot, 0));
                } catch (...) {
                }
            }
        }
    }

    // 3) 需要新增:静态节点池追加到链表尾部
    int need = m - k;
    if (need <= 0) return {written, 0};
    // 仅当链表读全(尾部 next==0,或空链表 head==0)才允许追加
    bool can_append;
    if (k > 0) {
        uint32_t tail_next =
            mem_.ReadUInt32(existing[static_cast<size_t>(k - 1)] + cfg.next_offset);
        can_append = tail_next == 0 || tail_next == 0xFFFFFFFF;
    } else {
        uint32_t head = mem_.ReadUInt32(item_base + cfg.head_offset);
        can_append = head == 0 || head == 0xFFFFFFFF;
    }
    if (!can_append) return {written, need};

    std::set<uintptr_t> used(existing.begin(), existing.end());
    std::optional<uintptr_t> prev =
        k > 0 ? std::optional<uintptr_t>(existing[static_cast<size_t>(k - 1)])
              : std::nullopt;
    int added = 0;
    for (int i = k; i < m; ++i) {
        uintptr_t addr = FindFreeStaticNode(used);
        if (addr == 0) continue;  // 池满/找不到空闲槽:跳过该行,不强行写
        used.insert(addr);
        WriteNode(addr, props[static_cast<size_t>(i)]);
        // 初始化新节点的关键字段(否则游戏当"无效节点",忽略颜色、显示默认色):
        //   prev(0x04)=前驱(头节点为 0);sentinel(0x08)=0xFFFFFFFF;node_flag(0x10)=1
        mem_.WriteUInt32(addr + cfg.prev_offset, prev ? *prev : 0);
        mem_.WriteUInt32(addr + cfg.sentinel_offset, 0xFFFFFFFF);
        mem_.WriteUInt32(addr + cfg.node_flag_offset, 1);
        if (prev) {
            mem_.WriteUInt32(*prev + cfg.next_offset, addr);
        } else {
            mem_.WriteUInt32(item_base + cfg.head_offset, addr);
        }
        prev = addr;
        ++added;
    }
    if (prev) mem_.WriteUInt32(*prev + cfg.next_offset, 0);
    return {written + added, need - added};
}

std::pair<uintptr_t, uint32_t> GameModder::PoolCfg() {
    // 返回当前节点池的 (base, size),按 pool_type 配置分发(无 if-else):
    // - "static":固定静态区(static_base/static_size)。
    // - "dynamic":VirtualAllocEx 动态分配(steam/v1.30,对齐 web.ct)——游戏不认识
    //   这块内存、不会 free,地址每次运行时分配总是有效;分配失败返回 (0,0),调用方跳过新增。
    const AttrListCfg& cfg = config_.Current().attr_list;
    if (cfg.pool_type == "dynamic") {
        if (!pool_base_ && mem_.IsOpen()) {
            // 动态池:保证 ≥ 30 槽
            uint32_t slots = std::max<uint32_t>(30, cfg.static_size / cfg.static_slot);
            uint32_t size = slots * cfg.static_slot;
            uintptr_t base = 0;
            try {
                base = mem_.Alloc(size);
                mem_.Write(base, std::vector<unsigned char>(size, 0));
            } catch (...) {
                base = 0;
            }
            if (base) {
                pool_base_ = base;
                pool_size_ = size;
            }
        }
        return {pool_base_, pool_size_};
    }
    return {cfg.static_base, cfg.static_size};
}

bool GameModder::IsPoolNode(uintptr_t addr) {
    auto pc = PoolCfg();
    return pc.first && addr >= pc.first && addr < pc.first + pc.second;
}

uintptr_t GameModder::FindFreeStaticNode(const std::set<uintptr_t>& used) {
    // 在节点池里找一块全 0 的 static_slot 槽(最高地址优先,对齐槽网格)。
    // 池由 PoolCfg() 决定(固定静态池 / 动态池);槽网格对齐 = base % static_slot,
    // 保证我们用的槽与原版修改器后续分配的槽不错位冲突。全 0 要求自然避开已用节点/
    // 静态数据表/锚点等非零区域,绝不写非零字节。找不到返回 0。
    const AttrListCfg& cfg = config_.Current().attr_list;
    uint32_t size = cfg.static_slot;
    auto pc = PoolCfg();
    if (!pc.first) return 0;
    uintptr_t pool_end = pc.first + pc.second;
    uint32_t align = static_cast<uint32_t>(pc.first % size);
    // 从池顶向下扫,且候选地址与槽网格对齐
    uintptr_t addr = pool_end - size;
    addr -= (addr - align) % size;
    while (addr >= pc.first) {
        bool ok = true;
        for (uintptr_t u : used) {
            if (addr < u + size && u < addr + size) {
                ok = false;  // 与已用节点重叠
                break;
            }
        }
        if (ok) {
            try {
                auto data = mem_.Read(addr, size);
                bool all_zero = std::all_of(data.begin(), data.end(),
                                            [](unsigned char b) { return b == 0; });
                if (all_zero) return addr;
            } catch (...) {
            }
        }
        addr -= size;
    }
    return 0;
}

void GameModder::WriteNode(uintptr_t addr, const ItemProperty& p) {
    // 写节点字段值。id/color 为 DWORD,precent/value 为 Float(对齐 CT)。
    const AttrListCfg& cfg = config_.Current().attr_list;
    mem_.WriteUInt32(addr + cfg.id_offset, p.attr_id);
    mem_.WriteUInt32(addr + cfg.color_offset, p.color);
    // precent 在 CT 中是 Float:写 1.0/0.0,而非 uint32 1(否则游戏按 float 读得到非零垃圾值)
    mem_.WriteFloat(addr + cfg.precent_offset, p.precent ? 1.0f : 0.0f);
    mem_.WriteFloat(addr + cfg.value_offset, p.value);
}

void GameModder::NormalizeNode(uintptr_t addr) {
    // 自愈:确保节点是"有效"的——sentinel(0x08)=0xFFFFFFFF、node_flag(0x10)=1。
    // 否则游戏把节点当无效,忽略其 0x14 颜色、显示默认色(实测蓝)。幂等。
    const AttrListCfg& cfg = config_.Current().attr_list;
    try {
        if (mem_.ReadUInt32(addr + cfg.sentinel_offset) != 0xFFFFFFFF)
            mem_.WriteUInt32(addr + cfg.sentinel_offset, 0xFFFFFFFF);
        if (mem_.ReadUInt32(addr + cfg.node_flag_offset) != 1)
            mem_.WriteUInt32(addr + cfg.node_flag_offset, 1);
    } catch (...) {
    }
}

// ============================================================================
// 内存查看
// ============================================================================

std::vector<unsigned char> GameModder::ReadMemoryDump(uintptr_t addr, int size) {
    return mem_.Read(addr, size);
}

}  // namespace poq
