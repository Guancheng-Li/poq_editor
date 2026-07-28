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

// Format a Win32 error code into a human-readable string (mirrors the C#
// Win32Exception.Message used by FormatMessage).
std::string FormatWin32Error(DWORD err) {
    char* buf = nullptr;
    DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string msg = (buf && n > 0) ? std::string(buf, n) : std::string("error ") + std::to_string(err);
    if (buf) ::LocalFree(buf);
    // Trim trailing whitespace/newlines.
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' '))
        msg.pop_back();
    return msg;
}

std::string AddrHex(uintptr_t addr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08llX", static_cast<unsigned long long>(addr));
    return buf;
}

}  // namespace

ProcessMemory::~ProcessMemory() { Close(); }

std::vector<ProcessInfo> ProcessMemory::EnumerateProcesses(const std::string& name_filter) {
    std::vector<ProcessInfo> list;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == nullptr || snap == INVALID_HANDLE_VALUE) return list;

    // Use the ANSI variant explicitly so szExeFile is a narrow char string,
    // independent of the project's UNICODE setting.
    PROCESSENTRY32A pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstA(snap, &pe)) {
        do {
            std::string name(pe.szExeFile);
            if (name_filter.empty()) {
                list.push_back({pe.th32ProcessID, std::move(name)});
            } else {
                // Case-insensitive substring search, matching C# IndexOf(OrdinalIgnoreCase).
                std::string n = name;
                std::string f = name_filter;
                std::transform(n.begin(), n.end(), n.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(f.begin(), f.end(), f.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (n.find(f) != std::string::npos)
                    list.push_back({pe.th32ProcessID, std::move(name)});
            }
        } while (::Process32NextA(snap, &pe));
    }
    ::CloseHandle(snap);
    return list;
}

bool ProcessMemory::Open(unsigned int pid, unsigned int access) {
    Close();
    handle_ = ::OpenProcess(access, FALSE, pid);
    return handle_ != nullptr;
}

void ProcessMemory::Close() {
    if (handle_ != nullptr) {
        ::CloseHandle(handle_);
        handle_ = nullptr;
    }
}

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

unsigned int ProcessMemory::ReadUInt32(uintptr_t base_address) {
    auto data = Read(base_address, 4);
    return static_cast<unsigned int>(data[0]) |
           (static_cast<unsigned int>(data[1]) << 8) |
           (static_cast<unsigned int>(data[2]) << 16) |
           (static_cast<unsigned int>(data[3]) << 24);
}

void ProcessMemory::Write(uintptr_t base_address, const std::vector<unsigned char>& data) {
    if (handle_ == nullptr) throw std::runtime_error("进程未打开");
    // Try to make the target page writable first; some game memory is read-only.
    DWORD old_protect = 0;
    ::VirtualProtectEx(handle_, reinterpret_cast<LPVOID>(base_address),
                       static_cast<SIZE_T>(data.size()), PAGE_EXECUTE_READWRITE, &old_protect);
    SIZE_T bytes_written = 0;
    BOOL ok = ::WriteProcessMemory(handle_,
                                   reinterpret_cast<LPVOID>(base_address),
                                   data.data(), static_cast<SIZE_T>(data.size()), &bytes_written);
    if (!ok) {
        DWORD err = ::GetLastError();
        throw std::runtime_error("WriteProcessMemory 失败 @ " + AddrHex(base_address) +
                                 ": " + FormatWin32Error(err));
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

uintptr_t ProcessMemory::ParseAddress(const std::string& s_in) {
    std::string s = s_in;
    // Trim whitespace.
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return 0;
    s = s.substr(a, b - a + 1);
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty()) return 0;
    return static_cast<uintptr_t>(std::strtoull(s.c_str(), nullptr, 16));
}

}  // namespace poq
