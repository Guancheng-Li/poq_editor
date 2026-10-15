#include "Native/ProcessMemory.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <windows.h>
#include <tlhelp32.h>

namespace poq {

namespace {

// Format a Win32 error code into a human-readable string.
std::string FormatWin32Error(DWORD err) {
    char* buf = nullptr;
    DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string msg = (buf && n > 0) ? std::string(buf, n)
                                     : std::string("error ") + std::to_string(err);
    if (buf) ::LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' '))
        msg.pop_back();
    return msg;
}

std::string AddrHex(uintptr_t addr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08llX", static_cast<unsigned long long>(addr));
    return buf;
}

// Trim ASCII whitespace from both ends.
std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], len,
                          nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                     nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring out(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], wlen);
    return out;
}

// Decode a byte string with the given code page into UTF-8. Nullopt on failure.
std::optional<std::string> DecodeBytes(const std::string& bytes, UINT cp) {
    if (bytes.empty()) return std::nullopt;
    int wlen = ::MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()),
                                     nullptr, 0);
    if (wlen <= 0) return std::nullopt;
    std::vector<wchar_t> wbuf(static_cast<size_t>(wlen));
    ::MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), wbuf.data(),
                          wlen);
    int ulen = ::WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), wlen, nullptr, 0, nullptr,
                                     nullptr);
    if (ulen <= 0) return std::nullopt;
    std::string out(static_cast<size_t>(ulen), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), wlen, &out[0], ulen, nullptr, nullptr);
    return out;
}

void AppendLe32(std::vector<unsigned char>& v, uint32_t x) {
    v.push_back(static_cast<unsigned char>(x & 0xFF));
    v.push_back(static_cast<unsigned char>((x >> 8) & 0xFF));
    v.push_back(static_cast<unsigned char>((x >> 16) & 0xFF));
    v.push_back(static_cast<unsigned char>((x >> 24) & 0xFF));
}

}  // namespace

ProcessMemory::~ProcessMemory() { Close(); }

// ============================================================================
// 进程 / 模块枚举
// ============================================================================

std::vector<ProcessInfo> ProcessMemory::EnumerateProcesses(const std::string& name_filter) {
    std::vector<ProcessInfo> list;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == nullptr || snap == INVALID_HANDLE_VALUE) return list;

    // 用 W 变体(与 Qt 版一致),显式转换 DWORD -> unsigned int。
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            std::string name = WideToUtf8(pe.szExeFile);
            if (name_filter.empty()) {
                list.push_back({static_cast<unsigned int>(pe.th32ProcessID), std::move(name)});
            } else if (ToLower(name).find(ToLower(name_filter)) != std::string::npos) {
                list.push_back({static_cast<unsigned int>(pe.th32ProcessID), std::move(name)});
            }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return list;
}

std::vector<ModuleInfo> ProcessMemory::EnumerateModules(unsigned int pid) {
    std::vector<ModuleInfo> list;
    // 从 64 位进程枚举 32 位(WoW64)目标进程的模块时必须带 SNAPMODULE32。
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == nullptr || snap == INVALID_HANDLE_VALUE) return list;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (::Module32FirstW(snap, &me)) {
        do {
            std::string name = WideToUtf8(me.szModule);
            list.push_back({std::move(name),
                            reinterpret_cast<uintptr_t>(me.modBaseAddr),
                            static_cast<uint32_t>(me.modBaseSize)});
        } while (::Module32NextW(snap, &me));
    }
    ::CloseHandle(snap);
    return list;
}

std::optional<uintptr_t> ProcessMemory::FindModuleBase(unsigned int pid,
                                                       const std::string& name) {
    if (name.empty()) return std::nullopt;
    std::string target = name;
    size_t slash = target.find_last_of("/\\");
    if (slash != std::string::npos) target = target.substr(slash + 1);
    target = ToLower(target);
    for (const ModuleInfo& m : EnumerateModules(pid)) {
        std::string mn = m.name;
        size_t s2 = mn.find_last_of("/\\");
        if (s2 != std::string::npos) mn = mn.substr(s2 + 1);
        if (ToLower(mn) == target) return m.base;
    }
    return std::nullopt;
}

std::optional<std::string> ProcessMemory::FindModulePath(unsigned int pid,
                                                         const std::string& name) {
    if (name.empty()) return std::nullopt;
    std::string target = name;
    size_t slash = target.find_last_of("/\\");
    if (slash != std::string::npos) target = target.substr(slash + 1);
    target = ToLower(target);
    // 从 64 位进程枚举 32 位(WoW64)目标进程的模块时必须带 SNAPMODULE32。
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == nullptr || snap == INVALID_HANDLE_VALUE) return std::nullopt;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    std::optional<std::string> out;
    if (::Module32FirstW(snap, &me)) {
        do {
            std::string mn = WideToUtf8(me.szModule);
            size_t s2 = mn.find_last_of("/\\");
            if (s2 != std::string::npos) mn = mn.substr(s2 + 1);
            if (ToLower(mn) == target) {
                out = WideToUtf8(me.szExePath);
                break;
            }
        } while (::Module32NextW(snap, &me));
    }
    ::CloseHandle(snap);
    return out;
}

// ============================================================================
// 打开/关闭
// ============================================================================

bool ProcessMemory::Open(unsigned int pid, unsigned int access) {
    Close();
    handle_ = ::OpenProcess(access, FALSE, pid);
    pid_ = handle_ != nullptr ? pid : 0;  // 记录目标 pid(IsProcessAlive 用)
    return handle_ != nullptr;
}

void ProcessMemory::Close() {
    if (handle_ != nullptr) {
        ::CloseHandle(handle_);
        handle_ = nullptr;
    }
    pid_ = 0;
}

std::vector<RegionInfo> ProcessMemory::EnumerateRegions() {
    std::vector<RegionInfo> list;
    if (!IsOpen()) return list;
    uintptr_t addr = 0;
    while (addr < 0x7FFFFFFF) {
        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T n = ::VirtualQueryEx(handle_, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi));
        if (n == 0) break;
        // Protect 精确比较(对齐 memlib.py iter_regions: 含 PAGE_NOACCESS 但读不到会跳过)
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect == 0x01 || mbi.Protect == 0x02 || mbi.Protect == 0x04 ||
             mbi.Protect == 0x20 || mbi.Protect == 0x40 || mbi.Protect == 0x80)) {
            list.push_back({reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize});
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (addr <= 0x10000) break;  // 防死循环(虚拟地址回绕)
    }
    return list;
}

// ============================================================================
// 读写
// ============================================================================

std::vector<unsigned char> ProcessMemory::Read(uintptr_t base_address, int n_size) {
    if (handle_ == nullptr) throw std::runtime_error("进程未打开");
    std::vector<unsigned char> buf(n_size);
    SIZE_T bytes_read = 0;
    BOOL ok = ::ReadProcessMemory(handle_,
                                  reinterpret_cast<LPCVOID>(base_address),
                                  buf.data(), static_cast<SIZE_T>(n_size), &bytes_read);
    if (!ok) {
        DWORD err = ::GetLastError();
        throw std::runtime_error("ReadProcessMemory 失败 @ " + AddrHex(base_address) +
                                 ": " + FormatWin32Error(err));
    }
    return buf;
}

bool ProcessMemory::ReadBuf(uintptr_t base_address, void* out, int n_size) noexcept {
    if (handle_ == nullptr || n_size <= 0) return false;
    SIZE_T bytes_read = 0;
    return ::ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(base_address), out,
                               static_cast<SIZE_T>(n_size), &bytes_read) != 0 &&
           bytes_read == static_cast<SIZE_T>(n_size);
}

unsigned int ProcessMemory::ReadUInt32(uintptr_t base_address) {
    auto data = Read(base_address, 4);
    return static_cast<unsigned int>(data[0]) |
           (static_cast<unsigned int>(data[1]) << 8) |
           (static_cast<unsigned int>(data[2]) << 16) |
           (static_cast<unsigned int>(data[3]) << 24);
}

uint16_t ProcessMemory::ReadWord(uintptr_t base_address) {
    auto data = Read(base_address, 2);
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

float ProcessMemory::ReadFloat(uintptr_t base_address) {
    auto data = Read(base_address, 4);
    uint32_t bits = static_cast<uint32_t>(data[0]) |
                    (static_cast<uint32_t>(data[1]) << 8) |
                    (static_cast<uint32_t>(data[2]) << 16) |
                    (static_cast<uint32_t>(data[3]) << 24);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

std::optional<std::string> ProcessMemory::ReadStringGbk(uintptr_t base_address, int max_len,
                                                        int codepage) {
    std::vector<unsigned char> data;
    try {
        data = Read(base_address, max_len);
    } catch (...) {
        return std::nullopt;
    }
    size_t nul = 0;
    while (nul < data.size() && data[nul] != 0) ++nul;
    if (nul == 0) return std::nullopt;
    std::string bytes(reinterpret_cast<const char*>(data.data()), nul);

    std::vector<UINT> cps;
    // 注意:本程序清单把活动代码页设成了 UTF-8,CP_ACP 会解析成 65001 而非 GBK,
    // 所以这里显式用 936(GBK),对齐 Python 的 enc="gbk"。
    if (codepage == 0 || codepage == 936)
        cps = {936, CP_UTF8, 28591};
    else
        cps = {static_cast<UINT>(codepage), CP_UTF8, 28591};
    for (UINT cp : cps) {
        auto s = DecodeBytes(bytes, cp);
        if (!s) continue;
        // 过滤明显垃圾:含过多控制字符(除常见空白)则视为无效
        bool bad = false;
        for (char c : *s)
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
                bad = true;
                break;
            }
        if (bad) continue;
        if (!Trim(*s).empty()) return s;
    }
    return std::nullopt;
}

void ProcessMemory::Write(uintptr_t base_address, const std::vector<unsigned char>& data) {
    WriteInternal(base_address, data.data(), data.size(), false);
}

void ProcessMemory::WriteCode(uintptr_t base_address, const std::vector<unsigned char>& code) {
    WriteInternal(base_address, code.data(), code.size(), true);
}

void ProcessMemory::WriteInternal(uintptr_t base_address, const unsigned char* data,
                                  size_t size, bool is_code) {
    // 计划 P1(#7): 完整检查 + 恢复页面保护 + 代码写后 flush 指令缓存。
    // 原实现漏检 VirtualProtectEx 返回值、从不恢复 old_protect、不校验 bytes_written,
    // 写机器码后也没 flush —— 目标 CPU 可能仍执行旧指令缓存。
    if (handle_ == nullptr) throw std::runtime_error("进程未打开");
    if (data == nullptr || size == 0) return;
    if (size > (size_t)INT_MAX) throw std::runtime_error("写入长度超出范围");

    DWORD old_protect = 0;
    if (!::VirtualProtectEx(handle_, reinterpret_cast<LPVOID>(base_address),
                             static_cast<SIZE_T>(size), PAGE_EXECUTE_READWRITE, &old_protect)) {
        DWORD err = ::GetLastError();
        throw std::runtime_error("VirtualProtectEx 失败 @ " + AddrHex(base_address) + ": " +
                                 FormatWin32Error(err));
    }
    // 无论成功失败都恢复原页面保护(RAII: 抛异常时也会执行)
    struct ProtectGuard {
        HANDLE h;
        LPVOID a;
        SIZE_T n;
        DWORD old;
        ~ProtectGuard() {
            DWORD ignored = 0;
            ::VirtualProtectEx(h, a, n, old, &ignored);
        }
    } guard{handle_, reinterpret_cast<LPVOID>(base_address), static_cast<SIZE_T>(size),
            old_protect};

    SIZE_T bytes_written = 0;
    BOOL ok = ::WriteProcessMemory(handle_, reinterpret_cast<LPVOID>(base_address), data,
                                   static_cast<SIZE_T>(size), &bytes_written);
    if (!ok) {
        DWORD err = ::GetLastError();
        throw std::runtime_error("WriteProcessMemory 失败 @ " + AddrHex(base_address) + ": " +
                                 FormatWin32Error(err));
    }
    // 写成功不等于写全(跨页/部分写入会静默成功)
    if (bytes_written != static_cast<SIZE_T>(size)) {
        throw std::runtime_error("WriteProcessMemory 部分写入 @ " + AddrHex(base_address) + ": " +
                                 std::to_string(bytes_written) + "/" + std::to_string(size) +
                                 " 字节");
    }
    // 写机器码: 目标 CPU 可能仍在执行旧的指令缓存, 必须刷新
    if (is_code) {
        ::FlushInstructionCache(handle_, reinterpret_cast<LPCVOID>(base_address),
                                static_cast<SIZE_T>(size));
    }
}

void ProcessMemory::WriteUInt32(uintptr_t base_address, unsigned int value) {
    unsigned char buf[4];
    buf[0] = static_cast<unsigned char>(value & 0xFF);
    buf[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
    buf[2] = static_cast<unsigned char>((value >> 16) & 0xFF);
    buf[3] = static_cast<unsigned char>((value >> 24) & 0xFF);
    Write(base_address, std::vector<unsigned char>(buf, buf + 4));
}

void ProcessMemory::WriteWord(uintptr_t base_address, uint16_t value) {
    unsigned char buf[2];
    buf[0] = static_cast<unsigned char>(value & 0xFF);
    buf[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
    Write(base_address, std::vector<unsigned char>(buf, buf + 2));
}

void ProcessMemory::WriteFloat(uintptr_t base_address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    WriteUInt32(base_address, bits);
}

// ============================================================================
// PE 解析 / 内存分配 / 导出表 / 远程 malloc
// ============================================================================

std::optional<uintptr_t> ProcessMemory::ReadPeImageBase(uintptr_t module_base) {
    if (!IsOpen()) return std::nullopt;
    try {
        uint32_t e_lfanew = ReadUInt32(module_base + 0x3C);
        auto hdr = Read(module_base + e_lfanew, 0x40);
        uint16_t magic = static_cast<uint16_t>(hdr[0x18] | (hdr[0x19] << 8));
        if (magic == 0x10B) {  // PE32
            return static_cast<uintptr_t>(ReadUInt32(module_base + e_lfanew + 0x34));
        }
        if (magic == 0x20B) {  // PE32+ (64-bit)
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(hdr[0x30 + i]) << (8 * i);
            return static_cast<uintptr_t>(v);
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

uintptr_t ProcessMemory::Alloc(size_t size, uintptr_t near_addr) {
    if (!IsOpen()) throw std::runtime_error("进程未打开");
    if (near_addr != 0) {
        // 优先在 near 附近向下扫一段空闲页(便于 32 位 rel32 jmp 落在 ±2GB 内)
        const uint32_t step = 0x10000;  // 64KB
        uint32_t addr = static_cast<uint32_t>(near_addr) & 0xFFFF0000;
        for (int i = 0; i < 512; ++i) {
            void* p = ::VirtualAllocEx(handle_, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(addr)),
                                       size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return reinterpret_cast<uintptr_t>(p);
            addr = (addr - step) & 0xFFFFFFFF;
            if (addr < 0x10000) break;
        }
    }
    void* p = ::VirtualAllocEx(handle_, nullptr, size, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
    if (!p) {
        DWORD err = ::GetLastError();
        throw std::runtime_error("VirtualAllocEx 失败: " + FormatWin32Error(err));
    }
    return reinterpret_cast<uintptr_t>(p);
}

bool ProcessMemory::IsProcessAlive() const {
    if (handle_ == nullptr || pid_ == 0) return false;
    DWORD code = 0;
    if (!::GetExitCodeProcess(handle_, &code)) return false;
    return code == STILL_ACTIVE;
}

void ProcessMemory::Free(uintptr_t address) {
    if (!IsOpen()) return;
    ::VirtualFreeEx(handle_, reinterpret_cast<LPVOID>(address), 0, MEM_RELEASE);
}

// 计划 P1/P2(#8): 超时不再直接 TerminateThread 后当作普通失败返回 ——
// 被强杀的线程可能持有游戏内部锁 / 正在 malloc / 改全局对象, 目标进程可能已损坏,
// 调用方需要知道"状态不可信", 并避免释放它可能仍在访问的 stub/buffer。
ProcessMemory::RemoteCallResult ProcessMemory::CallRemoteThreadDetailed(
    uintptr_t stub, int wait_ms, uintptr_t result_slot) {
    RemoteCallResult rr;
    if (!IsOpen()) {
        rr.status = RemoteCallStatus::kFailed;
        return rr;
    }
    HANDLE h_thread = ::CreateRemoteThread(
        handle_, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(stub), nullptr, 0, nullptr);
    if (!h_thread) {
        DWORD err = ::GetLastError();
        throw std::runtime_error("CreateRemoteThread 失败: " + FormatWin32Error(err));
    }
    DWORD r = ::WaitForSingleObject(h_thread, static_cast<DWORD>(wait_ms));
    if (r == WAIT_OBJECT_0) {
        try {
            rr.value = ReadInt32(result_slot);
            rr.status = RemoteCallStatus::kCompleted;
        } catch (const std::exception&) {
            rr.status = RemoteCallStatus::kFailed;  // 线程结束了但读不到结果槽
        }
    } else if (r == WAIT_TIMEOUT) {
        // 超时: 线程可能正持有游戏内部锁/在改全局对象 —— 强杀会让目标进程状态不可信,
        // 如实上报, 由调用方提示用户(重启游戏)并避免后续依赖一致性的操作。
        ::TerminateThread(h_thread, 0);
        rr.status = RemoteCallStatus::kTimedOutStillRunning;
    } else {
        rr.status = RemoteCallStatus::kFailed;
    }
    ::CloseHandle(h_thread);
    // 进程已退出时统一上报 kProcessExited(远程内存随进程结束自动回收, 不必再管)
    if (rr.status != RemoteCallStatus::kCompleted && !IsProcessAlive()) {
        rr.status = RemoteCallStatus::kProcessExited;
    }
    return rr;
}

std::optional<int> ProcessMemory::CallRemoteThread(uintptr_t stub, int wait_ms,
                                                   uintptr_t result_slot) {
    auto rr = CallRemoteThreadDetailed(stub, wait_ms, result_slot);
    if (!rr.ok()) return std::nullopt;
    return rr.value;
}

uintptr_t ProcessMemory::FindExport(uintptr_t module_base, const std::string& name) {
    try {
        auto mz = Read(module_base, 0x40);
        if (mz[0] != 'M' || mz[1] != 'Z') return 0;
        uint32_t e_lfanew = static_cast<uint32_t>(mz[0x3C]) |
                            (static_cast<uint32_t>(mz[0x3D]) << 8) |
                            (static_cast<uint32_t>(mz[0x3E]) << 16) |
                            (static_cast<uint32_t>(mz[0x3F]) << 24);
        auto pe = Read(module_base + e_lfanew, 0x88);
        if (pe[0] != 'P' || pe[1] != 'E') return 0;
        uint16_t magic = static_cast<uint16_t>(pe[0x18] | (pe[0x19] << 8));
        if (magic != 0x10B) return 0;  // PE32 only
        auto rd32 = [](const std::vector<unsigned char>& b, size_t off) -> uint32_t {
            return static_cast<uint32_t>(b[off]) |
                   (static_cast<uint32_t>(b[off + 1]) << 8) |
                   (static_cast<uint32_t>(b[off + 2]) << 16) |
                   (static_cast<uint32_t>(b[off + 3]) << 24);
        };
        uint32_t export_rva = rd32(pe, 0x60);
        uint32_t export_size = rd32(pe, 0x64);
        if (!export_rva) return 0;
        auto exp = Read(module_base + export_rva, 0x28);
        uint32_t number_of_names = rd32(exp, 0x18);
        uint32_t addr_of_functions = rd32(exp, 0x1C);
        uint32_t addr_of_names = rd32(exp, 0x20);
        uint32_t addr_of_ordinals = rd32(exp, 0x24);
        uint32_t max_names = std::min<uint32_t>(number_of_names, 0x4000);
        for (uint32_t i = 0; i < max_names; ++i) {
            try {
                uint32_t name_rva = ReadUInt32(module_base + addr_of_names + i * 4);
                auto nm = ReadStringGbk(module_base + name_rva, 64, 0);
                if (nm && *nm == name) {
                    uint16_t ordinal = ReadWord(module_base + addr_of_ordinals + i * 2);
                    uint32_t func_rva = ReadUInt32(module_base + addr_of_functions +
                                                   static_cast<uint32_t>(ordinal) * 4);
                    // 转发器检测:func RVA 落在导出目录内(指向名字字符串)
                    if (func_rva >= export_rva && func_rva < export_rva + export_size)
                        return 0;
                    return module_base + func_rva;
                }
            } catch (...) {
                continue;
            }
        }
    } catch (...) {
        return 0;
    }
    return 0;
}

uintptr_t ProcessMemory::AllocMalloc(unsigned int pid, uint32_t size) {
    if (!IsOpen()) return 0;
    // 1) 找 CRT 的 malloc
    uintptr_t malloc_addr = 0;
    static const char* kCrtModules[] = {
        "msvcrt.dll", "msvcr100.dll", "msvcr110.dll", "msvcr120.dll",
        "ucrtbase.dll", "api-ms-win-crt-heap-l1-1-0.dll",
    };
    for (const char* mod : kCrtModules) {
        auto base = FindModuleBase(pid, mod);
        if (!base) continue;
        malloc_addr = FindExport(*base, "malloc");
        if (!malloc_addr) malloc_addr = FindExport(*base, "_malloc_base");
        if (malloc_addr) break;
    }
    if (!malloc_addr) return 0;
    // 2) ExitThread(kernel32)
    auto kbase = FindModuleBase(pid, "kernel32.dll");
    uintptr_t exit_addr = 0;
    if (kbase) exit_addr = FindExport(*kbase, "ExitThread");
    if (!exit_addr) return 0;
    // 3) 分配 stub + 结果槽(可执行内存)
    uintptr_t stub = 0;
    try {
        stub = Alloc(0x100);
    } catch (...) {
        return 0;
    }
    uintptr_t result_addr = stub + 0x80;
    uintptr_t result = 0;
    try {
        WriteUInt32(result_addr, 0);
        std::vector<unsigned char> code;
        code.push_back(0xB8); AppendLe32(code, static_cast<uint32_t>(malloc_addr));  // mov eax, malloc
        code.push_back(0x68); AppendLe32(code, size);                                // push size
        code.push_back(0xFF); code.push_back(0xD0);                                  // call eax
        code.push_back(0x83); code.push_back(0xC4); code.push_back(0x04);            // add esp,4
        code.push_back(0xA3); AppendLe32(code, static_cast<uint32_t>(result_addr));  // mov [result],eax
        code.push_back(0xB8); AppendLe32(code, static_cast<uint32_t>(exit_addr));    // mov eax, ExitThread
        code.push_back(0x6A); code.push_back(0x00);                                  // push 0
        code.push_back(0xFF); code.push_back(0xD0);                                  // call eax
        Write(stub, code);
        HANDLE h_thread = ::CreateRemoteThread(
            handle_, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(stub), nullptr,
            0, nullptr);
        if (!h_thread) {
            Free(stub);
            return 0;
        }
        ::WaitForSingleObject(h_thread, 5000);
        ::CloseHandle(h_thread);
        result = ReadUInt32(result_addr);
    } catch (...) {
        result = 0;
    }
    try {
        Free(stub);
    } catch (...) {
    }
    return result;
}

uintptr_t ProcessMemory::ParseAddress(const std::string& s_in) {
    std::string s = Trim(s_in);
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty()) return 0;
    return static_cast<uintptr_t>(std::strtoull(s.c_str(), nullptr, 16));
}

}  // namespace poq
