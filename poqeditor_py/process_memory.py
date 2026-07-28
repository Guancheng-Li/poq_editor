# -*- coding: utf-8 -*-
"""进程内存读写封装。对应 C# 的 Native/NativeMethods.cs + ProcessMemory.cs。

负责枚举进程、打开进程、按地址读写 DWORD/字节数组。仅在 Windows 上可用；
非 Windows 平台导入本模块不会崩溃（实际功能不可用），但调用任何方法会抛异常。
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from dataclasses import dataclass
from typing import List, Optional

PROCESS_ALL_ACCESS = 0x1F0FFF
TH32CS_SNAPPROCESS = 0x00000002
PAGE_EXECUTE_READWRITE = 0x40


# ---- Win32 API 绑定（仅 Windows 可用）----
_is_windows = False
_kernel32 = None
try:
    import sys
    if sys.platform == "win32":
        # use_last_error=True 让 ctypes.get_last_error() 可在调用后可靠读取
        _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        _is_windows = True
except (AttributeError, OSError):
    _kernel32 = None
    _is_windows = False


@dataclass
class ProcessInfo:
    """一个进程的信息。对应 C# 的 ProcessInfo。"""

    pid: int
    name: str

    def __str__(self) -> str:
        return f"{self.name}  (PID={self.pid})"


# ---- PROCESSENTRY32W 结构 ----
class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


def _bind():
    """配置 Win32 函数签名（仅在 Windows 上调用）。返回 None 表示不可用。"""
    if not _is_windows or _kernel32 is None:
        return None

    _kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _kernel32.OpenProcess.restype = wintypes.HANDLE

    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel32.CloseHandle.restype = wintypes.BOOL

    _kernel32.ReadProcessMemory.argtypes = [
        wintypes.HANDLE, wintypes.LPCVOID, wintypes.LPVOID,
        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
    ]
    _kernel32.ReadProcessMemory.restype = wintypes.BOOL

    _kernel32.WriteProcessMemory.argtypes = [
        wintypes.HANDLE, wintypes.LPVOID, wintypes.LPCVOID,
        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
    ]
    _kernel32.WriteProcessMemory.restype = wintypes.BOOL

    _kernel32.VirtualProtectEx.argtypes = [
        wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t,
        wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
    ]
    _kernel32.VirtualProtectEx.restype = wintypes.BOOL

    _kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    _kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE

    _kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    _kernel32.Process32FirstW.restype = wintypes.BOOL

    _kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    _kernel32.Process32NextW.restype = wintypes.BOOL

    _kernel32.FormatMessageW.argtypes = [
        wintypes.DWORD, wintypes.LPCVOID, wintypes.DWORD, wintypes.DWORD,
        wintypes.LPWSTR, wintypes.DWORD, ctypes.c_void_p,
    ]
    _kernel32.FormatMessageW.restype = wintypes.DWORD


_bind()


def format_message(error_code: int) -> str:
    """简易错误描述（对应 C# NativeMethods.FormatMessage）。"""
    if not _is_windows or _kernel32 is None:
        return f"error {error_code}"
    try:
        FORMAT_MESSAGE_FROM_SYSTEM = 0x00001000
        buf = ctypes.create_unicode_buffer(512)
        n = _kernel32.FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM, None, error_code, 0, buf, 512, None
        )
        if n > 0:
            return buf.value.strip()
    except Exception:
        pass
    return f"error {error_code}"


class ProcessMemory:
    """进程内存读写封装。对应 C# 的 ProcessMemory。"""

    def __init__(self) -> None:
        self.handle: int = 0  # 0 表示未打开

    @property
    def is_open(self) -> bool:
        """进程是否已打开。"""
        return self.handle != 0

    # ---- 枚举进程 ----
    @staticmethod
    def enumerate_processes(name_filter: Optional[str] = None) -> List[ProcessInfo]:
        """枚举所有进程，可选按名字过滤（大小写不敏感包含匹配）。"""
        result: List[ProcessInfo] = []
        if not _is_windows or _kernel32 is None:
            return result
        snap = _kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        if not snap:
            return result
        try:
            pe = PROCESSENTRY32W()
            pe.dwSize = ctypes.sizeof(PROCESSENTRY32W)
            if _kernel32.Process32FirstW(snap, ctypes.byref(pe)):
                while True:
                    name = pe.szExeFile
                    if not name_filter or name_filter.lower() in name.lower():
                        result.append(ProcessInfo(pid=pe.th32ProcessID, name=name))
                    if not _kernel32.Process32NextW(snap, ctypes.byref(pe)):
                        break
        finally:
            _kernel32.CloseHandle(snap)
        return result

    # ---- 打开/关闭 ----
    def open(self, pid: int, access: int = PROCESS_ALL_ACCESS) -> bool:
        """打开进程。access 默认 PROCESS_ALL_ACCESS。"""
        self.close()
        if not _is_windows or _kernel32 is None:
            return False
        self.handle = _kernel32.OpenProcess(access, False, pid)
        return self.is_open

    def close(self) -> None:
        """关闭已打开的进程句柄。"""
        if self.is_open:
            _kernel32.CloseHandle(self.handle)
            self.handle = 0

    # ---- 读写 ----
    def read(self, base_address: int, n_size: int) -> bytes:
        """从 base_address 读取 n_size 字节。"""
        if not self.is_open:
            raise RuntimeError("进程未打开")
        buf = (ctypes.c_ubyte * n_size)()
        bytes_read = ctypes.c_size_t(0)
        ok = _kernel32.ReadProcessMemory(
            self.handle, base_address, buf, n_size, ctypes.byref(bytes_read)
        )
        if not ok:
            err = ctypes.get_last_error()
            raise RuntimeError(
                f"ReadProcessMemory 失败 @ {base_address:08X}: {format_message(err)}"
            )
        return bytes(buf)

    def read_uint32(self, base_address: int) -> int:
        """读取一个 4 字节 DWORD（无符号）。"""
        data = self.read(base_address, 4)
        return int.from_bytes(data, byteorder="little", signed=False)

    def read_int32(self, base_address: int) -> int:
        """读取一个 4 字节 int（有符号）。"""
        data = self.read(base_address, 4)
        return int.from_bytes(data, byteorder="little", signed=True)

    def write(self, base_address: int, data: bytes) -> None:
        """写入字节数组。写入前尝试把目标页改为可写，部分游戏内存只读。"""
        if not self.is_open:
            raise RuntimeError("进程未打开")
        n = len(data)
        buf = (ctypes.c_ubyte * n)(*data)
        old_protect = wintypes.DWORD(0)
        _kernel32.VirtualProtectEx(
            self.handle, base_address, n, PAGE_EXECUTE_READWRITE, ctypes.byref(old_protect)
        )
        bytes_written = ctypes.c_size_t(0)
        ok = _kernel32.WriteProcessMemory(
            self.handle, base_address, buf, n, ctypes.byref(bytes_written)
        )
        if not ok:
            err = ctypes.get_last_error()
            raise RuntimeError(
                f"WriteProcessMemory 失败 @ {base_address:08X}: {format_message(err)}"
            )

    def write_uint32(self, base_address: int, value: int) -> None:
        """写入一个 4 字节 DWORD。"""
        self.write(base_address, value.to_bytes(4, byteorder="little", signed=False))

    # ---- 工具 ----
    @staticmethod
    def parse_address(s: str) -> int:
        """十六进制字符串 -> 整数地址。支持 "0x901FF0" / "901FF0"。"""
        s = s.strip()
        if s.lower().startswith("0x"):
            s = s[2:]
        return int(s, 16)
