#include "Core/ItemData.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace poq {
namespace ItemData {

namespace {

// 全角冒号(属性名规范显示分隔符)
std::string kFullColon() {
    return "\xEF\xBC\x9A";
}  // UTF-8 "："

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

const std::map<uint32_t, std::string>& AttrNames() {
    static const std::map<uint32_t, std::string> kMap = {
        {0x00, "异态时间减少"},   {0x01, "五行攻击力"},     {0x02, "最大五行攻击力"},
        {0x03, "五行防御力"},     {0x04, "耐久度"},         {0x05, "级别需求"},
        {0x06, "力量需求"},       {0x07, "体质需求"},       {0x08, "敏捷需求"},
        {0x09, "恢复箭支数量"},   {0x0A, "未知"},           {0x0B, "成功率"},
        {0x0C, "有效距离"},       {0x0D, "最大有效距离"},   {0x0E, "持续时间"},
        {0x0F, "精气消耗"},       {0x10, "未知"},           {0x11, "耐久度消耗"},
        {0x12, "击退敌人几率"},   {0x13, "未知"},           {0x14, "无敌"},
        {0x15, "生命"},           {0x16, "精气"},           {0x17, "最大生命"},
        {0x18, "最大精气"},       {0x19, "负重"},           {0x1A, "最大负重"},
        {0x1B, "力量"},           {0x1C, "体质"},           {0x1D, "敏捷"},
        {0x1E, "未知"},           {0x1F, "悟性"},           {0x20, "智慧"},
        {0x21, "魅力"},           {0x22, "未知"},           {0x23, "未知"},
        {0x24, "未知"},           {0x25, "未知"},           {0x26, "未知"},
        {0x27, "未知"},           {0x28, "金钱"},           {0x29, "普通攻击"},
        {0x2A, "最大普通攻击"},   {0x2B, "金属性攻击"},     {0x2C, "最大金属性攻击"},
        {0x2D, "木属性攻击"},     {0x2E, "最大木属性攻击"}, {0x2F, "水属性攻击"},
        {0x30, "最大水属性攻击"}, {0x31, "火属性攻击"},     {0x32, "最大火属性攻击"},
        {0x33, "土属性攻击"},     {0x34, "最大土属性攻击"}, {0x35, "普通防御"},
        {0x36, "金属性防御"},     {0x37, "木属性防御"},     {0x38, "水属性防御"},
        {0x39, "火属性防御"},     {0x3A, "土属性防御"},     {0x3B, "未知"},
        {0x3C, "闪避率"},         {0x3D, "命中率"},         {0x3E, "生命恢复速度"},
        {0x3F, "精气恢复速度"},   {0x40, "移动速度"},       {0x41, "攻击速度"},
        {0x42, "视野"},           {0x43, "攻击距离"},       {0x44, "反击伤害"},
        {0x45, "自损系数"},       {0x46, "吸命"},           {0x47, "吸魔"},
        {0x48, "致命一击几率"},   {0x49, "会心一击几率"},   {0x4A, "击退敌人几率"},
        {0x4B, "抵抗冲击"},       {0x4C, "物理攻击抗性"},   {0x4D, "五行攻击抗性"},
        {0x4E, "未知"},           {0x4F, "未知"},           {0x50, "眩晕攻击几率"},
        {0x51, "眩晕时间"},       {0x52, "中毒攻击几率"},   {0x53, "中毒时间"},
        {0x54, "冰冻攻击几率"},   {0x55, "冰冻时间"},       {0x56, "燃烧攻击几率"},
        {0x57, "燃烧时间"},       {0x58, "石化攻击几率"},   {0x59, "石化时间"},
        {0x5A, "溃散几率"},       {0x5B, "溃散时间"},       {0x5C, "眩晕抗性"},
        {0x5D, "中毒抗性"},       {0x5E, "冰冻抗性"},       {0x5F, "燃烧抗性"},
        {0x60, "石化抗性"},       {0x61, "溃散抗性"},       {0x62, "特殊状态抗性"},
    };
    return kMap;
}

const std::map<int, std::string>& ColorNames() {
    static const std::map<int, std::string> kMap = {
        {0, "白色"}, {1, "蓝色"}, {2, "浅蓝色"}, {3, "金色"}, {4, "不显示"},
    };
    return kMap;
}

const std::map<int, std::string>& PercentNames() {
    static const std::map<int, std::string> kMap = {
        {0, "数值"},
        {1, "百分比"},
    };
    return kMap;
}

const std::map<int, std::string>& ElementNames() {
    static const std::map<int, std::string> kMap = {
        {0, "无"}, {1, "金"}, {2, "木"}, {3, "水"}, {4, "火"}, {5, "土"},
    };
    return kMap;
}

const std::map<int, std::string>& IdentifiedNames() {
    static const std::map<int, std::string> kMap = {
        {0, "未鉴定"},
        {1, "已鉴定"},
    };
    return kMap;
}

// ------------------------------------------------------------------
// 属性 ID <-> 显示串
// ------------------------------------------------------------------

std::string AttrDisplay(uint32_t attr_id) {
    auto it = AttrNames().find(attr_id);
    std::string name = it != AttrNames().end() ? it->second : std::string("未知");
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%02X", attr_id & 0xFF);
    return std::string(hex) + kFullColon() + name;
}

std::vector<std::string> AttrDisplayList() {
    std::vector<std::string> list;
    for (const auto& kv : AttrNames()) list.push_back(AttrDisplay(kv.first));
    return list;
}

const std::vector<std::string>& AttrDisplayListRef() {
    // UI 每帧都要这份表(道具修改/GAM 词条下拉): 静态缓存一份, 不再每帧新建 ~99 个 string
    static const std::vector<std::string> k = AttrDisplayList();
    return k;
}

const std::vector<std::string>& PercentDisplayListRef() {
    static const std::vector<std::string> k = PercentDisplayList();
    return k;
}

std::optional<uint32_t> ParseAttrDisplay(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::string t = text;
    // 去首尾空白
    size_t a = t.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::nullopt;
    size_t b = t.find_last_not_of(" \t\r\n");
    t = t.substr(a, b - a + 1);

    std::string sep = kFullColon();
    size_t pos = t.find(sep);
    if (pos == std::string::npos) {
        sep = ":";
        pos = t.find(sep);
    }
    if (pos == std::string::npos) return std::nullopt;
    std::string head = t.substr(0, pos);
    a = head.find_first_not_of(" \t\r\n");
    if (a != std::string::npos) {
        b = head.find_last_not_of(" \t\r\n");
        head = head.substr(a, b - a + 1);
    }
    if (head.size() >= 2 && head[0] == '0' && (head[1] == 'x' || head[1] == 'X'))
        head = head.substr(2);
    if (head.empty()) return std::nullopt;
    char* end = nullptr;
    unsigned long v = std::strtoul(head.c_str(), &end, 16);
    if (end == head.c_str() || *end != '\0') return std::nullopt;
    return static_cast<uint32_t>(v);
}

bool IsValidAttrDisplay(const std::string& text) {
    auto aid = ParseAttrDisplay(text);
    return aid && AttrNames().count(*aid) > 0;
}

// ------------------------------------------------------------------
// 颜色 <-> 显示串
// ------------------------------------------------------------------

std::string ColorDisplay(int color) {
    auto it = ColorNames().find(color);
    return it != ColorNames().end() ? it->second : std::string("白色");
}

std::vector<std::string> ColorDisplayList() {
    std::vector<std::string> list;
    for (const auto& kv : ColorNames()) list.push_back(kv.second);
    return list;
}

std::optional<int> ParseColorDisplay(const std::string& text) {
    std::string t = text;
    size_t a = t.find_first_not_of(" \t\r\n");
    if (a != std::string::npos) {
        size_t b = t.find_last_not_of(" \t\r\n");
        t = t.substr(a, b - a + 1);
    }
    for (const auto& kv : ColorNames()) {
        if (t == kv.second) return kv.first;
    }
    return std::nullopt;
}

// ------------------------------------------------------------------
// 百分比 <-> 显示串
// ------------------------------------------------------------------

std::string PercentDisplay(int precent) {
    auto it = PercentNames().find(precent);
    return it != PercentNames().end() ? it->second : std::string("数值");
}

std::vector<std::string> PercentDisplayList() {
    std::vector<std::string> list;
    for (const auto& kv : PercentNames()) list.push_back(kv.second);
    return list;
}

std::optional<int> ParsePercentDisplay(const std::string& text) {
    std::string t = text;
    size_t a = t.find_first_not_of(" \t\r\n");
    if (a != std::string::npos) {
        size_t b = t.find_last_not_of(" \t\r\n");
        t = t.substr(a, b - a + 1);
    }
    for (const auto& kv : PercentNames()) {
        if (t == kv.second) return kv.first;
    }
    return std::nullopt;
}

// ------------------------------------------------------------------
// 属性名 <-> GAM G_Info 词条名
// ------------------------------------------------------------------

std::string AttrGInfoName(const std::string& n) {
    if (n.rfind("最大", 0) == 0) return "最大" + AttrGInfoName(n.substr(6));  // "最大" UTF-8=6B
    if (n == "普通攻击") return "普通攻击力";
    if (n == "普通防御") return "普通防御力";
    if (n == "吸命") return "吸取生命率";
    if (n == "吸魔") return "吸取魔法率";
    if (n == "异态时间减少") return "异常状态时间减少";
    if (n == "恢复箭支数量") return "自动恢复箭支数量";
    if (n == "燃烧攻击几率") return "失明攻击几率";
    if (n == "燃烧时间") return "失明时间";
    if (n == "燃烧抗性") return "失明抗性";
    static const char* elems[] = {"金", "木", "水", "火", "土"};
    for (const char* e : elems) {
        std::string s = e;
        if (n == s + "属性攻击") return s + "系攻击力";
        if (n == s + "属性防御") return s + "系防御力";
    }
    return n;
}

namespace {
std::string U8ToGbk(const std::string& u8) {
    if (u8.empty()) return "";
    int n = ::MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), nullptr, 0);
    if (n <= 0) return u8;
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), &w[0], n);
    int m = ::WideCharToMultiByte(936, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)m, '\0');
    ::WideCharToMultiByte(936, 0, w.data(), n, &s[0], m, nullptr, nullptr);
    return s;
}
}  // namespace

int AttrIdFromGInfoName(const std::string& gbk) {
    static const std::map<std::string, int> kMap = [] {
        std::map<std::string, int> m;
        for (const auto& kv : AttrNames()) m[U8ToGbk(AttrGInfoName(kv.second))] = (int)kv.first;
        return m;
    }();
    auto it = kMap.find(gbk);
    return it == kMap.end() ? -1 : it->second;
}

// ------------------------------------------------------------------
// 上下限成对属性(min id -> max id)
// ------------------------------------------------------------------

const std::map<int, int>& AffixPairs() {
    // 游戏实测(写档回读): 0x01/0x02 渲染成 "五行攻击力 +下 ~ +上"、0x0C/0x0D 渲染成
    // "有效距离 +下 ~ +上"; 0x29..0x34 同理。最大生命/最大精气/最大负重**不是**成对项。
    static const std::map<int, int> kMap = {
        {0x01, 0x02}, {0x0C, 0x0D}, {0x29, 0x2A}, {0x2B, 0x2C},
        {0x2D, 0x2E}, {0x2F, 0x30}, {0x31, 0x32}, {0x33, 0x34},
    };
    return kMap;
}

bool IsAffixPairMinId(int id) {
    return AffixPairs().count(id) > 0;
}

}  // namespace ItemData
}  // namespace poq
