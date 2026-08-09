#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace poq {

/// 物品属性静态查表数据与显示串互转工具(对应 Python 的 item_data.py)。
///
/// 所有映射提取自对齐的 Cheat Engine 表 DropDownList。属性名在界面与 JSON 中的
/// 规范显示格式为 "十六进制：名字"(两位大写十六进制 + 全角冒号 + 名字),例如
/// "3C：闪避率"。所有字符串均为 UTF-8。
namespace ItemData {

/// 非固定属性 ID -> 中文名(0x00..0x62,共 99 项)。
const std::map<uint32_t, std::string>& AttrNames();
/// 属性颜色:值 -> 颜色名。
const std::map<int, std::string>& ColorNames();
/// 是否百分比:值 -> 显示名。
const std::map<int, std::string>& PercentNames();
/// 五行:值 -> 名(基础属性用)。
const std::map<int, std::string>& ElementNames();
/// 是否鉴定:值 -> 名。
const std::map<int, std::string>& IdentifiedNames();

/// 弓箭/矢 类型值(弓箭数量仅在这两种类型下生效)。
constexpr int kArrowType13 = 13;
constexpr int kArrowType14 = 14;

// ---- 属性 ID <-> 显示串 ----
/// 属性 ID -> "3C：闪避率" 形式。未知 ID 也按 "XX：未知" 给出。
std::string AttrDisplay(uint32_t attr_id);
/// 全部合法属性名选项(按 ID 升序),用于下拉框。
std::vector<std::string> AttrDisplayList();
/// "3C：闪避率" -> 0x3C。无法解析返回 nullopt。兼容半角/全角冒号、带或不带 0x 前缀。
std::optional<uint32_t> ParseAttrDisplay(const std::string& text);
/// 显示串是否是合法属性名选项(ID 必须在 ATTR_NAMES 中)。
bool IsValidAttrDisplay(const std::string& text);

// ---- 颜色 <-> 显示串 ----
std::string ColorDisplay(int color);
std::vector<std::string> ColorDisplayList();
std::optional<int> ParseColorDisplay(const std::string& text);

// ---- 百分比 <-> 显示串 ----
std::string PercentDisplay(int precent);
std::vector<std::string> PercentDisplayList();
std::optional<int> ParsePercentDisplay(const std::string& text);

}  // namespace ItemData

}  // namespace poq
