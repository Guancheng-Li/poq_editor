using System.Diagnostics;
using System.Runtime.InteropServices;
using static PoqEditor.Native.NativeMethods;

namespace PoqEditor.Native;

/// <summary>
/// 进程内存读写封装。负责枚举进程、打开进程、按地址读写 DWORD/字节数组。
/// </summary>
public sealed class ProcessMemory : IDisposable
{
    public IntPtr Handle { get; private set; } = IntPtr.Zero;
    public bool IsOpen => Handle != IntPtr.Zero;

    /// <summary>枚举所有进程，可选按名字过滤。</summary>
    public static List<ProcessInfo> EnumerateProcesses(string? nameFilter = null)
    {
        var list = new List<ProcessInfo>();
        IntPtr snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == IntPtr.Zero) return list;
        try
        {
            var pe = new PROCESSENTRY32 { dwSize = (uint)Marshal.SizeOf<PROCESSENTRY32>() };
            if (Process32First(snap, ref pe))
            {
                do
                {
                    if (string.IsNullOrEmpty(nameFilter) ||
                        pe.szExeFile.IndexOf(nameFilter, StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        list.Add(new ProcessInfo { Pid = pe.th32ProcessID, Name = pe.szExeFile });
                    }
                } while (Process32Next(snap, ref pe));
            }
        }
        finally { CloseHandle(snap); }
        return list;
    }

    /// <summary>打开进程。access 默认 PROCESS_ALL_ACCESS。</summary>
    public bool Open(uint pid, uint access = PROCESS_ALL_ACCESS)
    {
        Close();
        Handle = OpenProcess(access, false, pid);
        return IsOpen;
    }

    public void Close()
    {
        if (IsOpen) { CloseHandle(Handle); Handle = IntPtr.Zero; }
    }

    public void Dispose() => Close();

    /// <summary>从 baseAddress 读取 nSize 字节。</summary>
    public byte[] Read(IntPtr baseAddress, int nSize)
    {
        if (!IsOpen) throw new InvalidOperationException("进程未打开");
        var buf = new byte[nSize];
        if (!ReadProcessMemory(Handle, baseAddress, buf, (IntPtr)nSize, out _))
        {
            int err = Marshal.GetLastWin32Error();
            throw new InvalidOperationException($"ReadProcessMemory 失败 @ {baseAddress.ToInt64():X8}: {FormatMessage(err)}");
        }
        return buf;
    }

    /// <summary>读取一个 4 字节 DWORD。</summary>
    public uint ReadUInt32(IntPtr baseAddress) =>
        BitConverter.ToUInt32(Read(baseAddress, 4), 0);

    public int ReadInt32(IntPtr baseAddress) => (int)ReadUInt32(baseAddress);

    /// <summary>写入字节数组。</summary>
    public void Write(IntPtr baseAddress, byte[] data)
    {
        if (!IsOpen) throw new InvalidOperationException("进程未打开");
        // 写入前尝试把目标页改为可写，部分游戏内存只读
        VirtualProtectEx(Handle, baseAddress, (IntPtr)data.Length, 0x40 /*PAGE_EXECUTE_READWRITE*/, out _);
        if (!WriteProcessMemory(Handle, baseAddress, data, (IntPtr)data.Length, out _))
        {
            int err = Marshal.GetLastWin32Error();
            throw new InvalidOperationException($"WriteProcessMemory 失败 @ {baseAddress.ToInt64():X8}: {FormatMessage(err)}");
        }
    }

    public void WriteUInt32(IntPtr baseAddress, uint value) => Write(baseAddress, BitConverter.GetBytes(value));

    /// <summary>十六进制字符串 -> IntPtr。支持 "0x901FF0" / "901FF0"。</summary>
    public static IntPtr ParseAddress(string s)
    {
        s = s.Trim();
        if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) s = s[2..];
        return new IntPtr(Convert.ToInt64(s, 16));
    }
}
