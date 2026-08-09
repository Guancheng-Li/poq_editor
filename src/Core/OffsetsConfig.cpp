#include "Core/OffsetsConfig.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace poq {

namespace {

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

}  // namespace

// ============================================================================
// BasicFieldCfg
// ============================================================================

bool BasicFieldCfg::IsForced(const std::optional<int>& type_val) const {
    if (!force_when || !type_val) return false;
    return std::find(force_when->values.begin(), force_when->values.end(), *type_val) !=
           force_when->values.end();
}

int BasicFieldCfg::Clamp(int v) const {
    if (min && v < *min) v = *min;
    if (max && v > *max) v = *max;
    return v;
}

// ============================================================================
// OffsetsConfig
// ============================================================================

const ModeCfg& OffsetsConfig::Current() const {
    auto it = modes.find(current_mode);
    if (it != modes.end()) return it->second;
    it = modes.find("mode0");
    if (it != modes.end()) return it->second;
    static const ModeCfg kFallback = DefaultMode();
    return kFallback;
}

long long OffsetsConfig::ParseLong(const std::string& s_in) {
    std::string s = Trim(s_in);
    int base = 16;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty()) return 0;
    return std::strtoll(s.c_str(), nullptr, base);
}

// ============================================================================
// 类型映射文本(与 Python offsets_config.py 的 TYPE_MAPPING_TEXT 一致)
// ============================================================================

const std::string& OffsetsConfig::TypeMappingText() {
    static const std::string kText =
        "0:宝石-镶嵌\n"
        "1:木材-打造\n"
        "2:矿石-打造\n"
        "3:兽骨-打造\n"
        "4:兽皮-打造\n"
        "5:兽筋-打造\n"
        "6:未知\n"
        "7:短刀-刺客\n"
        "8:长刀-游侠\n"
        "9:短剑-刺客\n"
        "10:长剑-游侠\n"
        "11:弓-游侠刺客\n"
        "12:弩-刺客\n"
        "13:箭-游侠刺客\n"
        "14:矢-刺客\n"
        "15:长杖-巫师术士\n"
        "16:法杖-巫师\n"
        "17:权杖-术士\n"
        "18:长戈-力士\n"
        "19:枪矛-力士\n"
        "20:斧钺-力士\n"
        "21:短斧-力士\n"
        "22:短戈-游侠\n"
        "23:短棒-力士\n"
        "24:短锤-力士\n"
        "25:头盔\n"
        "26:盔甲\n"
        "27:盾牌-游侠\n"
        "28:鞋子\n"
        "29:项链\n"
        "30:戒指\n"
        "31:护腕\n"
        "32:腰带\n"
        "33:法冠-巫师术士\n"
        "34:法袍-巫师术士\n"
        "35:未知\n"
        "36:未知\n"
        "37:未知\n"
        "38:未知\n"
        "39:未知\n"
        "40:未知\n"
        "41:未知\n"
        "42:未知\n"
        "43:未知\n"
        "44:未知\n"
        "45:未知\n"
        "46:未知\n"
        "47:未知\n"
        "48:未知\n"
        "49:未知\n"
        "50:未知\n"
        "51:肉类药品\n"
        "52:打造杂项\n";
    return kText;
}

std::map<int, std::string> OffsetsConfig::ParseTypeMapping(const std::string& text) {
    std::map<int, std::string> result;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string v = Trim(line.substr(0, colon));
        std::string name = Trim(line.substr(colon + 1));
        if (v.empty()) continue;
        char* end = nullptr;
        long val = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str()) continue;
        result[static_cast<int>(val)] = name;
    }
    return result;
}

// ============================================================================
// 默认模式(与 Python OffsetsConfig.default_mode 一致)
// ============================================================================

ModeCfg OffsetsConfig::DefaultMode() {
    ModeCfg m;
    m.name = "默认";
    m.item_base_adjust = 0;

    auto F = [](const char* label, uint32_t offset, const char* type) {
        BasicFieldCfg f;
        f.label = label;
        f.offset = offset;
        f.type = type;
        return f;
    };

    BasicFieldCfg bf;

    bf = F("武器名字", 0x28, "string_ptr");
    bf.codepage = 936;
    m.basic_fields.push_back(bf);

    bf = F("武器别名", 0x2C, "string_ptr");
    bf.readonly = true;
    bf.codepage = 0;
    m.basic_fields.push_back(bf);

    bf = F("武器ID", 0x20, "word");
    bf.readonly = true;
    m.basic_fields.push_back(bf);

    bf = F("武器等级", 0x34, "dword");
    bf.min = 1;
    bf.max = 9;
    m.basic_fields.push_back(bf);

    bf = F("是否鉴定", 0x108, "enum");
    bf.min = 0;
    bf.max = 1;
    bf.enum_map = {{0, "未鉴定"}, {1, "已鉴定"}};
    m.basic_fields.push_back(bf);

    bf = F("耐久度", 0x58, "dword");
    bf.min = 0;
    bf.max = 254;
    bf.max_ref = "耐久度上限";
    bf.force_when = ForceWhenCfg{"类型", {13, 14}, -1};
    m.basic_fields.push_back(bf);

    bf = F("耐久度上限", 0x5C, "dword");
    bf.min = 0;
    bf.max = 254;
    bf.hint = "0 为永不损坏";
    bf.force_when = ForceWhenCfg{"类型", {13, 14}, -1};
    m.basic_fields.push_back(bf);

    bf = F("基准价格", 0xF4, "dword");
    bf.min = 0;
    bf.max = 65535;
    m.basic_fields.push_back(bf);

    bf = F("重量", 0xF8, "dword");
    bf.min = 0;
    bf.max = 127;
    m.basic_fields.push_back(bf);

    bf = F("类型", 0xFC, "enum");
    bf.enum_map = ParseTypeMapping(TypeMappingText());
    bf.enum_display_value = true;
    bf.hint = "谨慎修改";
    m.basic_fields.push_back(bf);

    bf = F("孔数", 0x124, "dword");
    bf.min = 0;
    bf.max = 3;
    bf.force_when = ForceWhenCfg{"类型", {13, 14}, 0};
    m.basic_fields.push_back(bf);

    bf = F("等级需求", 0x150, "dword");
    bf.min = 0;
    bf.max = 1023;
    m.basic_fields.push_back(bf);

    bf = F("力量需求", 0x154, "dword");
    bf.min = 0;
    bf.max = 1023;
    m.basic_fields.push_back(bf);

    bf = F("敏捷需求", 0x158, "dword");
    bf.min = 0;
    bf.max = 1023;
    m.basic_fields.push_back(bf);

    bf = F("体质需求", 0x15C, "dword");
    bf.min = 0;
    bf.max = 1023;
    m.basic_fields.push_back(bf);

    bf = F("五行", 0x168, "enum");
    bf.min = 0;
    bf.max = 5;
    bf.enum_map = {{0, "无"}, {1, "金"}, {2, "木"}, {3, "水"}, {4, "火"}, {5, "土"}};
    m.basic_fields.push_back(bf);

    bf = F("弓箭数量", 0x398, "dword");
    bf.min = 0;
    bf.max = 32767;
    bf.conditional = ConditionalCfg{"类型", {13, 14}};
    bf.hint = "仅类型为 13(箭)/14(矢) 时生效";
    m.basic_fields.push_back(bf);

    return m;
}

OffsetsConfig OffsetsConfig::Default() {
    OffsetsConfig cfg;
    cfg.modes["mode0"] = DefaultMode();
    cfg.current_mode = "mode0";
    return cfg;
}

}  // namespace poq
