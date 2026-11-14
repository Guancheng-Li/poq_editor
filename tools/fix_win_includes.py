#!/usr/bin/env python3
"""After clang-format: keep <windows.h> before Win32 headers that require it.

clang-format IncludeCategories usually suffice; this fixes edge cases (e.g. split
blocks or headers added later). Only touches #include lines; idempotent.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "src"

WINDOWS = "windows.h"

# Must not appear before <windows.h> (MSDN: include windows.h first).
AFTER_WINDOWS = frozenset(
    {
        "commdlg.h",
        "shellapi.h",
        "shlobj.h",
        "shobjidl.h",
        "tlhelp32.h",
        "psapi.h",
        "wininet.h",
        "winuser.h",
    }
)

INC_RE = re.compile(r'^(\s*#\s*include\s+[<"])([^">]+)([">].*)$')


def header_name(path_part: str) -> str:
    return path_part.strip().replace("\\", "/").split("/")[-1].lower()


def fix_group(lines: list[str]) -> list[str]:
    if not lines:
        return lines
    parsed: list[tuple[str, str, str, str]] = []
    for line in lines:
        m = INC_RE.match(line)
        if not m:
            parsed.append((line, "", "", ""))
            continue
        parsed.append((line, m.group(1), m.group(2), m.group(3)))

    names = [header_name(p[2]) for p in parsed if p[1]]
    needs_win = WINDOWS in names or any(n in AFTER_WINDOWS for n in names)
    if not needs_win:
        return lines

    win_idx = next((i for i, p in enumerate(parsed) if p[1] and header_name(p[2]) == WINDOWS), None)
    first_dep = next(
        (i for i, p in enumerate(parsed) if p[1] and header_name(p[2]) in AFTER_WINDOWS), None
    )
    if first_dep is None:
        return lines
    if win_idx is not None and win_idx < first_dep:
        return lines

    # Build or move windows.h line immediately before first dependent.
    if win_idx is None:
        # Match bracket style of first dependent.
        dep = parsed[first_dep]
        if "<" in dep[1]:
            win_line = f"{dep[1]}{WINDOWS}>"
        else:
            win_line = f'{dep[1]}{WINDOWS}{dep[3]}'
        new_parsed = parsed[:first_dep] + [(win_line, dep[1], WINDOWS, dep[3])] + parsed[first_dep:]
    else:
        win_entry = parsed.pop(win_idx)
        insert_at = next(
            (
                i
                for i, p in enumerate(parsed)
                if p[1] and header_name(p[2]) in AFTER_WINDOWS
            ),
            first_dep,
        )
        if insert_at > win_idx:
            insert_at -= 1
        new_parsed = parsed[:insert_at] + [win_entry] + parsed[insert_at:]

    return [p[0] if not p[1] else f"{p[1]}{p[2]}{p[3]}" for p in new_parsed]


def fix_file(text: str) -> str:
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if not INC_RE.match(line):
            out.append(line)
            i += 1
            continue
        group: list[str] = []
        while i < len(lines):
            s = lines[i]
            if INC_RE.match(s):
                group.append(s)
                i += 1
                continue
            if s.strip() == "" and i + 1 < len(lines) and INC_RE.match(lines[i + 1]):
                i += 1
                continue
            break
        fixed = fix_group([g.rstrip("\n") for g in group])
        for g in fixed:
            out.append(g + ("\n" if not g.endswith("\n") else ""))
    return "".join(out)


def _norm(s: str) -> str:
    return s.replace("\r\n", "\n").replace("\r", "\n")


def main() -> int:
    changed = 0
    skipped = 0
    for path in sorted(ROOT.rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        orig = path.read_text(encoding="utf-8")
        new = fix_file(orig)
        if _norm(new) == _norm(orig):
            continue
        try:
            path.write_text(new, encoding="utf-8", newline="\n")
        except OSError as e:
            print(f"fix_win_includes: skip {path} ({e})")
            skipped += 1
            continue
        changed += 1
    print(f"fix_win_includes: updated {changed} file(s)" + (f", skipped {skipped}" if skipped else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
