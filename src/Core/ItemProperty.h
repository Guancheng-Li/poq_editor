#pragma once

#include <cstdint>

namespace poq {

/// 一条非固定属性修改项。对应 Python 的 models.py / C# 的 ItemProperty.cs。
///
/// node_addr 为该属性节点在游戏内存中的地址（读取时由链表遍历得到，保存时用于
/// 复用；手动新增的行 node_addr=0 表示尚无对应内存节点）。
struct ItemProperty {
    uintptr_t node_addr = 0;  // 节点内存地址（0 = 无对应节点，保存时需分配）
    uint32_t attr_id = 0;     // 属性名 ID（0x00..0x62）
    uint32_t color = 0;       // 颜色（0..4）
    uint32_t active = 1;      // node_flag@0x10: 1=激活(存档[C..]) 0=隐藏/未激活(存档[H..])
    uint32_t precent = 0;     // 是否百分比（0/1）
    float value = 0.0f;       // 属性值（Float）

    ItemProperty() = default;
    ItemProperty(uintptr_t node_addr_value, uint32_t attr_id_value, uint32_t color_value,
                 uint32_t active_value, uint32_t precent_value, float value_value)
        : node_addr(node_addr_value),
          attr_id(attr_id_value),
          color(color_value),
          active(active_value),
          precent(precent_value),
          value(value_value) {}
};

}  // namespace poq
