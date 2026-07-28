# -*- coding: utf-8 -*-
"""读取/写入 EquMod.ini 模板（UTF-8 编码，键 IndexN/NumN/ColorN/PrecentN）。

对应 C# 的 IniTemplate.cs。
"""

from __future__ import annotations

import os
from typing import List

from models import ItemProperty


def load(path: str) -> List[ItemProperty]:
    """从 UTF-8 ini 文件载入属性模板。文件不存在时返回空列表。

    解析键 IndexN/NumN/ColorN/PrecentN（N 为整数序号），并按序号升序排列。
    """
    result: List[ItemProperty] = []
    if not os.path.exists(path):
        return result
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()

    d: dict = {}
    for line in lines:
        t = line.strip()
        if not t or t.startswith(";") or t.startswith("["):
            continue
        eq = t.find("=")
        if eq < 0:
            continue
        key = t[:eq].strip()
        val = t[eq + 1:].strip()
        d[key] = val

    count = 0
    if "Count" in d:
        try:
            count = int(d["Count"])
        except ValueError:
            count = 0

    # 收集所有出现过的序号
    nums = set()
    prefixes = ("Index", "Num", "Color", "Precent")
    for k in d.keys():
        for prefix in prefixes:
            if k.lower().startswith(prefix.lower()) and len(k) > len(prefix):
                suffix = k[len(prefix):]
                try:
                    nums.add(int(suffix))
                except ValueError:
                    pass

    ordered = sorted(nums)
    if not ordered and count > 0:
        ordered = list(range(1, count + 1))

    def get_or_default(key: str) -> str:
        # 大小写不敏感查找
        for k, v in d.items():
            if k.lower() == key.lower():
                return v
        return ""

    def try_int(s: str) -> int:
        try:
            return int(s)
        except (ValueError, TypeError):
            return 0

    for i in ordered:
        result.append(ItemProperty(
            index=try_int(get_or_default(f"Index{i}")),
            num=try_int(get_or_default(f"Num{i}")),
            color=try_int(get_or_default(f"Color{i}")),
            precent=try_int(get_or_default(f"Precent{i}")),
        ))
    return result


def save(path: str, props: List[ItemProperty]) -> None:
    """把属性列表写入 UTF-8 ini 文件（不带 BOM，CRLF 行尾，兼容原版 ini 解析）。"""
    lines: List[str] = [
        ";如不要擅自改动，会造成程序无法正常运行",
        ";EquCount 模板共有属性数",
        ";EquIndex 属性的代码",
        ";EquNum1 属性的数值",
        ";EquColor1 属性的颜色",
        ";EquPrecent 属性是否是百分比",
        "[Mod]",
        f"Count={len(props)}",
    ]
    for i, p in enumerate(props):
        n = i + 1
        lines.append(f"Index{n}={p.index}")
        lines.append(f"Num{n}={p.num}")
        lines.append(f"Color{n}={p.color}")
        lines.append(f"Precent{n}={p.precent}")
    # CRLF 行尾
    text = "\r\n".join(lines) + "\r\n"
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
