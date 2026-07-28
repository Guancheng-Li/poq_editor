#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <QString>

namespace poq {

/// One enumerated process entry. Mirrors the C# `ProcessInfo` class.
struct ProcessInfo {
    unsigned int pid = 0;
    std::string name;  // e.g. "POQ.EXE"

    /// Display string for the process combo box: "POQ.EXE  (PID=1234)".
    QString Display() const {
        return QString::fromUtf8(name.c_str()) + QString("  (PID=%1)").arg(pid);
    }
};

/// Win32 process-memory read/write wrapper. Mirrors the C# `ProcessMemory` class.
/// All addresses are 64-bit-capable (`uintptr_t`) so a 64-bit editor can poke a
/// 32-bit game process.
class ProcessMemory {
public:
    static constexpr unsigned int kProcessAllAccess = 0x1F0FFF;

    ProcessMemory() = default;
    ~ProcessMemory();

    ProcessMemory(const ProcessMemory&) = delete;
    ProcessMemory& operator=(const ProcessMemory&) = delete;

    /// Whether a process handle is currently open.
    bool IsOpen() const { return handle_ != 0; }

    /// Enumerate all processes, optionally filtered by a case-insensitive name
    /// substring.
    static std::vector<ProcessInfo> EnumerateProcesses(const std::string& name_filter = "");

    /// Open the process with the given access mask. Returns true on success.
    /// Closes any previously-opened handle first.
    bool Open(unsigned int pid, unsigned int access = kProcessAllAccess);

    /// Close the current handle, if any.
    void Close();

    /// Read `n_size` bytes from `base_address`. Throws std::runtime_error on
    /// failure (matching the C# behavior).
    std::vector<unsigned char> Read(uintptr_t base_address, int n_size);

    /// Read a 4-byte little-endian DWORD.
    unsigned int ReadUInt32(uintptr_t base_address);

    /// Read a 4-byte signed int.
    int ReadInt32(uintptr_t base_address) { return static_cast<int>(ReadUInt32(base_address)); }

    /// Write `data` to `base_address`. Attempts to flip the target page to
    /// PAGE_EXECUTE_READWRITE first, since some game memory is read-only.
    void Write(uintptr_t base_address, const std::vector<unsigned char>& data);

    /// Write a 4-byte DWORD.
    void WriteUInt32(uintptr_t base_address, unsigned int value);

    /// Parse a hex address string ("0x901FF0" or "901FF0") into an address.
    static uintptr_t ParseAddress(const std::string& s);
    static uintptr_t ParseAddress(const QString& s) {
        return ParseAddress(s.toStdString());
    }

private:
    void* handle_ = nullptr;  // HANDLE
};

}  // namespace poq
