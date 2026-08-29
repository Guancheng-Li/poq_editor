#include "GamFile.h"

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
    {"req_agi", 0x130}, {"req_con", 0x134}, {"wx", 0x140},
};
}  // namespace

const std::vector<GamFile::FieldDesc>& GamFile::FieldList() {
    static const std::vector<FieldDesc> k = {
        {"lvl", "等级"}, {"dur", "耐久"}, {"durmax", "耐久上限"}, {"price", "价格"},
        {"wgt", "重量"}, {"type", "类型"}, {"ident", "鉴定"}, {"sockets", "孔数"},
        {"req_lvl", "等级需求"}, {"req_str", "力量需求"}, {"req_agi", "敏捷需求"},
        {"req_con", "体质需求"}, {"wx", "五行"}, {"f_118", "攻击"}, {"f_11c", "防御"},
        {"f_110", "模板ID"}, {"f_10c", "经验"}, {"f_c8", "?C8"}, {"f_e4", "?E4"},
        {"f_e8", "?E8"}, {"f_100", "?100"}, {"f_104", "?104"}, {"f_124", "?124"},
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

std::string GamItem::GetGInfo() const {
    // 找字段名串 "G_Info", 其后紧跟的字符串即词条文本
    for (size_t off = 0; off + 4 < content.size(); ++off) {
        std::string s;
        size_t used = 0;
        if (GamFile::DecStr(content, off, s, used)) {
            if (s == "G_Info" && off + used + 4 < content.size()) {
                std::string val;
                size_t used2 = 0;
                if (GamFile::DecStr(content, off + used, val, used2)) return val;
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
                dirty = true;
                return true;
            }
        }
    }
    return false;
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
        // [K / [H / [Z ...] (可跨行: 该行只写 "["... ]
        size_t n = inner.size();
        size_t k = 0;
        if (n >= 2 && inner[0] == '[') {
            char c = inner[1];
            if (c == 'K' || c == 'H' || c == 'Z' || c == 'C') {
                cur_color = c;
                k = 2;
                // 该行结尾的 ']' 表示组结束
                if (inner[n - 1] == ']') { group_ends = true; n -= 1; }
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
                        r.vmax = vmax;
                        r.has_max = has_max;
                        r.is_percent = is_percent;
                        rows.push_back(std::move(r));
                    }
                }
            }
        }
        if (group_ends) cur_color = 0;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return !rows.empty();
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
    // emit: 组内 attr 尾随 " "(非百分比) 或 "\x0f"(百分比)
    auto emit = [&](const GInfoRow& r) {
        return fmt(r) + (r.is_percent ? std::string(1, (char)0x0F) : std::string(" "));
    };
    for (size_t i = 0; i < rows.size(); ++i) {
        const GInfoRow& r = rows[i];
        if (r.color == 0) {
            // 白(无组): 无尾随空格, 百分比才有 \x0f
            out += fmt(r);
            if (r.is_percent) out += (char)0x0F;
            out += "\n";
        } else {
            size_t j = i + 1;
            while (j < rows.size() && rows[j].color == r.color) ++j;
            out += "[";
            out += r.color;
            if (j == i + 1) {
                // 单属性组: 无尾随空格(百分比才有 \x0f)
                out += fmt(r);
                if (r.is_percent) out += (char)0x0F;
            } else {
                for (size_t k = i; k < j; ++k) {
                    if (k > i) out += "\n";
                    out += emit(rows[k]);
                }
            }
            out += "]\n";
            i = j - 1;
        }
    }
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
        int idx = (int)f.items.size();
        f.items.push_back(std::move(it));
        cur.item_idx.push_back(idx);
        cur.insert_pos = p + 4 + L + 8;
        cur.insert_cnt_off = active_cnt_off;
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
    // 先判断是否有长度变化的 dirty 物品(词条改长/改短/加词条)
    bool need_relayout = false;
    for (const auto& it : f.items)
        if (it.dirty && !it.is_new && it.content.size() != it.len) { need_relayout = true; break; }

    std::vector<uint8_t> b = f.buf;
    if (need_relayout) {
        // 单容器档(根容器延伸到 EOF): h2=容器长度, EOF-4 为其回显。多容器档无法安全中途变长。
        if (!(f.dataoff && f.dataoff + f.cont_len == f.buf.size())) {
            err = "此存档为多地图结构，暂不支持中途把物品改长。请用\"复制物品/粘贴物品\"或\"创建新武器\"生成新物品代替。";
            return false;
        }
        // 按 pos 升序把所有 dirty 物品重编(等长/变长一起重建), 其间字节整体平移
        std::vector<int> idx;
        for (size_t i = 0; i < f.items.size(); ++i)
            if (f.items[i].dirty && !f.items[i].is_new) idx.push_back((int)i);
        std::sort(idx.begin(), idx.end(), [&](int a, int c) { return f.items[(size_t)a].pos < f.items[(size_t)c].pos; });
        std::vector<uint8_t> nb;
        nb.reserve(f.buf.size() + 0x100);
        nb.insert(nb.end(), f.buf.begin(), f.buf.begin() + f.dataoff);  // 保留文件头
        size_t src = f.dataoff;
        for (int i : idx) {
            const auto& it = f.items[(size_t)i];
            if (it.pos < src) { err = "重排冲突(位置异常)"; return false; }
            nb.insert(nb.end(), f.buf.begin() + src, f.buf.begin() + it.pos);
            std::vector<uint8_t> rec;
            GamItem::BuildRecord(it, rec);
            nb.insert(nb.end(), rec.begin(), rec.end());
            src = it.pos + 4 + it.len + 8;
        }
        nb.insert(nb.end(), f.buf.begin() + src, f.buf.end());
        // 更新容器长度 h2 @0x5C(连续密钥 0x61) 与 EOF-4 回显(重启密钥 0x59)
        uint32_t new_len = (uint32_t)(nb.size() - f.dataoff);
        for (int i = 0; i < 4; ++i)
            nb[(size_t)(0x5C + i)] = (uint8_t)(((new_len >> (8 * i)) & 0xFF) ^ (0x61 + i));
        uint32_t eof = (uint32_t)(nb.size() - 4);
        {
            std::vector<uint8_t> totb;
            RawX59Range((const uint8_t*)&new_len, 4, 0x59, totb);
            memcpy(&nb[eof], totb.data(), 4);
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
        if (old_c >= 0xFFFFFF) { err = "计数溢出"; return false; }
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