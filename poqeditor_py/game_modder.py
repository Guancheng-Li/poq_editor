# -*- coding: utf-8 -*-
"""秦殇物品内存修改核心逻辑。对应 C# 的 Core/GameModder.cs。

所有地址/偏移来自 OffsetsConfig，可调整。算法（逆向自原版 poq_hack.exe）：
  1) OpenProcess(PROCESS_ALL_ACCESS, pid)
  2) 读 [AnchorAddress] 得到手持物品指针 P
  3) 物品基址 = P + ItemBaseAdjust
  4) 读/写物品各字段（基础属性 + 46 条属性槽）
"""

from __future__ import annotations

from typing import Dict, List

from models import ItemProperty
from offsets_config import OffsetsConfig
from process_memory import ProcessMemory


class GameModder:
    """秦殇物品内存修改核心。"""

    def __init__(self, config: OffsetsConfig) -> None:
        self.config = config
        self.mem = ProcessMemory()

    def initialize(self, pid: int) -> int:
        """初始化：打开进程并验证锚点可读。成功返回手持指针。

        Raises:
            RuntimeError: OpenProcess 失败或读取手持指针为 0。
        """
        if not self.mem.open(pid, self.config.process_access_value):
            raise RuntimeError("OpenProcess 失败（请以管理员身份运行）")
        # 验证锚点可读
        p = self.mem.read_uint32(self.config.anchor_addr_value)
        if p == 0:
            raise RuntimeError("读取手持指针为 0，请先在游戏中拾起一件物品")
        return p

    def get_held_item_pointer(self) -> int:
        """当前手持物品指针 [AnchorAddress]。"""
        return self.mem.read_uint32(self.config.anchor_addr_value)

    def get_item_base(self) -> int:
        """物品基址 = 手持指针 + ItemBaseAdjust。"""
        return self.get_held_item_pointer() + self.config.item_base_adjust

    def read_basic_fields(self, item_base: int) -> Dict[str, int]:
        """读取所有基础属性字段。返回 {标签: 值}。"""
        result: Dict[str, int] = {}
        for f in self.config.basic_fields:
            result[f.label] = self.mem.read_uint32(item_base + f.offset)
        return result

    def write_basic_fields(self, item_base: int, values: Dict[str, int]) -> None:
        """写入基础属性字段。values 的键为标签。"""
        for f in self.config.basic_fields:
            if f.label in values:
                self.mem.write_uint32(item_base + f.offset, values[f.label])

    def read_properties(self, item_base: int) -> List[ItemProperty]:
        """按配置的属性数组布局扫描当前物品的属性槽，返回带地址的属性列表。

        越界或不可读则停止扫描（对应 C# 的 try/catch break）。
        """
        result: List[ItemProperty] = []
        pl = self.config.property_list
        if not pl.scan_array:
            return result
        base_addr = item_base + pl.base_offset_from_item
        for i in range(pl.count):
            slot = base_addr + i * pl.stride
            try:
                idx = self.mem.read_int32(slot + pl.index_offset)
                num = self.mem.read_int32(slot + pl.num_offset)
                color = self.mem.read_int32(slot + pl.color_offset)
                prec = self.mem.read_int32(slot + pl.precent_offset)
                result.append(ItemProperty(
                    address=f"{slot:08X}",
                    index=idx,
                    num=num,
                    color=color,
                    precent=prec,
                ))
            except Exception:
                # 越界或不可读则停止扫描
                break
        return result

    def write_properties(self, props: List[ItemProperty]) -> None:
        """把网格中的属性写回各自地址（地址取自每行的 Address 列）。"""
        pl = self.config.property_list
        for p in props:
            if not p.address or not p.address.strip():
                continue
            addr = ProcessMemory.parse_address(p.address)
            self.mem.write_uint32(addr + pl.index_offset, p.index & 0xFFFFFFFF)
            self.mem.write_uint32(addr + pl.num_offset, p.num & 0xFFFFFFFF)
            self.mem.write_uint32(addr + pl.color_offset, p.color & 0xFFFFFFFF)
            self.mem.write_uint32(addr + pl.precent_offset, p.precent & 0xFFFFFFFF)
