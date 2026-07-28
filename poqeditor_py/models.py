# -*- coding: utf-8 -*-
"""物品属性模型。对应 C# 的 ItemProperty.cs。

一条属性修改项。Address 为该属性在游戏内存中的写入地址（可手填或由读取扫描得到）。
"""

from dataclasses import dataclass


@dataclass
class ItemProperty:
    """一条属性修改项。

    Attributes:
        address: 该属性在游戏内存中的写入地址（十六进制字符串或空）。
        index: 属性代码（Index）。
        num: 数值（Num）。
        color: 颜色（Color）。
        precent: 是否百分比（Precent）。
    """

    address: str = ""
    index: int = 0
    num: int = 0
    color: int = 0
    precent: int = 0
