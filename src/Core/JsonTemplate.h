#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Core/ItemProperty.h"
#include "Core/Json.h"
#include "Core/OffsetsConfig.h"

namespace poq {

/// 基础属性面板的当前值(供导出/保存用)。
/// 数值字段(dword/word/enum/弓箭数量)存 int;字符串字段(string_ptr)存 UTF-8。
struct BasicState {
    std::map<std::string, int> ints;
    std::map<std::string, std::string> strs;
};

/// JSON 物品属性模板的加载结果(对应 Python json_template.load 的返回值)。
struct TemplateLoadResult {
    /// 基础属性:{label: int}(已 clamp,不含只读字段)。
    std::map<std::string, int> basic_values;
    /// 字符串基础字段:{规范标签: 值}(如 道具名字/道具别名)。UTF-8,与导出同编码。
    std::map<std::string, std::string> basic_strings;
    /// 合法的属性列表。
    std::vector<ItemProperty> attrs;
    /// 被跳过属性的错误描述(非空时 UI 弹窗)。
    std::vector<std::string> errors;
};

/// JSON 物品属性模板的加载与导出(对应 Python 的 json_template.py)。
///
/// 模板格式:
///   {
///     "武器等级": 5,                # 基础属性:按字段名,值见下
///     "是否鉴定": "已鉴定",          # enum 存显示名
///     "五行": "金",                  # enum 存显示名
///     "耐久度": 100,
///     ...
///     "属性列表": [
///       {"颜色": "浅蓝色", "属性名": "3C：闪避率", "类型": "数值", "属性值": 15},
///       ...
///     ]
///   }
///
/// 基础属性:没有特殊说明的字段存原始内存值(int);是否鉴定/五行 存显示名;
/// 武器名字/别名 存字符串(只读,加载时不写内存)。属性值(属性值)可选,缺省 0。
class JsonTemplate {
public:
    /// 加载 JSON 模板。
    ///
    /// 失败(文件不存在 / JSON 解析失败 / 顶层不是对象)时抛 std::runtime_error,
    /// 调用方弹窗且不执行任何加载动作。
    static TemplateLoadResult Load(const std::string& path,
                                   const std::vector<BasicFieldCfg>& basic_fields);

    /// 导出当前物品属性为 JSON(属性名="3C：闪避率",颜色="浅蓝色",类型="数值",含属性值)。
    ///
    /// basic_state: {label: value},value 为 int(dword/word/enum/弓箭数量)或
    /// std::string(string_ptr)。enum 转显示名;string_ptr 保留字符串;其余写 int。
    static void Save(const std::string& path, const BasicState& basic_state,
                     const std::vector<BasicFieldCfg>& basic_fields,
                     const std::vector<ItemProperty>& attrs);

private:
    /// 把 JSON 值转成 int。enum 接受显示名或 "值：名字" 或数字;其余接受数字。失败返回 nullopt。
    static std::optional<int> ToInt(const json::Value& val, const BasicFieldCfg& f);
    /// 解析非固定属性值:**不允许浮点数**,必须是整数(int 或整数形式的字符串)。
    static std::optional<float> ParseAttrValue(const json::Value& raw);
    static int Clamp(const BasicFieldCfg& f, int v);
    /// 浮点值若是整数(含负数,如 -10.0),导出为 int;否则保留 float。
    static json::Value ToJsonValue(float v);
};

}  // namespace poq
