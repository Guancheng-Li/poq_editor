#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// 轻量 JSON DOM(UTF-8),替代 Qt 的 QJson*,仅用于属性模板的加载/导出。
///
/// 对象成员用有序 vector<pair<string,Value>> 保存(保留插入顺序,与 QJsonObject
/// 的迭代行为兼容)。数值统一存 double,写出时整数按整数形式、否则 %.15g。
/// Parse 失败抛 std::runtime_error。
namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };

    Type type = Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    // ---- 构造 ----
    static Value MakeNull() { return Value(); }
    static Value MakeBool(bool b) {
        Value v;
        v.type = Bool;
        v.boolean = b;
        return v;
    }
    static Value MakeNumber(double d) {
        Value v;
        v.type = Number;
        v.number = d;
        return v;
    }
    static Value MakeString(std::string s) {
        Value v;
        v.type = String;
        v.string = std::move(s);
        return v;
    }
    static Value MakeArray() {
        Value v;
        v.type = Array;
        return v;
    }
    static Value MakeObject() {
        Value v;
        v.type = Object;
        return v;
    }

    // ---- 类型判断 ----
    bool IsNull() const { return type == Null; }
    bool IsBool() const { return type == Bool; }
    bool IsNumber() const { return type == Number; }
    bool IsString() const { return type == String; }
    bool IsArray() const { return type == Array; }
    bool IsObject() const { return type == Object; }

    // ---- 对象访问 ----
    /// 按 key 找成员,不存在返回 nullptr。
    const Value* Find(const std::string& key) const {
        for (const auto& kv : object)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool Contains(const std::string& key) const { return Find(key) != nullptr; }
    /// 按 key 取成员并写回;不存在时不修改并返回 false。
    bool Get(const std::string& key, Value& out) const {
        const Value* v = Find(key);
        if (!v) return false;
        out = *v;
        return true;
    }
    /// 追加/覆盖一个成员(已存在则覆盖,保持原位置)。
    void Set(const std::string& key, Value v) {
        for (auto& kv : object) {
            if (kv.first == key) {
                kv.second = std::move(v);
                return;
            }
        }
        object.emplace_back(key, std::move(v));
    }
    void PushBack(Value v) { array.push_back(std::move(v)); }
};

/// 解析 JSON 文本。失败抛 std::runtime_error(带出错位置)。
Value Parse(const std::string& text);

/// 序列化。indent=true 时用 2 空格缩进(Qt QJsonDocument::Indented 风格)。
std::string Write(const Value& v, bool indent);

}  // namespace json
