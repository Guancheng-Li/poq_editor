#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace poq {

/// AOB 注入目标与捕获条件(对应 Python 的 HookCfg)。
struct HookCfg {
    std::string module = "EasyRPG116.dll";
    uint32_t offset = 0x1D0D;               // 注入偏移
    std::string aob = "8B 44 86 1E 5E";     // 参考原指令字节
    uint32_t weight_offset = 0xF8;          // 重量字段偏移(捕获有效性判定)
    uint32_t weight_max = 1000;             // 重量 <= 此值才捕获
};

/// 条件字段配置:{"field": <label>, "values": [..]}(对应 Python 的 conditional)。
struct ConditionalCfg {
    std::string field;
    std::vector<int> values;
};

/// 强制字段配置:{"field": <label>, "values": [..], "value": <int>}
/// (对应 Python 的 force_when:引用字段值在 values 中时,本字段强制为 value)。
struct ForceWhenCfg {
    std::string field;
    std::vector<int> values;
    int value = 0;
};

/// 一条基础属性字段(对应 Python 的 BasicFieldCfg)。
struct BasicFieldCfg {
    std::string label;
    uint32_t offset = 0;
    std::string type = "dword";  // dword|word|string_ptr|enum
    std::optional<int> min;
    std::optional<int> max;
    bool readonly = false;
    std::map<int, std::string> enum_map;         // 值 -> 显示名(enum 类型)
    bool enum_display_value = false;             // enum 下拉框是否显示 "值：名字"
    std::optional<ConditionalCfg> conditional;   // 仅当引用字段值在 values 中才可编辑/写入
    std::optional<std::string> max_ref;          // 实际上限取自另一字段(如耐久度<=耐久度上限)
    std::optional<std::string> hint;             // 界面提示
    int codepage = 936;                          // string_ptr 的代码页(0=CP_ACP)
    std::optional<ForceWhenCfg> force_when;      // 引用字段值在 values 中时强制为 value

    /// 该字段当前是否被 force_when 强制(引用字段值在 values 中)。
    bool IsForced(const std::optional<int>& type_val) const;
    /// 把 v clamp 到 [min,max]。
    int Clamp(int v) const;
};

/// 属性链表内存布局(对应 Python 的 AttrListCfg)。
struct AttrListCfg {
    uint32_t head_offset = 0x45C;        // 头指针位于 物品基址 + head_offset
    uint32_t next_offset = 0x00;
    uint32_t prev_offset = 0x04;         // 前驱指针(双向链表)
    uint32_t sentinel_offset = 0x08;     // 有效节点哨兵(=0xFFFFFFFF)
    uint32_t node_flag_offset = 0x10;    // 启用颜色标志(=1)
    uint32_t id_offset = 0x0C;
    uint32_t color_offset = 0x14;
    uint32_t precent_offset = 0x1C;
    uint32_t value_offset = 0x20;
    uint32_t node_size = 0x28;           // 单个节点数据区大小
    uint32_t max_nodes = 30;             // 遍历/写入上限,防止坏链表死循环
    // 静态节点池:原版修改器在游戏静态零缓冲顶部按 static_slot 一个槽向下分配新节点。
    uint32_t static_base = 0x00901000;   // 池区起始地址
    uint32_t static_size = 0x1000;       // 池区大小
    uint32_t static_slot = 0x30;         // 每个槽位间隔字节
};

/// 一种版本下的全部偏移(对应 Python 的 ModeCfg)。
struct ModeCfg {
    std::string name = "默认";
    HookCfg hook;
    int item_base_adjust = 0;  // 物品基址 = cur_item 值 + 此调整(默认 0)
    std::vector<BasicFieldCfg> basic_fields;
    AttrListCfg attr_list;
};

/// 偏移配置根对象:多模式 + 当前模式(对应 Python 的 OffsetsConfig)。
///
/// 单文件版:配置内置在代码里(不读 offsets.json),启动直接 Default()。
class OffsetsConfig {
public:
    std::map<std::string, ModeCfg> modes;
    std::string current_mode = "mode0";

    /// 当前模式(找不到时退回 mode0 / 默认值)。
    const ModeCfg& Current() const;
    void SetCurrent(const std::string& id) {
        if (modes.count(id)) current_mode = id;
    }

    /// 默认模式(来自对齐的 Cheat Engine 表,与 Python default_mode 一致)。
    static ModeCfg DefaultMode();
    static OffsetsConfig Default();

    /// 类型映射文本("值:名字" 每行一条),与 Python TYPE_MAPPING_TEXT 一致。
    static const std::string& TypeMappingText();
    /// 把 "值:名字" 多行文本解析成 {值: 名字}(忽略空行/格式错误行)。
    static std::map<int, std::string> ParseTypeMapping(const std::string& text);

    /// 解析十六进制/十进制数字字符串(支持 "0x901FF0" / "901FF0")。
    static long long ParseLong(const std::string& s);
};

}  // namespace poq
