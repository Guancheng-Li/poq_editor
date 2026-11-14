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
const std::map<uint32_t, std::string>& attr_names();
/// 属性颜色:值 -> 颜色名。
const std::map<int, std::string>& color_names();
/// 是否百分比:值 -> 显示名。
const std::map<int, std::string>& percent_names();
/// 五行:值 -> 名(基础属性用)。
const std::map<int, std::string>& element_names();
/// 是否鉴定:值 -> 名。
const std::map<int, std::string>& identified_names();

/// 弓箭/矢 类型值(弓箭数量仅在这两种类型下生效)。
constexpr int kArrowType13 = 13;
constexpr int kArrowType14 = 14;

// ---- 属性 ID <-> 显示串 ----
/// 属性 ID -> "3C：闪避率" 形式。未知 ID 也按 "XX：未知" 给出。
std::string attr_display(uint32_t attr_id);
/// 全部合法属性名选项(按 ID 升序),用于下拉框。
std::vector<std::string> attr_display_list();
/// 同 attr_display_list, 但静态缓存(UI 每帧调用用这个, 不每次新建 vector)。
const std::vector<std::string>& attr_display_list_ref();
/// 同 percent_display_list 的静态缓存版本。
const std::vector<std::string>& percent_display_list_ref();
/// "3C：闪避率" -> 0x3C。无法解析返回 nullopt。兼容半角/全角冒号、带或不带 0x 前缀。
std::optional<uint32_t> parse_attr_display(const std::string& text);
/// 显示串是否是合法属性名选项(ID 必须在 ATTR_NAMES 中)。
bool is_valid_attr_display(const std::string& text);

// ---- 属性名 <-> GAM G_Info 词条名 ----
/// 下拉框属性名(UTF-8) -> GAM G_Info 里的写法(UTF-8)。无差异时原样返回。
/// 例: 普通攻击 -> 普通攻击力, 燃烧时间 -> 失明时间, 恢复箭支数量 -> 自动恢复箭支数量。
std::string attr_g_info_name(const std::string& attrNameUtf8);
/// GAM G_Info 词条名(GBK 字节) -> 属性 ID；无法识别返回 -1。
int attr_id_from_g_info_name(const std::string& ginfoNameGbk);

// ---- 上下限成对属性 ----
/// 只有这些属性是"下限/上限"成对的(min id -> max id):
/// 五行攻击力 0x01/0x02, 有效距离 0x0C/0x0D, 普通攻击力 0x29/0x2A,
/// 金/木/水/火/土系攻击力 0x2B..0x34。
/// 其余"最大X"(最大生命 0x17、最大精气 0x18、最大负重 0x1A…)都是**独立词条**,不合并。
const std::map<int, int>& affix_pairs();
/// id 是否是某个成对属性的下限 id。
bool is_affix_pair_min_id(int id);

// ---- 颜色 <-> 显示串 ----
std::string color_display(int color);
std::vector<std::string> color_display_list();
std::optional<int> parse_color_display(const std::string& text);

// ---- 百分比 <-> 显示串 ----
std::string percent_display(int precent);
std::vector<std::string> percent_display_list();
std::optional<int> parse_percent_display(const std::string& text);

}  // namespace ItemData

}  // namespace poq
