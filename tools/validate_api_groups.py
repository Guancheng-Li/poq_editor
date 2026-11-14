#!/usr/bin/env python3
"""Ensure src/UI/_api_groups.txt mentions match Api*.h snake_case declarations."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
API_DIR = ROOT / "src" / "UI"
GROUPS = API_DIR / "_api_groups.txt"

DECL = re.compile(r"\b([a-z][a-z0-9_]*)\s*\(")


def collect_api_decls() -> set[str]:
    names: set[str] = set()
    for path in sorted(API_DIR.glob("Api*.h")):
        text = path.read_text(encoding="utf-8")
        for m in DECL.finditer(text):
            n = m.group(1)
            if n in ("if", "for", "while", "switch", "catch", "return"):
                continue
            names.add(n)
    return names


def collect_index_names() -> set[str]:
    text = GROUPS.read_text(encoding="utf-8")
    names: set[str] = set()
    for line in text.splitlines():
        line = line.split("//", 1)[0].strip()
        if not line or line.startswith("----"):
            continue
        for part in line.split(","):
            part = part.strip()
            if part and re.match(r"^[a-z][a-z0-9_]*$", part):
                names.add(part)
    return names


def main() -> int:
    if not GROUPS.is_file():
        print(f"missing {GROUPS}")
        return 1
    api = collect_api_decls()
    idx = collect_index_names()
    missing_in_index = sorted(api - idx)
    stale_in_index = sorted(idx - api)
    if missing_in_index:
        print("Api*.h 未写入 _api_groups.txt:")
        for n in missing_in_index:
            print(f"  {n}")
    if stale_in_index:
        print("_api_groups.txt 多余条目( Api*.h 中无声明 ):")
        for n in stale_in_index:
            print(f"  {n}")
    if missing_in_index or stale_in_index:
        return 1
    print(f"validate_api_groups: OK ({len(api)} symbols)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
