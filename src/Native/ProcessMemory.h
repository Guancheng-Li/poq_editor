#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace poq {

/// One enumerated process entry. Mirrors the C# `ProcessInfo` class.
struct ProcessInfo {
    unsigned int pid = 0;
    std::string name;  // e.g. "POQ.EXE"

    /// Display string for the process combo box: "POQ.EXE  (PID=1234)".
    std::string Display() const {
        return name + "  (PID=" + std::to_string(pid) + ")";
    }
};

/// One enumerated module of a target process (used to resolve the game image base).
struct ModuleInfo {
    std::string name;
    uintptr_t base = 0;
    uint32_t size = 0;
};

/// One committed, readable memory region of the target process.
struct RegionInfo {
    uintptr_t base = 0;
    size_t size = 0;
};

/// Win32 process-memory read/write wrapper. Mirrors the Python `ProcessMemory` class.
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

    /// Enumerate the modules loaded in `pid` (SNAPMODULE|SNAPMODULE32 so a 64-bit
    /// caller can see the 32-bit target's modules).
    static std::vector<ModuleInfo> EnumerateModules(unsigned int pid);

    /// Find a module's base address by name (case-insensitive basename match).
    /// Returns nullopt if not found.
    static std::optional<uintptr_t> FindModuleBase(unsigned int pid, const std::string& name);

    /// Find a module's full path (W->UTF-8) by name (case-insensitive basename
    /// match). Returns nullopt if not found.
    static std::optional<std::string> FindModulePath(unsigned int pid, const std::string& name);

    /// Open the process with the given access mask. Returns true on success.
    /// Closes any previously-opened handle first.
    bool Open(unsigned int pid, unsigned int access = kProcessAllAccess);

    /// Close the current handle, if any.
    void Close();

    /// Enumerate the target process's committed memory regions whose protection
    /// allows reading (exact Protect in {1,2,4,0x20,0x40,0x80}, matching
    /// memlib.py iter_regions). Scans 0..0x7FFFFFFF (32-bit target space).
    std::vector<RegionInfo> EnumerateRegions();

    /// Read `n_size` bytes from `base_address`. Throws std::runtime_error on failure.
    std::vector<unsigned char> Read(uintptr_t base_address, int n_size);

    /// Read a 4-byte little-endian DWORD.
    unsigned int ReadUInt32(uintptr_t base_address);

    /// Read a 4-byte signed int.
    int ReadInt32(uintptr_t base_address) { return static_cast<int>(ReadUInt32(base_address)); }

    /// Read a 2-byte little-endian WORD.
    uint16_t ReadWord(uintptr_t base_address);

    /// Read a 4-byte little-endian float.
    float ReadFloat(uintptr_t base_address);

    /// Read a NUL-terminated string at `base_address` (up to `max_len` bytes) and
    /// decode it (gbk / utf-8 / latin-1 fallback). Returns UTF-8. `codepage` 0 or
    /// 936 means CP_ACP (GBK on Chinese systems). Returns nullopt on failure or
    /// if the decoded text is empty/garbage (caller shows "未知物品").
    std::optional<std::string> ReadStringGbk(uintptr_t base_address, int max_len, int codepage);

    /// Write `data` to `base_address`. Attempts to flip the target page to
    /// PAGE_EXECUTE_READWRITE first, since some game memory is read-only.
    void Write(uintptr_t base_address, const std::vector<unsigned char>& data);

    /// Write a 4-byte DWORD.
    void WriteUInt32(uintptr_t base_address, unsigned int value);

    /// Write a 2-byte WORD.
    void WriteWord(uintptr_t base_address, uint16_t value);

    /// Write a 4-byte float.
    void WriteFloat(uintptr_t base_address, float value);

    /// Parse the PE OptionalHeader ImageBase of the module loaded at
    /// `module_base` inside the target process (PE32/PE32+). nullopt on failure.
    std::optional<uintptr_t> ReadPeImageBase(uintptr_t module_base);

    /// Allocate `size` bytes of executable memory in the target process. When
    /// `near` is nonzero, first try to allocate near it (useful for a 32-bit
    /// rel32 jmp within ±2GB). Throws std::runtime_error on failure.
    uintptr_t Alloc(size_t size, uintptr_t near_addr = 0);

    /// Free memory previously allocated with Alloc (MEM_RELEASE).
    void Free(uintptr_t address);

    /// Run `stub` in a remote thread and wait up to `wait_ms` milliseconds. On
    /// completion, read the int32 at `result_slot` as the return value. Returns
    /// nullopt on timeout (the thread is terminated) or if the read fails.
    std::optional<int> CallRemoteThread(uintptr_t stub, int wait_ms, uintptr_t result_slot);

    /// Look up an exported function address (PE32) inside a target-process module.
    /// Returns 0 if the module/export is missing or the entry is a forwarder.
    uintptr_t FindExport(uintptr_t module_base, const std::string& name);

    /// Call the CRT `malloc` inside the target process via a remote-thread stub,
    /// returning the allocated address (0 on failure). Depends on the target
    /// having loaded a CRT with a usable `malloc` export.
    uintptr_t AllocMalloc(unsigned int pid, uint32_t size);

    /// Parse a hex address string ("0x901FF0" or "901FF0") into an address.
    static uintptr_t ParseAddress(const std::string& s);

private:
    void* handle_ = nullptr;  // HANDLE
};

}  // namespace poq
