#!/usr/bin/env python3
"""Rename poq project PascalCase functions/methods to snake_case (declarations only)."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "src"

SNAKE_OVERRIDES = {
    "Default": "default_config",
    "Char": "char_state",  # App 访问器; 避免 C++ 关键字 char
}

# Never rename (APIs / macros / external)
SKIP_NAMES = {
    "App",  # type, not func — guarded by types set too
    "FOR_EACH",
    "MB_ICONERROR",
    "MB_ICONWARNING",
    "MB_OK",
    "MB_YESNO",
    "MB_ICONQUESTION",
    "FLT_MAX",
    "INT_MAX",
    "UINT32_MAX",
    "NULL",
    "WINAPI",
    "WinMain",
    "wWinMain",
    "APIENTRY",
    "IMGUI_IMPL_API",
    "POQ_VER_BUILD",
    "POQ_VER_STRING",
    "CreateToolhelp32Snapshot",
    "Module32First",
    "Module32Next",
    "Process32First",
    "Process32Next",
    "OpenProcess",
    "CloseHandle",
    "ReadProcessMemory",
    "WriteProcessMemory",
    "VirtualAllocEx",
    "VirtualFreeEx",
    "VirtualProtectEx",
    "FlushInstructionCache",
    "CreateRemoteThread",
    "WaitForSingleObject",
    "GetExitCodeThread",
    "MultiByteToWideChar",
    "WideCharToMultiByte",
    "GetModuleHandleW",
    "LoadLibraryW",
    "SetCurrentProcessExplicitAppUserModelID",
    "ZeroMemory",
    "MAKELANGID",
    "FormatMessageA",
    "LocalFree",
    "D3D11CreateDeviceAndSwapChain",
    "CreateRenderTargetView",
    "ClearRenderTargetView",
    "ImGui_ImplWin32_EnableDpiAwareness",
    "ImGui_ImplWin32_Init",
    "ImGui_ImplWin32_Shutdown",
    "ImGui_ImplWin32_NewFrame",
    "ImGui_ImplDX11_Init",
    "ImGui_ImplDX11_Shutdown",
    "ImGui_ImplDX11_NewFrame",
    "ImGui_ImplDX11_RenderDrawData",
}

TYPE_RE = re.compile(r"\b(?:struct|class|enum(?:\s+class)?)\s+([A-Z][A-Za-z0-9_]*)\b")
# function/method declaration (one line heuristic)
DECL_RE = re.compile(
    r"^\s*(?:static\s+|inline\s+|virtual\s+|explicit\s+)*"
    r"(?:[\w:<>,\s&*]+?\s+)?([A-Z][A-Za-z0-9_]*)\s*\("
)
CPP_METHOD_RE = re.compile(r"\b([A-Z][A-Za-z0-9_]*(?:::[A-Z][A-Za-z0-9_]*)?)::([A-Z][A-Za-z0-9_]*)\s*\(")


def pascal_to_snake(name: str) -> str:
    if name in SNAKE_OVERRIDES:
        return SNAKE_OVERRIDES[name]
    s = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    s = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s)
    return s.lower()


def is_pascal_func(name: str) -> bool:
    if name in SKIP_NAMES:
        return False
    # ImGui / Dear ImGui types (ImVec2, ImGuiIO, …)
    if name.startswith("Im") and len(name) > 2 and name[2].isupper():
        return False
    if name.startswith("ImGui") or name.startswith("IMGUI"):
        return False
    if name.startswith("D3D"):
        return False
    # Macros / Win32 constants
    if "_" in name and name.upper() == name:
        return False
    return name[0].isupper()


def collect_from_text(text: str, types: set[str], funcs: set[str]) -> None:
    for m in TYPE_RE.finditer(text):
        types.add(m.group(1))
    for line in text.splitlines():
        m = DECL_RE.match(line)
        if not m:
            continue
        n = m.group(1)
        if n in types or not is_pascal_func(n):
            continue
        if line.strip().startswith("//"):
            continue
        funcs.add(n)
    for m in CPP_METHOD_RE.finditer(text):
        cls, meth = m.group(1), m.group(2)
        if meth in types or not is_pascal_func(meth):
            continue
        if cls.split("::")[-1] in types or cls in types:
            funcs.add(meth)


def main() -> int:
    types: set[str] = set()
    funcs: set[str] = set()
    for path in sorted(ROOT.rglob("*")):
        if path.suffix not in (".h", ".cpp"):
            continue
        if "imgui" in path.parts:
            continue
        text = path.read_text(encoding="utf-8")
        collect_from_text(text, types, funcs)

    funcs -= types
    funcs -= SKIP_NAMES

    renames = {}
    for name in funcs:
        snake = pascal_to_snake(name)
        if snake != name:
            renames[name] = snake

    print(f"Types: {len(types)}, rename {len(renames)} functions/methods")
    for old in sorted(renames, key=len, reverse=True)[:50]:
        print(f"  {old} -> {renames[old]}")
    if len(renames) > 50:
        print(f"  ... +{len(renames) - 50} more")

    for path in sorted(ROOT.rglob("*")):
        if path.suffix not in (".h", ".cpp"):
            continue
        text = path.read_text(encoding="utf-8")
        orig = text
        for old, new in sorted(renames.items(), key=lambda x: -len(x[0])):
            text = re.sub(rf"\b{re.escape(old)}\b", new, text)
        if text != orig:
            path.write_text(text, encoding="utf-8", newline="\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
