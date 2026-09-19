#include "GamFile.h"

#include "Core/ItemData.h"

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <windows.h>

namespace poq {

static std::wstring Utf8ToWidePath(const std::string& s) {
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static FILE* FOpenW(const std::string& path, const wchar_t* mode) {
    return _wfopen(Utf8ToWidePath(path).c_str(), mode);
}

// ---------------------------------------------------------------------------
// 基础字节工具(与 python gamkit 一致)
// ---------------------------------------------------------------------------
static const uint8_t kACH[] = {
    'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t',
    'u','v','w','x','y','z','0','1','2','3','4','5','6','7','8','9','_'
};
static bool IsAliasCh(uint8_t b) {
    for (size_t i = 0; i < sizeof(kACH); ++i) if (kACH[i] == b) return true;
    return false;
}

uint32_t GamFile::X59Dw(const std::vector<uint8_t>& b, size_t pos, uint8_t k0) {
    if (pos + 4 > b.size()) return 0xFFFFFFFFu;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= (uint32_t)((b[pos + (size_t)i] ^ (uint8_t)(k0 + i)) & 0xFF) << (8 * i);
    }
    return v;
}

static void RawX59Range(const uint8_t* src, size_t n, uint8_t k0, std::vector<uint8_t>& out);

static uint32_t D32LE(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void X59Range(const std::vector<uint8_t>& b, size_t pos, size_t n, uint8_t k0,
                     std::vector<uint8_t>& out) {
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = b[pos + i] ^ (uint8_t)(k0 + (uint8_t)i);
}

uint32_t GamFile::DwAt(const std::vector<uint8_t>& c, size_t off) {
    if (off + 4 > c.size()) return 0xFFFFFFFFu;
    return D32LE(&c[off]) ^ 0xAAABACADu;
}

bool GamFile::DecStr(const std::vector<uint8_t>& c, size_t off, std::string& out, size_t& used) {
    uint32_t slen = DwAt(c, off);
    if (slen < 2 || slen > 0x2000 || off + 4 + slen > c.size()) return false;
    size_t strlen = slen - 1;
    std::vector<uint8_t> raw(strlen);
    for (size_t i = 0; i < strlen; ++i)
        raw[i] = c[off + 4 + i] ^ (uint8_t)((strlen - 0x56 - i) & 0xFF);
    out.assign(raw.begin(), raw.end());
    used = 4 + slen;
    return true;
}

static uint32_t RawEncDw(uint32_t v) {  // 把 dword 值编码(嵌套 ^KEY)
    return v ^ 0xAAABACADu;
}

// ---------------------------------------------------------------------------
// 别名 / 记录解析
// ---------------------------------------------------------------------------
namespace {
/// 在内容里找"第一个 3..48 位全小写别名"标准字符串; 返回别名 + aend。
bool find_alias(const std::vector<uint8_t>& c, std::string& alias, size_t& aend) {
    size_t n = c.size();
    size_t hi = n > 0x200 ? 0x200 : n;
    for (size_t off = 0x04; off + 8 < hi; ++off) {
        uint32_t slen = GamFile::DwAt(c, off);
        if (slen < 3 || slen > 49) continue;
        size_t strlen = slen - 1;
        if (off + 4 + strlen > n) continue;
        std::string s;
        bool ok = true;
        size_t nz = 0;
        for (size_t i = 0; i < strlen; ++i) {
            uint8_t b = c[off + 4 + i] ^ (uint8_t)((strlen - 0x56 - i) & 0xFF);
            if (b == 0) break;
            if (!IsAliasCh(b)) { ok = false; break; }
            s.push_back((char)b);
            ++nz;
        }
        if (ok && nz >= 3) {
            alias = s;
            aend = off + 4 + slen;  // 别名尾
            return true;
        }
    }
    return false;
}
}  // namespace

static bool MakeItem(size_t pos, const std::vector<uint8_t>& buf, GamItem& it) {
    uint32_t L = GamFile::X59Dw(buf, pos);
    if (L < 0x100 || L > 0x6000 || pos + 4 + L + 8 > buf.size()) return false;
    uint32_t cnt = GamFile::X59Dw(buf, pos + 4 + L);
    uint32_t total = GamFile::X59Dw(buf, pos + 4 + L + 4);
    if (cnt != 0 || total != L + 12) return false;
    X59Range(buf, pos + 4, L, 0x59, it.content);
    it.len = L;
    it.pos = pos;
    it.rec_end = pos + 4 + L + 8;
    it.dirty = false;
    size_t aend = 0;
    if (!find_alias(it.content, it.alias, aend)) {
        it.alias = "?";
        aend = 0x1C + L + 1;
    }
    it.aend = aend;
    // 中文名: 位于 [+0x04 路径字段][+4] 处(路径字段长度可变, 决定名字偏移)
    {
        std::string pv;
        size_t pused = 0;
        size_t noff = 0xD;
        if (GamFile::DecStr(it.content, 0x4, pv, pused)) noff = 0x4 + pused + 4;
        it.name_off = noff;
        std::string nm;
        size_t nused = 0;
        if (GamFile::DecStr(it.content, noff, nm, nused)) it.name = nm;
    }
    // 树过滤缓存: 拼好"中文名(别名)"并转小写, 过滤器每帧直接比对(P1)
    {
        auto low = [](const std::string& x) {
            std::string r = x;
            for (char& c : r)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            return r;
        };
        it.alias_low = low(it.alias);
        if (it.name.empty() || it.alias == "?") it.disp_low = it.alias_low;
        else it.disp_low = low(it.name + "(" + it.alias + ")");
    }
    it.ParseAffixes();
    return true;
}

// ---------------------------------------------------------------------------
// 固定字段表(相对 aend)
// ---------------------------------------------------------------------------
namespace {
struct FieldDef { const char* name; uint32_t off; };
const FieldDef kFields[] = {
    {"lvl", 0x0C}, {"dur", 0x30}, {"durmax", 0x34}, {"f_c8", 0xC8},
    {"price", 0xCC}, {"wgt", 0xD0}, {"type", 0xD4}, {"ident", 0xE0},
    {"f_e4", 0xE4}, {"f_e8", 0xE8}, {"sockets", 0xFC}, {"f_100", 0x100},
    {"f_104", 0x104}, {"f_10c", 0x10C}, {"f_110", 0x110}, {"f_118", 0x118},
    {"f_11c", 0x11C}, {"f_124", 0x124}, {"req_lvl", 0x128}, {"req_str", 0x12C},
    {"req_agi", 0x130}, {"req_con", 0x134}, {"wx", 0x140}, {"arrows", 0x370},
};
}  // namespace

const std::vector<GamFile::FieldDesc>& GamFile::FieldList() {
    static const std::vector<FieldDesc> k = {
        {"lvl", "等级"}, {"dur", "耐久"}, {"durmax", "耐久上限"}, {"price", "价格"},
        {"wgt", "重量"}, {"type", "类型"}, {"ident", "鉴定"}, {"sockets", "孔数"},
        {"req_lvl", "等级需求"}, {"req_str", "力量需求"}, {"req_agi", "敏捷需求"},
        {"req_con", "体质需求"}, {"wx", "五行"}, {"arrows", "箭支数量"},
    };
    return k;
}

int32_t GamItem::GetField(const char* name, bool* ok) const {
    const FieldDef* def = nullptr;
    for (const auto& f : kFields) if (strcmp(f.name, name) == 0) { def = &f; break; }
    if (!def) { if (ok) *ok = false; return 0; }
    size_t off = aend + def->off;
    if (off + 4 > content.size()) { if (ok) *ok = false; return 0; }
    if (ok) *ok = true;
    uint32_t v = GamFile::DwAt(content, off);
    return (int32_t)v;  // -1 = 空
}

bool GamItem::SetField(const char* name, int64_t v) {
    const FieldDef* def = nullptr;
    for (const auto& f : kFields) if (strcmp(f.name, name) == 0) { def = &f; break; }
    if (!def) return false;
    size_t off = aend + def->off;
    if (off + 4 > content.size()) return false;
    uint32_t enc = RawEncDw((uint32_t)(v & 0xFFFFFFFFu));
    content[off] = (uint8_t)(enc & 0xFF);
    content[off + 1] = (uint8_t)((enc >> 8) & 0xFF);
    content[off + 2] = (uint8_t)((enc >> 16) & 0xFF);
    content[off + 3] = (uint8_t)((enc >> 24) & 0xFF);
    dirty = true;
    return true;
}

bool GamItem::ParseRecord(const std::vector<uint8_t>& rec, GamItem& out) {
    // 记录字节 = [len][content][cnt=0][echo=len+12], 0x59 编码
    return MakeItem(0, rec, out) && out.pos == 0;
}

bool GamItem::BuildRecord(const GamItem& it, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t L = (uint32_t)it.content.size();
    std::vector<uint8_t> lenb, ccb, cntb, totb;
    RawX59Range((const uint8_t*)&L, 4, 0x59, lenb);
    RawX59Range(it.content.data(), it.content.size(), 0x59, ccb);
    uint32_t zero = 0;
    RawX59Range((const uint8_t*)&zero, 4, 0x59, cntb);
    uint32_t tot = L + 12;
    RawX59Range((const uint8_t*)&tot, 4, 0x59, totb);
    out.insert(out.end(), lenb.begin(), lenb.end());
    out.insert(out.end(), ccb.begin(), ccb.end());
    out.insert(out.end(), cntb.begin(), cntb.end());
    out.insert(out.end(), totb.begin(), totb.end());
    return true;
}

// 内容末尾 4 字节 = 内容长度(词条数组区的收尾字段); 内容变长后需同步, 否则游戏侧解析会错位
static void WriteContentLen(std::vector<uint8_t>& c) {
    if (c.size() < 4) return;
    uint32_t e = (uint32_t)c.size() ^ 0xAAABACADu;
    size_t o = c.size() - 4;
    c[o] = (uint8_t)(e & 0xFF);
    c[o + 1] = (uint8_t)((e >> 8) & 0xFF);
    c[o + 2] = (uint8_t)((e >> 16) & 0xFF);
    c[o + 3] = (uint8_t)((e >> 24) & 0xFF);
}

std::string GamItem::GetGInfo() const {
    // 命中缓存: 直接读该偏移处的值字段(P3)
    if (ginfo_key_off_ != 0 && ginfo_key_off_ + 4 < content.size()) {
        std::string s;
        size_t used = 0;
        if (GamFile::DecStr(content, ginfo_key_off_, s, used) && s == "G_Info") {
            std::string val;
            size_t used2 = 0;
            if (GamFile::DecStr(content, ginfo_key_off_ + used, val, used2)) return val;
        }
        ginfo_key_off_ = 0;   // 缓存失效, 退回全扫
    }
    // 找字段名串 "G_Info", 其后紧跟的字符串即词条文本
    for (size_t off = 0; off + 4 < content.size(); ++off) {
        std::string s;
        size_t used = 0;
        if (GamFile::DecStr(content, off, s, used)) {
            if (s == "G_Info" && off + used + 4 < content.size()) {
                std::string val;
                size_t used2 = 0;
                if (GamFile::DecStr(content, off + used, val, used2)) {
                    ginfo_key_off_ = off;   // 记住偏移, 后续调用 O(1)
                    return val;
                }
            }
        }
    }
    return "";
}

bool GamItem::SetGInfo(const std::string& gbktext) {
    // 找 G_Info 值字段, 在 content 中整体替换(等长或变长均可; 变长由 Save 重排落盘)
    for (size_t off = 0; off + 4 < content.size(); ++off) {
        std::string s;
        size_t used = 0;
        if (GamFile::DecStr(content, off, s, used)) {
            if (s == "G_Info") {
                size_t valoff = off + used;
                std::string oldval;
                size_t oldused = 0;
                if (!GamFile::DecStr(content, valoff, oldval, oldused)) return false;
                if (oldval == gbktext) return true;   // 无变化: 不动数组
                // 编码新文本: [4B len=(strlen+1)^KEY][chars ^(strlen-0x56-i)][\0 ^...]
                size_t newlen = gbktext.size();
                uint32_t slen = (uint32_t)newlen + 1;
                uint32_t enc = slen ^ 0xAAABACADu;
                std::vector<uint8_t> nb;
                nb.reserve(valoff + 4 + newlen + 1 + (content.size() - valoff - oldused));
                nb.insert(nb.end(), content.begin(), content.begin() + valoff);
                nb.push_back((uint8_t)(enc & 0xFF));
                nb.push_back((uint8_t)((enc >> 8) & 0xFF));
                nb.push_back((uint8_t)((enc >> 16) & 0xFF));
                nb.push_back((uint8_t)((enc >> 24) & 0xFF));
                for (size_t i = 0; i < newlen; ++i)
                    nb.push_back((uint8_t)(gbktext[i] ^ (uint8_t)((newlen - 0x56 - i) & 0xFF)));
                nb.push_back((uint8_t)(0 ^ (uint8_t)((newlen - 0x56 - newlen) & 0xFF)));  // 结尾 \0
                nb.insert(nb.end(), content.begin() + valoff + oldused, content.end());
                content = std::move(nb);
                InvalidateGInfoCache();
                if (has_affixes) WriteContentLen(content);  // G_Info 在数组前面, 变长/变短都平移了尾帧
                dirty = true;
                // 词条数组是游戏真正读的结构: G_Info 改了必须同步重建它
                // (no_sync: 补长度时只在文本尾部补空格, 词条行没变, 不必也不能重建 —— 否则
                //  第二遍会拿到已重建的数组, 让"只存上限节点"这类消歧规则失效、凭空长出下限节点)
                if (has_affixes && !no_sync && !SyncAffixesFromGInfo()) return false;
                return true;
            }
        }
    }
    return false;
}

bool GamItem::SetAlias(const std::string& newalias) {
    // 别名字段 = 内容里第一个标准小写串; 其占位 = 4B 长度前缀 + 字符 + 结尾 null; 尾 = aend
    size_t slen = alias.size() + 1;
    if (aend < 4 + slen) return false;
    size_t off = aend - 4 - slen;
    size_t newlen = newalias.size();
    if (newlen < 1 || newlen > 0x40) return false;
    uint32_t slenv = (uint32_t)newlen + 1;
    uint32_t enc = slenv ^ 0xAAABACADu;
    std::vector<uint8_t> nb;
    nb.reserve(content.size() + newlen + 8);
    nb.insert(nb.end(), content.begin(), content.begin() + (ptrdiff_t)off);
    nb.push_back((uint8_t)(enc & 0xFF));
    nb.push_back((uint8_t)((enc >> 8) & 0xFF));
    nb.push_back((uint8_t)((enc >> 16) & 0xFF));
    nb.push_back((uint8_t)((enc >> 24) & 0xFF));
    for (size_t i = 0; i < newlen; ++i)
        nb.push_back((uint8_t)(newalias[i] ^ (uint8_t)((newlen - 0x56 - i) & 0xFF)));
    nb.push_back((uint8_t)(0 ^ (uint8_t)((newlen - 0x56 - newlen) & 0xFF)));  // 结尾 null
    nb.insert(nb.end(), content.begin() + (ptrdiff_t)aend, content.end());
    content = std::move(nb);
    InvalidateGInfoCache();
    alias = newalias;
    aend = off + 4 + slenv;
    if (has_affixes) WriteContentLen(content); // 词条数组随之后移, 收尾长度字段要跟着改
    dirty = true;
    return true;
}

bool GamItem::SetName(const std::string& gbkname) {
    size_t off = name_off;
    if (off == 0) return false;
    std::string oldv;
    size_t oldused = 0;
    if (!GamFile::DecStr(content, off, oldv, oldused)) return false;
    size_t newlen = gbkname.size();
    if (newlen < 1 || newlen > 0x40) return false;
    uint32_t slenv = (uint32_t)newlen + 1;
    uint32_t enc = slenv ^ 0xAAABACADu;
    std::vector<uint8_t> nb;
    nb.reserve(content.size() + newlen + 8);
    nb.insert(nb.end(), content.begin(), content.begin() + (ptrdiff_t)off);
    nb.push_back((uint8_t)(enc & 0xFF));
    nb.push_back((uint8_t)((enc >> 8) & 0xFF));
    nb.push_back((uint8_t)((enc >> 16) & 0xFF));
    nb.push_back((uint8_t)((enc >> 24) & 0xFF));
    for (size_t i = 0; i < newlen; ++i)
        nb.push_back((uint8_t)(gbkname[i] ^ (uint8_t)((newlen - 0x56 - i) & 0xFF)));
    nb.push_back((uint8_t)(0 ^ (uint8_t)((newlen - 0x56 - newlen) & 0xFF)));
    nb.insert(nb.end(), content.begin() + (ptrdiff_t)(off + oldused), content.end());
    long long delta = (long long)(4 + newlen + 1) - (long long)oldused;
    content = std::move(nb);
    InvalidateGInfoCache();
    name = gbkname;
    aend = (size_t)((long long)aend + delta);  // 名在别名之前, 长度变化会平移 aend
    if (has_affixes) WriteContentLen(content); // 词条数组随之后移, 收尾长度字段要跟着改
    dirty = true;
    return true;
}

// ------------------------------------------------------------------
// 词条数组(记录末尾): 游戏真正读的结构, G_Info 只是它的文本渲染
// ------------------------------------------------------------------

static float BitsToFloat(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
static uint32_t FloatToBits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

void GamItem::ParseAffixes() {
    affixes.clear();
    has_affixes = false;
    affix_tail = 0;
    const std::vector<uint8_t>& c = content;
    if (c.size() < 64) return;
    // 末尾 4 字节 = 内容长度
    if (GamFile::DwAt(c, c.size() - 4) != (uint32_t)c.size()) return;
    // 词条数组在内容末尾: 先从尾部按 44 步长倒着验证(P4: O(节点数), 原来整段逐字节全扫);
    // 倒着不成立时退回原来的前向全扫(兼容异常布局)。
    size_t base = 0, n = 0;
    if (c.size() >= 48 && GamFile::DwAt(c, c.size() - 48) == 0x2Cu &&
        GamFile::DwAt(c, c.size() - 48 + 4) == 0xFFFFFFFFu) {
        size_t b = c.size() - 48;
        size_t k = 1;
        while (b >= 44 && GamFile::DwAt(c, b - 44) == 0x2Cu &&
               GamFile::DwAt(c, b - 44 + 4) == 0xFFFFFFFFu) {
            b -= 44;
            ++k;
        }
        if (b >= 4 && GamFile::DwAt(c, b - 4) == (uint32_t)k) {
            base = b;
            n = k;
        }
    }
    if (n == 0) {
        // 回退: 收集所有 [0x2C][0xFFFFFFFF] 标记, 取最长 44 步长链且末节点收在长度字段之前
        std::vector<size_t> marks;
        for (size_t o = 0; o + 8 <= c.size(); ++o)
            if (GamFile::DwAt(c, o) == 0x2Cu && GamFile::DwAt(c, o + 4) == 0xFFFFFFFFu)
                marks.push_back(o);
        for (size_t i = 0; i < marks.size(); ++i) {
            size_t k = 1;
            while (i + k < marks.size() && marks[i + k] == marks[i] + 44 * k) ++k;
            if (marks[i] + 44 * k == c.size() - 4 && k > n) {
                base = marks[i];
                n = k;
            }
        }
    }
    if (n == 0 || base < 4) {
        // 没找到节点链: 再看是不是"0 节点数组"(词条被全部删光, 只剩 [count=0][长度])。
        // 必须放在这里(确认无链之后) —— 正常物品的末节点负载尾部本身也是 0, 提前判会全部误判。
        if (c.size() >= 8 && GamFile::DwAt(c, c.size() - 8) == 0u) {
            affix_tail = 8;
            has_affixes = true;
        }
        return;
    }
    // 首节点前一个 dword = 词条数, 与链长自洽才认
    if (GamFile::DwAt(c, base - 4) != (uint32_t)n) return;
    for (size_t k = 0; k < n; ++k) {
        size_t p = base + 8 + 44 * k;
        AffixNode a;
        a.id = (int32_t)GamFile::DwAt(c, p);
        a.active = GamFile::DwAt(c, p + 4);
        a.color = GamFile::DwAt(c, p + 8);
        a.a = BitsToFloat(GamFile::DwAt(c, p + 0x10));
        a.value = BitsToFloat(GamFile::DwAt(c, p + 0x14));
        a.c = BitsToFloat(GamFile::DwAt(c, p + 0x1C));
        affixes.push_back(a);
    }
    affix_tail = 4 + 44 * n + 4;
    has_affixes = true;
}

bool GamItem::SyncAffixesFromGInfo() {
    if (!has_affixes) return false;
    std::vector<GInfoRow> rows;
    if (!ParseGInfoRows(rows)) return false;

    // 上下限成对属性: 下限 id -> 上限 id(G_Info 无 ~ 时两者同值; 数组里必须成对存在)
    const std::map<int, int>& kPair = ItemData::AffixPairs();

    // +0x10 = "比例"标志。语料实测分两类:
    //   非天生百分比的属性(命中率/闪避率/普通防御/各系攻击力防御/生命/精气…) -> A=1.0 时文本才带 %
    //   天生百分比的属性(下表: 攻击速度/吸命/致命一击/眩晕几率/抗性/时间类…) -> 文本恒带 %, 与 A 无关,
    //       且原数组里 A 绝大多数是 0(偶有 1) —— 文本表达不出来, 所以按原节点保留 A
    static const int kAlwaysPct[] = {0x40, 0x41, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D,
                                     0x50, 0x52, 0x54, 0x56, 0x58, 0x5A, 0x5C, 0x5D, 0x5E, 0x5F,
                                     0x60, 0x62};
    auto defC = [](int id) -> float { return id == 0x41 ? -1.0f : 0.0f; };
    auto colorOf = [](char ch) -> uint32_t {
        switch (ch) {
            case 'K': return 1;
            case 'C': return 2;
            case 'H': return 2;
            case 'Z': return 3;
            default: return 0;
        }
    };
    auto activeOf = [](char ch) -> uint32_t { return ch == 'H' ? 0u : 1u; };
    // 原节点的"占用"标记: 同一 id 在数组里可能有多个(如攻击速度出现两次), 必须按出现顺序
    // 一一对应, 否则会把一个节点的系数搬到另一个上。
    std::vector<bool> origUsed(affixes.size(), false);
    auto mk = [&](int id, uint32_t col, uint32_t act, float v, bool pct) {
        AffixNode nd;
        nd.id = id;
        nd.active = act;
        nd.color = col;
        nd.value = v;
        nd.c = defC(id);
        bool always = false;
        for (int x : kAlwaysPct)
            if (x == id) always = true;
        bool found = false;
        // 优先取"同 id 且同颜色/同激活"的原节点, 再退回同 id 的第一个 —— 同 id 多节点时
        // 这样才不会把 C 系数搬到另一个节点上(实测: 攻击速度出现两次时 C 会串位)
        for (int pass = 0; pass < 2 && !found; ++pass) {
            for (size_t oi = 0; oi < affixes.size(); ++oi) {
                if (origUsed[oi] || affixes[oi].id != id) continue;
                if (pass == 0 && (affixes[oi].color != col || affixes[oi].active != act)) continue;
                origUsed[oi] = true;
                nd.c = affixes[oi].c;
                if (always) nd.a = affixes[oi].a;
                found = true;
                break;
            }
        }
        if (!always) nd.a = pct ? 1.0f : 0.0f;
        else if (!found) nd.a = 0.0f;
        return nd;
    };

    std::vector<AffixNode> list;
    static const int kAtkElem[] = {0x2B, 0x2D, 0x2F, 0x31, 0x33};
    static const int kDefElem[] = {0x36, 0x37, 0x38, 0x39, 0x3A};
    auto idAt = [&](size_t i) { return ItemData::AttrIdFromGInfoName(rows[i].name); };
    bool hasAtkElem = false, hasDefElem = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        int id = idAt(i);
        for (int e : kAtkElem)
            if (id == e) hasAtkElem = true;
        for (int e : kDefElem)
            if (id == e) hasDefElem = true;
    }
    auto origHas = [&](int id) {
        for (const auto& o : affixes)
            if (o.id == id) return true;
        return false;
    };

    // 五行攻击力的"聚合形态"(游戏实测 tmp/_u01o.gam): 5 系下限相同 -> G_Info 里只有一行
    // "五行攻击力 +V", 紧跟 5 行 "X系攻击力 +W"(没有"最大"字样, W 其实是各系的**上限**)。
    // 例: 数组 2B=1,2C=2,2D=1,2E=2… -> 文本 "五行攻击力 +1 / 金系攻击力 +2 / 木系攻击力 +2…"
    // 所以要还原成 min=V、max=各行的值, 否则照字面解析会把下限 1 写成 2。
    std::vector<bool> used(rows.size(), false);
    // 各系下限"隐形"判定: 5 个 X系攻击力行都在、都只有单值(无"最大X系攻击力"行)、且没有
    // "五行攻击力"聚合行 —— 说明聚合行被游戏丢弃了(任一系下限是浅蓝色时就会, 实测 _u01r.gam)。
    // 这时 5 系下限全都不在文本里, 必须按原数组保留, 否则下限会被写成上限值。
    bool aggDropped = false;
    if (hasAtkElem) {
        bool allElem = true, anyMax = false, anyAg = false;
        for (size_t i = 0; i < rows.size(); ++i) {
            int d = idAt(i);
            if (d == 0x01 || d == 0x02) anyAg = true;
            for (int k = 0; k < 5; ++k)
                if (d == kAtkElem[k] + 1) anyMax = true;
        }
        for (int k = 0; k < 5; ++k) {
            bool found = false;
            for (size_t i = 0; i < rows.size(); ++i)
                if (idAt(i) == kAtkElem[k]) found = true;
            if (!found) allElem = false;
        }
        aggDropped = allElem && !anyMax && !anyAg;
    }
    if (hasAtkElem) {
        int agIdx = -1, elemIdx[5] = {-1, -1, -1, -1, -1};
        bool allElem = true, anyMax = false;
        for (size_t i = 0; i < rows.size(); ++i) {
            int id = idAt(i);
            if (id == 0x01 && agIdx < 0) agIdx = (int)i;
            for (int k = 0; k < 5; ++k) {
                if (id == kAtkElem[k] && elemIdx[k] < 0) elemIdx[k] = (int)i;
                if (id == kAtkElem[k] + 1) anyMax = true;
            }
        }
        for (int k = 0; k < 5; ++k)
            if (elemIdx[k] < 0) allElem = false;
        if (agIdx >= 0 && allElem && !anyMax) {
            used[(size_t)agIdx] = true;
            // 下限节点的颜色来自"五行攻击力"那一行, 但游戏是取某一系下限的颜色作代表, 5 系各自
            // 的颜色文本里看不出来 —— 所以:
            //   聚合行颜色没变 -> 逐节点保留原颜色(只有某一系被染色的情形, 如实测的 2B=金)
            //   聚合行颜色变了 -> 视为用户要整体改色, 5 个下限都换成新色
            uint32_t agCol = colorOf(rows[(size_t)agIdx].color);
            uint32_t agAct = activeOf(rows[(size_t)agIdx].color);
            float agMin = (float)rows[(size_t)agIdx].vmin;
            bool found0 = false;
            uint32_t oldAggCol = agCol, oldAggAct = agAct;
            for (const auto& o : affixes)
                if (o.id == kAtkElem[0]) {
                    oldAggCol = o.color;
                    oldAggAct = o.active;
                    found0 = true;
                    break;
                }
            bool recolor = found0 && (agCol != oldAggCol || agAct != oldAggAct);
            for (int k = 0; k < 5; ++k) {
                size_t ri = (size_t)elemIdx[k];
                used[ri] = true;
                uint32_t mc = agCol, ma = agAct;
                if (!recolor)
                    for (const auto& o : affixes)
                        if (o.id == kAtkElem[k]) {
                            mc = o.color;
                            ma = o.active;
                            break;
                        }
                list.push_back(mk(kAtkElem[k], mc, ma, agMin, rows[(size_t)agIdx].is_percent));
                list.push_back(mk(kAtkElem[k] + 1, colorOf(rows[ri].color), activeOf(rows[ri].color),
                                  (float)rows[ri].vmin, rows[ri].is_percent));
            }
        }
    }

    // 五行攻击力/五行防御力: 游戏既能用 0x01/0x03 单个节点表示, 也能用 5 系节点等值表示,
    // 渲染出的文本可能一样, 所以按"原数组怎么编的"来保持:
    //   ① G_Info 里已有 5 系各行 -> 跳过聚合行(那行是它们算出来的显示)
    //   ② 原数组用的是 5 系节点 -> 继续写 5 系节点(新值)
    //   ③ 否则才写聚合 id(用户新加该词条的情形)
    for (size_t i = 0; i < rows.size(); ++i) {
        if (used[i]) continue;
        const GInfoRow& r = rows[i];
        int id = idAt(i);
        if (id < 0) continue;
        uint32_t col = colorOf(r.color), act = activeOf(r.color);
        float v = (float)r.vmin;
        if (id == 0x01 || id == 0x03) {
            const int* elems = (id == 0x01) ? kAtkElem : kDefElem;
            bool groupRowsPresent = (id == 0x01) ? hasAtkElem : hasDefElem;
            if (groupRowsPresent) continue;   // ①
            bool origElem = false;
            for (int k = 0; k < 5; ++k)
                if (origHas(elems[k])) origElem = true;
            if (origElem) {                   // ②
                // 5 系节点在原数组里, 继续按 5 系写: 下限/单值 = 聚合行的值;
                // 上限(只有攻击系有)文本里表达不出来 -> 保留原上限值。
                // 颜色同攻击侧: 聚合行颜色没变就逐节点保留原色, 变了才整组换色。
                bool found0 = false;
                uint32_t oldCol = col, oldAct = act;
                for (const auto& o : affixes)
                    if (o.id == elems[0]) {
                        oldCol = o.color;
                        oldAct = o.active;
                        found0 = true;
                        break;
                    }
                bool recolor = found0 && (col != oldCol || act != oldAct);
                for (int k = 0; k < 5; ++k) {
                    uint32_t mc = col, ma = act;
                    if (!recolor)
                        for (const auto& o : affixes)
                            if (o.id == elems[k]) {
                                mc = o.color;
                                ma = o.active;
                                break;
                            }
                    list.push_back(mk(elems[k], mc, ma, v, r.is_percent));
                    if (id == 0x01) {
                        // 上限值/比例在文本里表达不出来 -> 按原上限节点保留
                        float mv = v;
                        bool mpct = r.is_percent;
                        for (const auto& o : affixes)
                            if (o.id == elems[k] + 1) {
                                mv = o.value;
                                mpct = (o.a != 0.0f);
                                break;
                            }
                        list.push_back(mk(elems[k] + 1, col, act, mv, mpct));
                    }
                }
                continue;
            }
            // ③ 落到下面的普通规则(0x01/0x02 是成对项, 0x03 是单项)
            if (id == 0x03) {
                list.push_back(mk(0x03, col, act, v, r.is_percent));
                continue;
            }
        }
        auto pit = kPair.find(id);
        if (pit != kPair.end()) {
            // ParseGInfoRows 会把 "X +下 ~ +上" 拆成两行, 所以"有最大行" == "文本给了上下两个值"
            bool hasMaxRow = (i + 1 < rows.size() && idAt(i + 1) == pit->second);
            float mv = hasMaxRow ? (float)rows[i + 1].vmin : v;
            if (hasMaxRow) used[i + 1] = true;
            // 各系攻击力下限的"隐形下限": 文本只给一个数字时它是【上限】; 若原数组该下限是
            // 浅蓝色(color 2), 游戏会把"五行攻击力"聚合行整个丢掉(实测 tmp/_u01r.gam), 下限
            // 在文本里彻底消失 —— 必须保留原下限的值/颜色, 否则会把下限写成上限值。
            bool isAtkMin = false;
            for (int e : kAtkElem)
                if (id == e) isAtkMin = true;
            // "X系攻击力 +v" 这一行的 v 是下限还是上限? 文本看不出来, 用原数组判定:
            // 原数组没有下限节点、只有上限节点 -> 这行就是上限(实测 tmp/_u01u.gam: 下限被换成了
            // 五行防御力, 只剩 2C/2E/30/32/34, 游戏照样印 "金系攻击力 +2")。
            if (!hasMaxRow && isAtkMin) {
                bool oMin = false, oMax = false;
                for (const auto& o : affixes) {
                    if (o.id == id) oMin = true;
                    if (o.id == id + 1) oMax = true;
                }
                if (!oMin && oMax) {
                    list.push_back(mk(pit->second, col, act, v, r.is_percent));
                    continue;
                }
            }
            // 各系攻击力下限: 文本只给一个数字时它是【上限】, 下限若因为聚合行被丢弃而
            // 在文本里消失(见上 aggDropped), 就整体按原数组保留下限节点。
            bool keepOrig = false;
            AffixNode origMin;
            if (!hasMaxRow && aggDropped && isAtkMin)
                for (const auto& o : affixes)
                    if (o.id == id) {
                        origMin = o;
                        keepOrig = true;
                        break;
                    }
            list.push_back(keepOrig ? origMin : mk(id, col, act, v, r.is_percent));
            list.push_back(mk(pit->second, col, act, mv,
                              hasMaxRow ? rows[i + 1].is_percent : r.is_percent));
            continue;
        }
        list.push_back(mk(id, col, act, v, r.is_percent));
    }
    // 值为 0 的原节点 G_Info 不渲染(游戏也不显示), 但它确实是数组内容: 原样保留,
    // 免得只改了个名字/等级就把这类占位节点丢掉。
    for (const auto& old : affixes) {
        if (old.value != 0.0f) continue;
        bool used = false;
        for (const auto& nd : list)
            if (nd.id == old.id) {
                used = true;
                break;
            }
        if (!used) list.push_back(old);
    }

    return SetAffixes(list);
}

bool GamItem::PadContentTo(size_t want_len) {
    if (!has_affixes) return false;
    for (int iter = 0; iter < 4 && content.size() < want_len; ++iter) {
        std::string gi = GetGInfo();
        if (gi.empty()) return false;
        size_t need = want_len - content.size();
        gi.append(need, ' ');          // G_Info 尾部补空格(解析时逐行去尾空格, 不影响词条)
        size_t before = content.size();
        no_sync = true;                // 只动文本尾部 -> 不重建词条数组
        bool okpad = SetGInfo(gi);
        no_sync = false;
        if (!okpad) return false;
        if (content.size() <= before) return false;   // 没长说明补不进去
    }
    return content.size() >= want_len;
}

bool GamItem::SetAffixes(const std::vector<AffixNode>& list) {
    if (content.size() < affix_tail) return false;
    // 组装: [前缀][count][节点...][内容长度]
    size_t keep = content.size() - affix_tail;
    std::vector<uint8_t> out(content.begin(), content.begin() + (ptrdiff_t)keep);
    auto putDw = [&](uint32_t v) {
        uint32_t e = v ^ 0xAAABACADu;
        out.push_back((uint8_t)(e & 0xFF));
        out.push_back((uint8_t)((e >> 8) & 0xFF));
        out.push_back((uint8_t)((e >> 16) & 0xFF));
        out.push_back((uint8_t)((e >> 24) & 0xFF));
    };
    putDw((uint32_t)list.size());
    for (const auto& nd : list) {
        putDw(0x2Cu);
        putDw(0xFFFFFFFFu);
        putDw((uint32_t)nd.id);
        putDw(nd.active);
        putDw(nd.color);
        putDw(0);
        putDw(FloatToBits(nd.a));
        putDw(FloatToBits(nd.value));
        putDw(0);
        putDw(FloatToBits(nd.c));
        putDw(0);
    }
    putDw((uint32_t)out.size() + 4);   // 内容长度(含本 dword)
    content = std::move(out);
    InvalidateGInfoCache();
    affixes = list;
    affix_tail = 4 + 44 * list.size() + 4;
    has_affixes = true;
    dirty = true;
    return true;
}

bool GamItem::ParseGInfoRows(std::vector<GInfoRow>& rows) const {
    std::string txt = GetGInfo();
    rows.clear();
    if (txt.empty()) return false;
    char cur_color = 0;
    // 逐行(记录字节偏移, 供值重写)
    size_t pos = 0;
    while (pos <= txt.size()) {
        size_t nl = txt.find('\n', pos);
        std::string line = (nl == std::string::npos) ? txt.substr(pos) : txt.substr(pos, nl - pos);
        bool group_ends = false;
        std::string inner = line;
        size_t inner_start = pos;  // inner 相对 txt 的字节偏移(该行开头)
        // [K / [H / [Z ... (可跨行: 组首行只写 "["+字母, ']' 落在组末行行尾)
        size_t n = inner.size();
        size_t k = 0;
        if (n >= 2 && inner[0] == '[') {
            char c = inner[1];
            if (c == 'K' || c == 'H' || c == 'Z' || c == 'C') {
                cur_color = c;
                k = 2;
            }
        }
        // 组结束: 行尾的 ']'(单行组与多行组的末行都是这个写法)
        {
            size_t e = n;
            while (e > k && (inner[e - 1] == ' ' || inner[e - 1] == '\t')) --e;
            if (e > k && inner[e - 1] == ']') {
                group_ends = true;
                n = e - 1;
            }
        }
        // 头部行: &... 或 Lv.
        {
            std::string s = inner.substr(k, n - k);
            size_t t0 = s.find_first_not_of(" \t");
            if (t0 != std::string::npos) {
                bool is_header = (s[t0] == '&');
                if (!is_header) {
                    // 取名字 token(到空白/GBK对)
                    size_t j = t0;
                    while (j < n - k && (uint8_t)inner[k + j] > 0x20) {
                        if ((uint8_t)inner[k + j] >= 0x81 && k + j + 1 < n) j += 2;
                        else j += 1;
                    }
                    std::string name = inner.substr(k + t0, j - t0);
                    is_header = (name == "Lv" || name == "Lv.");
                }
                if (!is_header) {
                    // 属性名 ... 值
                    std::string name;
                    long vmin = 0, vmax = 0;
                    // 名字到第一个空白
                    size_t j = t0;
                    while (j < n - k && (uint8_t)inner[k + j] > 0x20) {
                        if ((uint8_t)inner[k + j] >= 0x81 && k + j + 1 < n) j += 2;
                        else j += 1;
                    }
                    name = inner.substr(k + t0, j - t0);
                    // 跳过空白
                    size_t vp = j;
                    while (vp < n - k && (inner[k + vp] == ' ' || inner[k + vp] == '\t')) ++vp;
                    // 值: [+-]digits [~ [+-]digits]
                    size_t v1 = vp;
                    bool neg = (v1 < n - k && (inner[k + v1] == '+' || inner[k + v1] == '-'));
                    if (neg) ++v1;
                    size_t d = v1;
                    while (d < n - k && inner[k + d] >= '0' && inner[k + d] <= '9') ++d;
                    if (d > v1) {
                        vmin = atol(inner.substr(k + v1, d - v1).c_str());
                        if (inner[k + v1 - 1] == '-') vmin = -vmin;
                        // 百分比标记 \x0f
                        bool is_percent = false;
                        size_t t = d;
                        while (t < n - k && (uint8_t)inner[k + t] == 0x0F) { is_percent = true; ++t; }
                        size_t t2 = t;
                        while (t2 < n - k && (inner[k + t2] == ' ' || inner[k + t2] == '\t')) ++t2;
                        bool has_max = false;
                        if (t2 < n - k && inner[k + t2] == '~') {
                            size_t t3 = t2 + 1;
                            while (t3 < n - k && (inner[k + t3] == ' ' || inner[k + t3] == '\t')) ++t3;
                            bool neg2 = (t3 < n - k && (inner[k + t3] == '+' || inner[k + t3] == '-'));
                            if (neg2) ++t3;
                            size_t d2 = t3;
                            while (d2 < n - k && inner[k + d2] >= '0' && inner[k + d2] <= '9') ++d2;
                            if (d2 > t3) {
                                vmax = atol(inner.substr(k + t3, d2 - t3).c_str());
                                if (neg2 && t3 > 0 && inner[k + t3 - 1] == '-') vmax = -vmax;
                                has_max = true;
                                // 范围上限后也可能有 \x0f
                                size_t tt = d2;
                                while (tt < n - k && (uint8_t)inner[k + tt] == 0x0F) { is_percent = true; ++tt; }
                            }
                        }
                        GInfoRow r{};
                        r.color = cur_color;
                        r.name = name;
                        r.vmin = vmin;
                        r.vmax = 0;
                        r.has_max = false;
                        r.is_percent = is_percent;
                        rows.push_back(std::move(r));
                        if (has_max) {
                            // 上限是独立词条: 拆成 "最大<名>" 一行(UI 同道具修改; 保存时再合成 "名 +v ~ +w")
                            GInfoRow r2{};
                            r2.color = cur_color;
                            r2.name = std::string("\xD7\xEE\xB4\xF3") + name;  // GBK "最大"
                            r2.vmin = vmax;
                            r2.is_percent = is_percent;
                            rows.push_back(std::move(r2));
                        }
                    }
                }
            }
        }
        if (group_ends) cur_color = 0;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    // 文本非空即成功: 0 行(词条删光)也是合法结果 —— 不能用 rows.empty() 当失败,
    // 否则 SyncAffixesFromGInfo 会放弃重建, "删光词条"就写不进数组(P0)。
    return !txt.empty();
}

bool GamItem::SetGInfoRowValue(size_t idx, long vmin, long vmax) {
    // 值改写受 等长原位 约束(变长需重排)。交原文本编辑面板处理; 此处不自动改写。
    (void)idx; (void)vmin; (void)vmax;
    return false;
}

std::string GamItem::GetGInfoHeader() const {
    std::string txt = GetGInfo();
    size_t nl = txt.find('\n');
    std::string first = (nl == std::string::npos) ? txt : txt.substr(0, nl);
    size_t t0 = first.find_first_not_of(" \t");
    if (t0 == std::string::npos) return "";
    // 头行: &... 或 Lv.
    if (first[t0] == '&') {
        return (nl == std::string::npos) ? first : first + "\n";
    }
    // 名字 token 是否为 Lv / Lv.
    size_t j = t0;
    while (j < first.size() && (uint8_t)first[j] > 0x20) ++j;
    std::string name = first.substr(t0, j - t0);
    if (name == "Lv" || name == "Lv.") {
        return (nl == std::string::npos) ? first : first + "\n";
    }
    return "";
}

bool GamItem::BuildGInfoText(const std::string& header, const std::vector<GInfoRow>& rows,
                             std::string& out) {
    out.clear();
    out += header;
    // 合并相邻的 (X, 最大X) -> X +v ~ +w
    // 只对真正的上下限成对属性合并(如 普通攻击力/最大普通攻击力、有效距离/最大有效距离);
    // 最大生命/最大精气/最大负重 是独立词条, 绝不能并进前一行。
    std::vector<GInfoRow> M;
    for (size_t i = 0; i < rows.size(); ++i) {
        const GInfoRow& r = rows[i];
        if (r.name.size() >= 4 && r.name.compare(0, 4, "\xD7\xEE\xB4\xF3") == 0 && !M.empty() &&
            M.back().color == r.color && !M.back().has_max && M.back().name == r.name.substr(4) &&
            ItemData::IsAffixPairMinId(ItemData::AttrIdFromGInfoName(M.back().name))) {
            M.back().has_max = true;
            M.back().vmax = r.vmin;
            continue;
        }
        M.push_back(r);
        M.back().has_max = false;
    }
    auto sign = [](long v) { return v < 0 ? "-" : "+"; };
    // fmt: "名 +值[ ~ +上限]"(不带尾随)
    auto fmt = [&](const GInfoRow& r) {
        std::string s = r.name;
        char buf[32];
        std::snprintf(buf, sizeof(buf), " %s%ld", sign(r.vmin), r.vmin < 0 ? -r.vmin : r.vmin);
        s += buf;
        if (r.has_max) {
            std::snprintf(buf, sizeof(buf), " ~ %s%ld", sign(r.vmax), r.vmax < 0 ? -r.vmax : r.vmax);
            s += buf;
        }
        return s;
    };
    // 尾部标记: 百分比->\x0f; 有上限(~)->无; 无上限->空格 (与游戏原文一致)
    auto tail = [](const GInfoRow& r) {
        if (r.is_percent) return std::string(1, (char)0x0F);
        if (r.has_max) return std::string();
        return std::string(" ");
    };
    auto emit = [&](const GInfoRow& r) { return fmt(r) + tail(r); };
    for (size_t i = 0; i < M.size(); ++i) {
        const GInfoRow& r = M[i];
        if (r.color == 0) {
            out += emit(r);
            out += "\n";
        } else {
            size_t j = i + 1;
            while (j < M.size() && M[j].color == r.color) ++j;
            out += "[";
            out += r.color;
            if (j == i + 1) {
                out += emit(r);
            } else {
                for (size_t k = i; k < j; ++k) {
                    if (k > i) out += "\n";
                    out += emit(M[k]);
                }
            }
            out += "]\n";
            i = j - 1;
        }
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();  // 原文各行以 \n 分隔,末尾无换行
    return true;
}

// ---------------------------------------------------------------------------
// 加载 / 遍历
// ---------------------------------------------------------------------------
namespace {
struct WalkCtx {
    GamFile& f;
    GamContainer* cur = nullptr;
    WalkCtx(GamFile& f_) : f(f_) {}
};

size_t walkSubtree(const std::vector<uint8_t>& buf, GamFile& f, GamContainer& cur,
                   size_t p, int depth, int depth_limit, size_t active_cnt_off) {
    if (p + 12 > buf.size()) return 1;
    uint32_t L = GamFile::X59Dw(buf, p);
    if (L < 0x100 || L > 0x6000 || p + 4 + L + 8 > buf.size()) return 1;
    uint32_t cnt = GamFile::X59Dw(buf, p + 4 + L);
    uint32_t total = GamFile::X59Dw(buf, p + 4 + L + 4);
    std::vector<uint8_t> cc;
    X59Range(buf, p + 4, L, 0x59, cc);
    std::string a;
    size_t aend = 0;
    if (cnt == 0 && total == L + 12) {
        GamItem it;
        if (!MakeItem(p, buf, it)) return 4 + L + 8;
        it.cnt_off = active_cnt_off;  // 所属组的 count 偏移(删除时递减)
        int idx = (int)f.items.size();
        f.items.push_back(std::move(it));
        cur.item_idx.push_back(idx);
        cur.insert_pos = p + 4 + L + 8;
        cur.insert_cnt_off = active_cnt_off;
        // 按组统计(最终锚点取物品最多的组; 避免锚到 blob/紧凑项)
        auto& gs = cur.gstat[active_cnt_off];
        gs.first += 1;
        gs.second = p + 4 + L + 8;
        return 4 + L + 8;
    }
    if (find_alias(cc, a, aend) && cnt >= 1 && cnt <= 600 && depth < depth_limit) {
        size_t end = p + 4 + L + 4;
        unsigned guard = 0;
        uint32_t consumed = 0;
        // 递归子节点: 直接步进, 失配则向前容差扫描(紧凑记录有尾部差异)
        while (consumed < cnt && guard < 800000u) {
            ++guard;
            size_t base = end;
            size_t step = walkSubtree(buf, f, cur, end, depth + 1, depth_limit, p + 4 + L);
            if (step > 1) {
                end += step;
                ++consumed;
                continue;
            }
            // 向前扫描直到找到可解析节点(记为一个子节点); 窗口对齐 python(0x300)
            size_t q = base;
            size_t steps = 0;
            bool found = false;
            while (steps < 0x300u) {
                ++steps;
                ++q;
                if (q + 16 > buf.size()) break;
                size_t s2 = walkSubtree(buf, f, cur, q, depth + 1, depth_limit, p + 4 + L);
                if (s2 > 1) {
                    end = q + s2;
                    found = true;
                    break;
                }
            }
            ++consumed;
            if (!found) { end = base + 1; }
        }
        return end - p;
    }
    if (!a.empty()) return 4 + L + 8;  // obj 紧凑对象
    return 1;                 // 未知数据, 逐字节前移(对齐 python)
}
}  // namespace

namespace {
/// 内存档解析(Load 与 Save 重排共用)。定义见下(Load 之前的声明)。
bool ParseLoaded(GamFile& f, const std::string& path_, std::string& err);
}  // namespace

bool GamFile::Load(const std::string& path_, GamFile& f, std::string& err) {
    f = GamFile();
    FILE* fp = FOpenW(path_, L"rb");
    if (!fp) { err = "无法打开文件: " + path_; return false; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0x100 || sz > 0x40000000L) { fclose(fp); err = "文件尺寸异常"; return false; }
    f.buf.resize((size_t)sz);
    if (fread(f.buf.data(), 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); err = "文件读取失败"; return false; }
    fclose(fp);

    return ParseLoaded(f, path_, err);
}

namespace {
/// 内存档解析(Load 读取字节后共用; 保存重排后也用它对临时缓冲重读取锚点)。
bool ParseLoaded(GamFile& f, const std::string& path_, std::string& err) {
    if (f.buf.size() < 0x4C + 4 || memcmp(f.buf.data(), "EASYRPG", 7) != 0) {
        err = "不是有效的 GAM 存档(缺少 EASYRPG 头)";
        return false;
    }
    f.path = path_;
    f.rec = GamFile::X59Dw(f.buf, 0x4C);
    f.dataoff = GamFile::X59Dw(f.buf, 0x50, 0x5D);
    // 40B 头: 0x54 起连续 0x59+i
    if (0x54 + 40 <= f.buf.size()) {
        std::vector<uint8_t> h40;
        X59Range(f.buf, 0x54, 40, 0x59, h40);
        f.cont_len = (uint32_t)D32LE(&h40[8]);
    }
    if (f.dataoff < 0x4C || f.dataoff + 0x100 > f.buf.size()) {
        err = "数据区偏移异常"; return false;
    }

    // ---- 遍历 body: 容器=顶层组节点, 走进子树收割物品(场景级分组) ----
    size_t pos = f.dataoff;
    GamContainer first;
    first.start = f.dataoff;
    first.alias = "@当前地图";
    first.first = true;
    f.containers.push_back(first);
    GamContainer* cur = &f.containers.back();
    unsigned guard = 0;
    while (pos + 16 < f.buf.size() && guard < 3000000u) {
        ++guard;
        uint32_t L = GamFile::X59Dw(f.buf, pos);
        if (L >= 0x100 && L <= 0x6000 && pos + 4 + L + 8 <= f.buf.size()) {
            uint32_t cnt = GamFile::X59Dw(f.buf, pos + 4 + L);
            uint32_t total = GamFile::X59Dw(f.buf, pos + 4 + L + 4);
            std::vector<uint8_t> cc;
            X59Range(f.buf, pos + 4, L, 0x59, cc);
            std::string a;
            size_t aend = 0;
            if (find_alias(cc, a, aend) && cnt >= 1 && cnt <= 600 && !(cnt == 0 && total == L + 12)) {
                // 顶层容器根: 走进整棵树, 物品归到本容器
                f.containers.back().end = pos;
                GamContainer nc;
                nc.start = pos;
                nc.alias = a;
                nc.insert_cnt_off = pos + 4 + L;
                f.containers.push_back(nc);
                cur = &f.containers.back();
                pos += walkSubtree(f.buf, f, *cur, pos, 0, 8, cur->insert_cnt_off);
                continue;
            }
        }
        // 平铺节点 -> 当前容器
        pos += walkSubtree(f.buf, f, *cur, pos, 0, 8, cur->insert_cnt_off);
    }
    f.containers.back().end = f.buf.size();
    size_t n = f.buf.size();
    for (auto& c : f.containers) {
        // 插入锚点 = 物品数最多的组(真正的物品组; 排除 blob/紧凑项误锚)
        if (!c.gstat.empty()) {
            size_t bo = 0;
            int bc = -1;
            size_t be = 0;
            for (auto& kv : c.gstat)
                if (kv.second.first > bc) { bc = kv.second.first; bo = kv.first; be = kv.second.second; }
            if (bc > 0 && bo) { c.insert_cnt_off = bo; c.insert_pos = be; }
        }
        if (c.end == n) c.last = true;
        if (c.start == f.dataoff) c.first = true;
    }
    return true;
}
}  // namespace

// ---------------------------------------------------------------------------
// 保存
// ---------------------------------------------------------------------------
static void RawX59Range(const uint8_t* src, size_t n, uint8_t k0, std::vector<uint8_t>& out) {
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = src[i] ^ (uint8_t)(k0 + (uint8_t)i);
}

bool GamFile::Save(GamFile& f, const std::string& out_path,
                   const std::vector<std::pair<int, std::vector<uint8_t>>>& added,
                   std::string& err) {
    // 内容一旦比原来短, 游戏按固定偏移读的字段就会错位(实测: 地上物品消失)。
    // 这里先把所有"变短的" dirty 物品补回原长度(在 G_Info 尾部补空格)。
    for (auto& it : f.items)
        if (it.dirty && !it.is_new && !it.removed && it.content.size() < it.len)
            it.PadContentTo(it.len);

    // 先判断是否有长度变化的 dirty 物品(词条改长/改短/加词条)或被删物品
    bool need_relayout = false;
    for (const auto& it : f.items)
        if (it.removed || (it.dirty && !it.is_new && it.content.size() != it.len)) {
            need_relayout = true;
            break;
        }

    std::vector<uint8_t> b = f.buf;
    if (need_relayout) {
        // 支持单/多容器: 全文件按 dirty/删除物品重排(字节整体平移), 再按"受影响容器"更新长度回显。
        // 容器尾 echo = 容器长度(干净容器); 含紧凑NPC的容器尾不是纯 echo, 跳过(游戏按遍历加载不受影响)。
        // 按 pos 升序: dirty 物品重编, removed 物品整帧丢弃
        std::vector<int> idx;
        for (size_t i = 0; i < f.items.size(); ++i)
            if ((f.items[i].dirty && !f.items[i].is_new) || f.items[i].removed) idx.push_back((int)i);
        std::sort(idx.begin(), idx.end(), [&](int a, int c) { return f.items[(size_t)a].pos < f.items[(size_t)c].pos; });
        std::vector<uint8_t> nb;
        nb.reserve(f.buf.size() + 0x100);
        nb.insert(nb.end(), f.buf.begin(), f.buf.begin() + f.dataoff);  // 保留文件头
        size_t src = f.dataoff;
        for (int i : idx) {
            const auto& it = f.items[(size_t)i];
            if (it.pos < src) { err = "重排冲突(位置异常)"; return false; }
            nb.insert(nb.end(), f.buf.begin() + src, f.buf.begin() + it.pos);
            if (!it.removed) {  // 删除帧: 不加回(即丢弃)
                std::vector<uint8_t> rec;
                GamItem::BuildRecord(it, rec);
                nb.insert(nb.end(), rec.begin(), rec.end());
            }
            src = it.pos + 4 + it.len + 8;
        }
        nb.insert(nb.end(), f.buf.begin() + src, f.buf.end());
        // 平移量: delta_at(旧位置) = 该位置之前所有变更物品的长度差之和(removed 为负)
        auto delta_at = [&](size_t old_pos) -> long long {
            long long d = 0;
            for (int i : idx) {
                const auto& it = f.items[(size_t)i];
                if (it.pos < old_pos) {
                    if (it.removed) d -= (long long)(4 + it.len + 8);
                    else d += (long long)it.content.size() - (long long)it.len;
                }
            }
            return d;
        };
        auto has_change = [&](const GamContainer& c) {
            for (int i : c.item_idx)
                if (i >= 0 && (size_t)i < f.items.size() &&
                    ((f.items[(size_t)i].dirty && !f.items[(size_t)i].is_new) ||
                     f.items[(size_t)i].removed))
                    return true;
            return false;
        };
        // 递减被删物品所属组的计数(写在平移后的位置)
        {
            std::map<size_t, int> dec;
            for (int i : idx)
                if (f.items[(size_t)i].removed && f.items[(size_t)i].cnt_off)
                    dec[f.items[(size_t)i].cnt_off] += 1;
            for (auto& kv : dec) {
                size_t off_old = kv.first;
                size_t off_new = (size_t)((long long)off_old + delta_at(off_old));
                if (off_old + 4 > f.buf.size() || off_new + 4 > nb.size()) continue;
                uint32_t c = X59Dw(f.buf, off_old);
                uint32_t nc = (c >= (uint32_t)kv.second) ? c - (uint32_t)kv.second : 0;
                std::vector<uint8_t> cb;
                RawX59Range((const uint8_t*)&nc, 4, 0x59, cb);
                memcpy(&nb[off_new], cb.data(), 4);
            }
        }
        // 更新每个受影响容器的尾部长度回显(仅当其旧尾 echo==旧容器长)
        bool first_dirty = false;
        for (auto& c : f.containers) {
            if (!has_change(c)) continue;
            if (c.start == f.dataoff && c.end == f.dataoff + f.cont_len) first_dirty = true;
            if (c.end < 4 || c.end > f.buf.size()) continue;
            uint32_t old_len = (uint32_t)(c.end - c.start);
            std::vector<uint8_t> tb(f.buf.begin() + (ptrdiff_t)c.end - 4,
                                    f.buf.begin() + (ptrdiff_t)c.end);
            uint32_t old_tail = X59Dw(tb, 0, 0x59);
            if (old_tail != old_len) continue;  // 非纯 echo 容器, 不动
            long long d_end = delta_at(c.end);
            size_t new_end = (size_t)((long long)c.end + d_end);
            size_t new_start = (size_t)((long long)c.start + delta_at(c.start));
            uint32_t new_len = (uint32_t)(new_end - new_start);
            if (new_end >= 4 && new_end <= nb.size()) {
                std::vector<uint8_t> totb;
                RawX59Range((const uint8_t*)&new_len, 4, 0x59, totb);
                memcpy(&nb[new_end - 4], totb.data(), 4);
            }
        }
        // h2 @0x5C = 首容器长度(连续密钥 0x61..0x64)
        if (first_dirty) {
            for (auto& c : f.containers) {
                if (c.start == f.dataoff && c.end == f.dataoff + f.cont_len) {
                    size_t new_end = (size_t)((long long)c.end + delta_at(c.end));
                    size_t new_start = (size_t)((long long)c.start + delta_at(c.start));
                    uint32_t new_len = (uint32_t)(new_end - new_start);
                    for (int i = 0; i < 4; ++i)
                        nb[(size_t)(0x5C + i)] =
                            (uint8_t)(((new_len >> (8 * i)) & 0xFF) ^ (0x61 + i));
                    break;
                }
            }
        }
        b = std::move(nb);
        // 重读一遍, 刷新容器锚点/cont_len(供下方新增插入使用)
        poq::GamFile tf;
        tf.buf = std::move(b);
        if (!ParseLoaded(tf, f.path, err)) return false;
        f = std::move(tf);
        b = f.buf;
    } else {
        // 等长: 就地重写 dirty 物品; 跳过 is_new
        for (auto& it : f.items) {
            if (!it.dirty || it.is_new) continue;
            if (it.content.size() != it.len) { err = "物品内容长度变化需走插入流程"; return false; }
            if (it.pos + 4 + it.len + 8 > f.buf.size()) { err = "物品位置越界"; return false; }
            std::vector<uint8_t> lenb, ccb, cntb, totb;
            RawX59Range((const uint8_t*)&it.len, 4, 0x59, lenb);
            RawX59Range(it.content.data(), it.content.size(), 0x59, ccb);
            uint32_t zero = 0;
            RawX59Range((const uint8_t*)&zero, 4, 0x59, cntb);
            uint32_t tot = it.len + 12;
            RawX59Range((const uint8_t*)&tot, 4, 0x59, totb);
            size_t q = it.pos;
            memcpy(&b[q], lenb.data(), 4); q += 4;
            memcpy(&b[q], ccb.data(), ccb.size()); q += ccb.size();
            memcpy(&b[q], cntb.data(), 4); q += 4;
            memcpy(&b[q], totb.data(), 4);
        }
    }
    // 2) 插入新增物品
    for (const auto& ad : added) {
        int ci = ad.first;
        if (ci < 0 || ci >= (int)f.containers.size()) continue;
        const auto& rec = ad.second;
        GamContainer& c = f.containers[(size_t)ci];
        size_t pos_at = c.insert_pos;
        size_t cnt_off = c.insert_cnt_off;
        if (pos_at > b.size() || cnt_off >= pos_at) { err = "插入位置异常"; return false; }
        // bump count (x59 编码)
        uint32_t old_c = X59Dw(b, cnt_off);
        if (old_c >= 0x10000) {
            char hb[128];
            std::snprintf(hb, sizeof(hb), "计数异常(容器 %s 计数偏移=0x%X 值=0x%X)",
                          c.alias.c_str(), (unsigned)cnt_off, (unsigned)old_c);
            err = hb;
            return false;
        }
        uint32_t new_c = old_c + 1;
        std::vector<uint8_t> cntb;
        RawX59Range((const uint8_t*)&new_c, 4, 0x59, cntb);
        memcpy(&b[cnt_off], cntb.data(), 4);
        // 插入记录
        b.insert(b.begin() + (ptrdiff_t)pos_at, rec.begin(), rec.end());
        // 若插入目标有 [total==长度] 字段, 更新
        // 首个容器: 更新 hdr dword[2] + 容器尾 total
        if (c.first) {
            uint32_t new_clen = f.cont_len + (uint32_t)rec.size();
            f.cont_len = new_clen;
            // hdr dword[2] @0x5C : 40B 头连续密钥 0x61..(0x5C-0x54=8)
            {
                uint32_t v = new_clen;
                for (int i = 0; i < 4; ++i)
                    b[(size_t)(0x5C + i)] = (uint8_t)(((v >> (8 * i)) & 0xFF) ^ (0x61 + i));
            }
            size_t total_off = f.dataoff + new_clen - 4;
            if (total_off + 4 <= b.size()) {
                uint32_t v = new_clen;
                std::vector<uint8_t> totb;
                RawX59Range((const uint8_t*)&v, 4, 0x59, totb);
                memcpy(&b[total_off], totb.data(), 4);
            }
        }
    }
    FILE* fp = FOpenW(out_path, L"wb");
    if (!fp) { err = "无法写入: " + out_path; return false; }
    fwrite(b.data(), 1, b.size(), fp);
    fclose(fp);
    return true;
}

}  // namespace poq