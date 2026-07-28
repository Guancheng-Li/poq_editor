# -*- coding: utf-8 -*-
"""偏移配置：全部内存地址/偏移都可在此文件调整，无需重新编译。

对应 C# 的 OffsetsConfig.cs。配置以 offsets.json (UTF-8) 形式存储，
schema 镜像 C# OffsetsConfig 类。
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from typing import List


@dataclass
class BasicField:
    """物品基础属性字段：标签 + 相对物品基址的偏移。"""

    label: str = ""
    offset: int = 0


@dataclass
class PropertyListCfg:
    """属性列表（46 条属性模板）的内存布局。"""

    base_offset_from_item: int = 0x404
    count: int = 46
    stride: int = 0x24
    index_offset: int = 0x00
    num_offset: int = 0x04
    color_offset: int = 0x14
    precent_offset: int = 0x20
    scan_array: bool = True


@dataclass
class OffsetsConfig:
    """偏移配置根对象。"""

    anchor_address: str = "0x901FF0"
    item_base_adjust: int = -240
    process_access: str = "0x1F0FFF"
    basic_fields: List[BasicField] = field(default_factory=list)
    property_list: PropertyListCfg = field(default_factory=PropertyListCfg)
    template_ini: str = "EquMod.ini"

    # ---- 派生值（对应 C# 的 [JsonIgnore] 属性）----
    @property
    def anchor_addr_value(self) -> int:
        """锚点地址的整数值。"""
        return parse_long(self.anchor_address)

    @property
    def process_access_value(self) -> int:
        """进程访问权限的整数值。"""
        return parse_long(self.process_access)

    # ---- 读写 JSON ----
    @staticmethod
    def load(path: str) -> "OffsetsConfig":
        """从 JSON 文件加载配置；文件不存在时返回默认值。"""
        if not os.path.exists(path):
            return OffsetsConfig.default()
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if data is None:
            return OffsetsConfig.default()
        return OffsetsConfig.from_dict(data)

    def save(self, path: str) -> None:
        """把配置以 UTF-8 缩进 JSON 写入文件。"""
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.to_dict(), f, indent=2, ensure_ascii=False)

    @staticmethod
    def from_dict(data: dict) -> "OffsetsConfig":
        """从字典构造配置（兼容 C# 风格的 PascalCase 键名）。"""
        pl = data.get("PropertyList") or data.get("property_list") or {}
        bf_list = data.get("BasicFields") or data.get("basic_fields") or []
        return OffsetsConfig(
            anchor_address=data.get("AnchorAddress", data.get("anchor_address", "0x901FF0")),
            item_base_adjust=int(data.get("ItemBaseAdjust", data.get("item_base_adjust", -240))),
            process_access=data.get("ProcessAccess", data.get("process_access", "0x1F0FFF")),
            basic_fields=[
                BasicField(
                    label=b.get("Label", b.get("label", "")),
                    offset=int(b.get("Offset", b.get("offset", 0))),
                )
                for b in bf_list
            ],
            property_list=PropertyListCfg(
                base_offset_from_item=int(pl.get("BaseOffsetFromItem", pl.get("base_offset_from_item", 0x404))),
                count=int(pl.get("Count", pl.get("count", 46))),
                stride=int(pl.get("Stride", pl.get("stride", 0x24))),
                index_offset=int(pl.get("IndexOffset", pl.get("index_offset", 0x00))),
                num_offset=int(pl.get("NumOffset", pl.get("num_offset", 0x04))),
                color_offset=int(pl.get("ColorOffset", pl.get("color_offset", 0x14))),
                precent_offset=int(pl.get("PrecentOffset", pl.get("precent_offset", 0x20))),
                scan_array=bool(pl.get("ScanArray", pl.get("scan_array", True))),
            ),
            template_ini=data.get("TemplateIni", data.get("template_ini", "EquMod.ini")),
        )

    def to_dict(self) -> dict:
        """序列化为与 C# offsets.json 兼容的 PascalCase 字典。"""
        return {
            "AnchorAddress": self.anchor_address,
            "ItemBaseAdjust": self.item_base_adjust,
            "ProcessAccess": self.process_access,
            "BasicFields": [
                {"Label": b.label, "Offset": b.offset} for b in self.basic_fields
            ],
            "PropertyList": {
                "BaseOffsetFromItem": self.property_list.base_offset_from_item,
                "Count": self.property_list.count,
                "Stride": self.property_list.stride,
                "IndexOffset": self.property_list.index_offset,
                "NumOffset": self.property_list.num_offset,
                "ColorOffset": self.property_list.color_offset,
                "PrecentOffset": self.property_list.precent_offset,
                "ScanArray": self.property_list.scan_array,
            },
            "TemplateIni": self.template_ini,
        }

    @staticmethod
    def default() -> "OffsetsConfig":
        """默认偏移（来自对原版 poq_hack.exe 的逆向，可能需要针对实际 POQ.EXE v1.30 校准）。"""
        return OffsetsConfig(
            anchor_address="0x901FF0",
            item_base_adjust=-240,
            process_access="0x1F0FFF",
            basic_fields=[
                BasicField("字段_0x00", 0x00),
                BasicField("字段_0x04", 0x04),
                BasicField("字段_0x08", 0x08),
                BasicField("字段_0x0C", 0x0C),
                BasicField("字段_0x10", 0x10),
                BasicField("字段_0x14", 0x14),
                BasicField("字段_0x20", 0x20),
                BasicField("字段_0xA0", 0xA0),
                BasicField("字段_0xCC", 0xCC),
                BasicField("字段_0xF8", 0xF8),
                BasicField("字段_0x110", 0x110),
                BasicField("字段_0x268", 0x268),
                BasicField("字段_0x404", 0x404),
            ],
            property_list=PropertyListCfg(
                base_offset_from_item=0x404,
                count=46,
                stride=0x24,
                index_offset=0x00,
                num_offset=0x04,
                color_offset=0x14,
                precent_offset=0x20,
                scan_array=True,
            ),
            template_ini="EquMod.ini",
        )


def parse_long(s: str) -> int:
    """解析十六进制字符串（支持 "0x901FF0" / "901FF0"），返回整数。"""
    s = s.strip()
    if s.lower().startswith("0x"):
        s = s[2:]
    return int(s, 16)
