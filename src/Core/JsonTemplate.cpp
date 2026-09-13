#include "Core/JsonTemplate.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "Core/ItemData.h"
#include "Core/Json.h"

namespace poq {

namespace {

// Trim ASCII whitespace from both ends.
std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Python int(s, 0) 语义:"0x" 十六进制、前导 0 八进制、否则十进制。失败返回 nullopt。
std::optional<long long> ParseIntBase0(const std::string& s) {
    if (s.empty()) return std::nullopt;
    int base = 10;
    size_t start = (s[0] == '+' || s[0] == '-') ? 1 : 0;
    if (s.size() - start >= 2 && s[start] == '0' && (s[start + 1] == 'x' || s[start + 1] == 'X'))
        base = 16;
    else if (s.size() - start >= 2 && s[start] == '0')
        base = 8;
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, base);
    if (errno != 0 || end == s.c_str() || *end != '\0') return std::nullopt;
    return v;
}

// 错误信息用:字符串带引号,数字直接输出(近似 Python 的 repr)。
std::string JsonValueRepr(const json::Value& v) {
    if (v.IsString()) return "'" + v.string + "'";
    if (v.IsBool()) return v.boolean ? "True" : "False";
    if (v.IsNumber()) return std::to_string(v.number);
    if (v.IsNull()) return "None";
    return std::string();
}

std::string ReprString(const std::string& s) { return "'" + s + "'"; }

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开文件:" + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("无法写入文件:" + path);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

}  // namespace

int JsonTemplate::Clamp(const BasicFieldCfg& f, int v) { return f.Clamp(v); }

std::optional<int> JsonTemplate::ToInt(const json::Value& val, const BasicFieldCfg& f) {
    if (!f.enum_map.empty()) {
        if (val.IsNumber()) {
            double d = val.number;
            if (d == static_cast<double>(static_cast<long long>(d)))
                return static_cast<int>(d);
            return std::nullopt;
        }
        if (val.IsString()) {
            std::string s = Trim(val.string);
            // "值：名字" 格式(如 "10：剑")-> 解析前导数字
            for (const std::string& sep : {"\xEF\xBC\x9A" /*：*/, ":"}) {
                size_t pos = s.find(sep);
                if (pos != std::string::npos) {
                    auto head = ParseIntBase0(Trim(s.substr(0, pos)));
                    if (head) return static_cast<int>(*head);
                    break;
                }
            }
            // 纯名字匹配
            for (const auto& kv : f.enum_map) {
                if (s == kv.second) return kv.first;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }
    // 数值字段
    if (val.IsBool()) return std::nullopt;
    if (val.IsNumber()) return static_cast<int>(val.number);
    if (val.IsString()) {
        auto v = ParseIntBase0(Trim(val.string));
        if (v) return static_cast<int>(*v);
    }
    return std::nullopt;
}

std::optional<float> JsonTemplate::ParseAttrValue(const json::Value& raw) {
    if (raw.IsBool()) return std::nullopt;
    if (raw.IsNumber()) {
        double d = raw.number;
        // Python 拒绝浮点数(15.0/12.5)。这里按"属性值必须是整数"的精神:
        // 非整数值(如 12.5)拒绝。
        if (d != static_cast<double>(static_cast<long long>(d))) return std::nullopt;
        return static_cast<float>(d);
    }
    if (raw.IsString()) {
        auto v = ParseIntBase0(Trim(raw.string));
        if (!v) return std::nullopt;
        return static_cast<float>(*v);
    }
    return std::nullopt;
}

json::Value JsonTemplate::ToJsonValue(float v) {
    if (v == static_cast<float>(static_cast<long long>(v)))
        return json::Value::MakeNumber(static_cast<double>(static_cast<int>(v)));
    return json::Value::MakeNumber(static_cast<double>(v));
}

// ============================================================================
// Load
// ============================================================================

TemplateLoadResult JsonTemplate::Load(const std::string& path,
                                      const std::vector<BasicFieldCfg>& basic_fields) {
    std::string data = ReadFile(path);
    json::Value root;
    try {
        root = json::Parse(data);
    } catch (const std::exception& ex) {
        throw std::runtime_error("JSON 解析失败:" + std::string(ex.what()));
    }
    if (!root.IsObject()) throw std::runtime_error("JSON 顶层必须是对象");

    std::map<std::string, const BasicFieldCfg*> field_map;
    for (const auto& f : basic_fields) field_map[f.label] = &f;

    TemplateLoadResult result;

    // ---- 基础属性 ----
    for (const auto& kv : root.object) {
        if (kv.first == "属性列表") continue;
        std::string label = kv.first;
        auto fit = field_map.find(label);
        if (fit == field_map.end() || fit->second->readonly) continue;  // 未知字段/只读:跳过
        auto iv = ToInt(kv.second, *fit->second);
        if (!iv) continue;
        result.basic_values[label] = Clamp(*fit->second, *iv);
    }

    // ---- 属性列表 ----
    const json::Value* raw_list = root.Find("属性列表");
    if (raw_list && raw_list->IsArray()) {
        const std::vector<json::Value>& arr = raw_list->array;
        for (size_t idx = 0; idx < arr.size(); ++idx) {
            const json::Value& item = arr[idx];
            if (!item.IsObject()) {
                result.errors.push_back("第 " + std::to_string(idx + 1) + " 条不是对象,跳过");
                continue;
            }
            const json::Value* color_v = item.Find("颜色");
            const json::Value* name_v = item.Find("属性名");
            const json::Value* type_v = item.Find("类型");
            const json::Value* active_v = item.Find("激活");
            std::string color_txt = color_v && color_v->IsString() ? color_v->string : "";
            std::string name_txt = name_v && name_v->IsString() ? name_v->string : "";
            std::string type_txt = type_v && type_v->IsString() ? type_v->string : "";
            // 激活状态可选;缺省(-1)时按颜色决定(见下方 color 解析后)
            int active_field = -1;
            if (active_v && active_v->IsNumber()) active_field = (active_v->number != 0) ? 1 : 0;
            else if (active_v && active_v->IsBool()) active_field = active_v->boolean ? 1 : 0;
            // 属性值可选,缺省 0(对应 Python 的 item.get("属性值", 0))
            const json::Value* value_raw = item.Find("属性值");
            json::Value value_default = json::Value::MakeNumber(0);
            const json::Value& value = value_raw ? *value_raw : value_default;

            auto color = ItemData::ParseColorDisplay(color_txt);
            auto aid = ItemData::ParseAttrDisplay(name_txt);
            auto prec = ItemData::ParsePercentDisplay(type_txt);
            auto val = ParseAttrValue(value);
            bool valid =
                color && aid && ItemData::AttrNames().count(*aid) > 0 && prec && val;
            if (!valid) {
                result.errors.push_back(
                    "第 " + std::to_string(idx + 1) +
                    " 条属性存在错误(颜色=" + ReprString(color_txt) +
                    " 属性名=" + ReprString(name_txt) +
                    " 类型=" + ReprString(type_txt) +
                    " 属性值=" + JsonValueRepr(value) + "),跳过");
                continue;
            }
            // 缺省激活状态:旧模板无"激活"字段时,浅蓝色(color=2)默认未激活,其它颜色默认激活
            // (flag=0 对非浅蓝词条可能被游戏当隐藏,保持 1 安全)。
            uint32_t active = (active_field >= 0)
                                  ? static_cast<uint32_t>(active_field)
                                  : ((*color == 2) ? 0u : 1u);
            result.attrs.push_back(ItemProperty(0, *aid, static_cast<uint32_t>(*color), active,
                                                static_cast<uint32_t>(*prec), *val));
        }
    }
    return result;
}

// ============================================================================
// Save
// ============================================================================

void JsonTemplate::Save(const std::string& path, const BasicState& basic_state,
                        const std::vector<BasicFieldCfg>& basic_fields,
                        const std::vector<ItemProperty>& attrs) {
    std::map<std::string, const BasicFieldCfg*> field_map;
    for (const auto& f : basic_fields) field_map[f.label] = &f;

    json::Value out = json::Value::MakeObject();

    // 按 basic_fields 顺序输出(先 ints 后 strs 的字段会被各自匹配),保证模板可读。
    for (const auto& f : basic_fields) {
        const std::string& label = f.label;
        if (f.type == "string_ptr") {
            auto it = basic_state.strs.find(label);
            if (it != basic_state.strs.end())
                out.Set(label, json::Value::MakeString(it->second));
            continue;
        }
        auto it = basic_state.ints.find(label);
        if (it == basic_state.ints.end()) continue;
        if (!f.enum_map.empty()) {
            int v = it->second;
            auto eit = f.enum_map.find(v);
            if (f.enum_display_value) {
                std::string name = eit != f.enum_map.end() ? eit->second
                                                           : std::to_string(v);
                out.Set(label, json::Value::MakeString(std::to_string(v) + "：" + name));
            } else {
                if (eit != f.enum_map.end())
                    out.Set(label, json::Value::MakeString(eit->second));
                else
                    out.Set(label, json::Value::MakeNumber(v));
            }
        } else {
            out.Set(label, json::Value::MakeNumber(it->second));
        }
    }

    json::Value attrs_arr = json::Value::MakeArray();
    for (const ItemProperty& p : attrs) {
        json::Value o = json::Value::MakeObject();
        o.Set("颜色", json::Value::MakeString(ItemData::ColorDisplay(static_cast<int>(p.color))));
        o.Set("激活", json::Value::MakeNumber(static_cast<double>(p.active ? 1 : 0)));
        o.Set("属性名", json::Value::MakeString(ItemData::AttrDisplay(p.attr_id)));
        o.Set("类型", json::Value::MakeString(ItemData::PercentDisplay(static_cast<int>(p.precent))));
        o.Set("属性值", ToJsonValue(p.value));
        attrs_arr.PushBack(std::move(o));
    }
    out.Set("属性列表", std::move(attrs_arr));

    WriteFile(path, json::Write(out, true) + "\n");
}

}  // namespace poq
