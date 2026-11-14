#include "Native/ProcessMemory.h"
#include "Native/ProcessMemoryDetail.h"

#include <windows.h>
#include <tlhelp32.h>
#include <optional>
#include <string>
#include <vector>

namespace poq {

using mem_detail::HandleGuard;
using mem_detail::to_lower;
using mem_detail::wide_to_utf8;

std::vector<ProcessInfo> ProcessMemory::enumerate_processes(const std::string& name_filter) {
  std::vector<ProcessInfo> list;
  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == nullptr || snap == INVALID_HANDLE_VALUE) return list;
  HandleGuard snap_guard(snap);

  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (::Process32FirstW(snap, &pe)) {
    do {
      std::string name = wide_to_utf8(pe.szExeFile);
      if (name_filter.empty()) {
        list.push_back({static_cast<unsigned int>(pe.th32ProcessID), std::move(name)});
      } else if (to_lower(name).find(to_lower(name_filter)) != std::string::npos) {
        list.push_back({static_cast<unsigned int>(pe.th32ProcessID), std::move(name)});
      }
    } while (::Process32NextW(snap, &pe));
  }
  ::CloseHandle(snap);
  return list;
}

namespace {

std::string module_basename_lower(const std::string& path) {
  std::string mn = path;
  size_t s = mn.find_last_of("/\\");
  if (s != std::string::npos) mn = mn.substr(s + 1);
  return to_lower(mn);
}

std::optional<std::string> poq_game_module_label(unsigned int pid) {
  for (const ModuleInfo& m : ProcessMemory::enumerate_modules(pid)) {
    const std::string mn = module_basename_lower(m.name);
    if (mn == "poq.exe") return std::string("POQ.exe");
    if (mn == "easyrpg.dll") return std::string("EasyRPG.dll");
    if (mn == "easyrpg116.dll") return std::string("EasyRPG116.dll");
  }
  return std::nullopt;
}

}  // namespace

std::vector<ProcessInfo> ProcessMemory::enumerate_poq_targets() {
  std::vector<ProcessInfo> out;
  for (const auto& p : enumerate_processes()) {
    if (to_lower(p.name) == "poq.exe") {
      out.push_back(p);
      continue;
    }
    if (std::optional<std::string> mod = poq_game_module_label(p.pid)) {
      ProcessInfo q = p;
      q.game_module = *mod;
      out.push_back(std::move(q));
    }
  }
  return out;
}

std::vector<ModuleInfo> ProcessMemory::enumerate_modules(unsigned int pid) {
  std::vector<ModuleInfo> list;
  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == nullptr || snap == INVALID_HANDLE_VALUE) return list;
  HandleGuard snap_guard(snap);
  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  if (::Module32FirstW(snap, &me)) {
    do {
      std::string name = wide_to_utf8(me.szModule);
      list.push_back({std::move(name), reinterpret_cast<uintptr_t>(me.modBaseAddr),
                      static_cast<uint32_t>(me.modBaseSize)});
    } while (::Module32NextW(snap, &me));
  }
  ::CloseHandle(snap);
  return list;
}

std::optional<uintptr_t> ProcessMemory::find_module_base(unsigned int pid,
                                                           const std::string& name) {
  if (name.empty()) return std::nullopt;
  std::string target = name;
  size_t slash = target.find_last_of("/\\");
  if (slash != std::string::npos) target = target.substr(slash + 1);
  target = to_lower(target);
  for (const ModuleInfo& m : enumerate_modules(pid)) {
    std::string mn = m.name;
    size_t s2 = mn.find_last_of("/\\");
    if (s2 != std::string::npos) mn = mn.substr(s2 + 1);
    if (to_lower(mn) == target) return m.base;
  }
  return std::nullopt;
}

std::optional<std::string> ProcessMemory::find_module_path(unsigned int pid,
                                                           const std::string& name) {
  if (name.empty()) return std::nullopt;
  std::string target = name;
  size_t slash = target.find_last_of("/\\");
  if (slash != std::string::npos) target = target.substr(slash + 1);
  target = to_lower(target);
  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == nullptr || snap == INVALID_HANDLE_VALUE) return std::nullopt;
  HandleGuard snap_guard(snap);
  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  std::optional<std::string> out;
  if (::Module32FirstW(snap, &me)) {
    do {
      std::string mn = wide_to_utf8(me.szModule);
      size_t s2 = mn.find_last_of("/\\");
      if (s2 != std::string::npos) mn = mn.substr(s2 + 1);
      if (to_lower(mn) == target) {
        out = wide_to_utf8(me.szExePath);
        break;
      }
    } while (::Module32NextW(snap, &me));
  }
  ::CloseHandle(snap);
  return out;
}

}  // namespace poq
