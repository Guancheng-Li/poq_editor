#include "Native/ProcessMemory.h"
#include "Native/ProcessMemoryDetail.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace poq {

using mem_detail::HandleGuard;
using mem_detail::addr_hex;
using mem_detail::append_le32;
using mem_detail::decode_bytes;
using mem_detail::format_win32_error;
using mem_detail::trim;

ProcessMemory::~ProcessMemory() {
  close();
}

// ============================================================================
// 打开/关闭
// ============================================================================

bool ProcessMemory::open(unsigned int pid, unsigned int access) {
  close();
  handle_ = ::OpenProcess(access, FALSE, pid);
  pid_ = handle_ != nullptr ? pid : 0;  // 记录目标 pid(is_process_alive 用)
  return handle_ != nullptr;
}

void ProcessMemory::close() {
  if (handle_ != nullptr) {
    ::CloseHandle(handle_);
    handle_ = nullptr;
  }
  pid_ = 0;
}

std::vector<RegionInfo> ProcessMemory::enumerate_regions() {
  std::vector<RegionInfo> list;
  if (!is_open()) return list;
  uintptr_t addr = 0;
  while (addr < 0x7FFFFFFF) {
    MEMORY_BASIC_INFORMATION mbi{};
    SIZE_T n = ::VirtualQueryEx(handle_, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi));
    if (n == 0) break;
    // Protect 精确比较(对齐 memlib.py iter_regions: 含 PAGE_NOACCESS 但读不到会跳过)
    if (mbi.State == MEM_COMMIT &&
        (mbi.Protect == 0x01 || mbi.Protect == 0x02 || mbi.Protect == 0x04 || mbi.Protect == 0x20 ||
         mbi.Protect == 0x40 || mbi.Protect == 0x80)) {
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

std::vector<unsigned char> ProcessMemory::read(uintptr_t base_address, int n_size) {
  if (handle_ == nullptr) throw std::runtime_error("进程未打开");
  std::vector<unsigned char> buf(n_size);
  SIZE_T bytes_read = 0;
  BOOL ok = ::ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(base_address), buf.data(),
                                static_cast<SIZE_T>(n_size), &bytes_read);
  if (!ok) {
    DWORD err = ::GetLastError();
    throw std::runtime_error("ReadProcessMemory 失败 @ " + addr_hex(base_address) + ": " +
                             format_win32_error(err));
  }
  return buf;
}

bool ProcessMemory::read_buf(uintptr_t base_address, void* out, int n_size) noexcept {
  if (handle_ == nullptr || n_size <= 0) return false;
  SIZE_T bytes_read = 0;
  return ::ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(base_address), out,
                             static_cast<SIZE_T>(n_size), &bytes_read) != 0 &&
         bytes_read == static_cast<SIZE_T>(n_size);
}

unsigned int ProcessMemory::read_u_int32(uintptr_t base_address) {
  auto data = read(base_address, 4);
  return static_cast<unsigned int>(data[0]) | (static_cast<unsigned int>(data[1]) << 8) |
         (static_cast<unsigned int>(data[2]) << 16) | (static_cast<unsigned int>(data[3]) << 24);
}

uint16_t ProcessMemory::read_word(uintptr_t base_address) {
  auto data = read(base_address, 2);
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

float ProcessMemory::read_float(uintptr_t base_address) {
  auto data = read(base_address, 4);
  uint32_t bits = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                  (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

std::optional<std::string> ProcessMemory::read_string_gbk(uintptr_t base_address, int max_len,
                                                          int codepage) {
  std::vector<unsigned char> data;
  try {
    data = read(base_address, max_len);
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
    auto s = decode_bytes(bytes, cp);
    if (!s) continue;
    // 过滤明显垃圾:含过多控制字符(除常见空白)则视为无效
    bool bad = false;
    for (char c : *s)
      if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
        bad = true;
        break;
      }
    if (bad) continue;
    if (!trim(*s).empty()) return s;
  }
  return std::nullopt;
}

void ProcessMemory::write(uintptr_t base_address, const std::vector<unsigned char>& data) {
  write_internal(base_address, data.data(), data.size(), false);
}

void ProcessMemory::write_code(uintptr_t base_address, const std::vector<unsigned char>& code) {
  write_internal(base_address, code.data(), code.size(), true);
}

void ProcessMemory::write_internal(uintptr_t base_address, const unsigned char* data, size_t size,
                                   bool is_code) {
  // 计划 P1(#7): 完整检查 + 恢复页面保护 + 代码写后 flush 指令缓存。
  // 原实现漏检 VirtualProtectEx 返回值、从不恢复 old_protect、不校验 bytes_written,
  // 写机器码后也没 flush —— 目标 CPU 可能仍执行旧指令缓存。
  if (handle_ == nullptr) throw std::runtime_error("进程未打开");
  if (data == nullptr || size == 0) return;
  if (size > static_cast<size_t>(INT_MAX)) throw std::runtime_error("写入长度超出范围");

  DWORD old_protect = 0;
  if (!::VirtualProtectEx(handle_, reinterpret_cast<LPVOID>(base_address),
                          static_cast<SIZE_T>(size), PAGE_EXECUTE_READWRITE, &old_protect)) {
    DWORD err = ::GetLastError();
    throw std::runtime_error("VirtualProtectEx 失败 @ " + addr_hex(base_address) + ": " +
                             format_win32_error(err));
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
  } guard{handle_, reinterpret_cast<LPVOID>(base_address), static_cast<SIZE_T>(size), old_protect};

  SIZE_T bytes_written = 0;
  BOOL ok = ::WriteProcessMemory(handle_, reinterpret_cast<LPVOID>(base_address), data,
                                 static_cast<SIZE_T>(size), &bytes_written);
  if (!ok) {
    DWORD err = ::GetLastError();
    throw std::runtime_error("WriteProcessMemory 失败 @ " + addr_hex(base_address) + ": " +
                             format_win32_error(err));
  }
  // 写成功不等于写全(跨页/部分写入会静默成功)
  if (bytes_written != static_cast<SIZE_T>(size)) {
    throw std::runtime_error("WriteProcessMemory 部分写入 @ " + addr_hex(base_address) + ": " +
                             std::to_string(bytes_written) + "/" + std::to_string(size) + " 字节");
  }
  // 写机器码: 目标 CPU 可能仍在执行旧的指令缓存, 必须刷新
  if (is_code) {
    ::FlushInstructionCache(handle_, reinterpret_cast<LPCVOID>(base_address),
                            static_cast<SIZE_T>(size));
  }
}

void ProcessMemory::write_u_int32(uintptr_t base_address, unsigned int value) {
  unsigned char buf[4];
  buf[0] = static_cast<unsigned char>(value & 0xFF);
  buf[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
  buf[2] = static_cast<unsigned char>((value >> 16) & 0xFF);
  buf[3] = static_cast<unsigned char>((value >> 24) & 0xFF);
  write(base_address, std::vector<unsigned char>(buf, buf + 4));
}

void ProcessMemory::write_word(uintptr_t base_address, uint16_t value) {
  unsigned char buf[2];
  buf[0] = static_cast<unsigned char>(value & 0xFF);
  buf[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
  write(base_address, std::vector<unsigned char>(buf, buf + 2));
}

void ProcessMemory::write_float(uintptr_t base_address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  write_u_int32(base_address, bits);
}

// ============================================================================
// PE 解析 / 内存分配 / 导出表 / 远程 malloc
// ============================================================================

std::optional<uintptr_t> ProcessMemory::read_pe_image_base(uintptr_t module_base) {
  if (!is_open()) return std::nullopt;
  try {
    uint32_t e_lfanew = read_u_int32(module_base + 0x3C);
    auto hdr = read(module_base + e_lfanew, 0x40);
    uint16_t magic = static_cast<uint16_t>(hdr[0x18] | (hdr[0x19] << 8));
    if (magic == 0x10B) {  // PE32
      return static_cast<uintptr_t>(read_u_int32(module_base + e_lfanew + 0x34));
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

uintptr_t ProcessMemory::alloc(size_t size, uintptr_t near_addr) {
  if (!is_open()) throw std::runtime_error("进程未打开");
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
  void* p =
      ::VirtualAllocEx(handle_, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!p) {
    DWORD err = ::GetLastError();
    throw std::runtime_error("VirtualAllocEx 失败: " + format_win32_error(err));
  }
  return reinterpret_cast<uintptr_t>(p);
}

bool ProcessMemory::is_process_alive() const {
  if (handle_ == nullptr || pid_ == 0) return false;
  DWORD code = 0;
  if (!::GetExitCodeProcess(handle_, &code)) return false;
  return code == STILL_ACTIVE;
}

void ProcessMemory::free(uintptr_t address) {
  if (!is_open()) return;
  ::VirtualFreeEx(handle_, reinterpret_cast<LPVOID>(address), 0, MEM_RELEASE);
}

// 计划 P1/P2(#8): 超时不再直接 TerminateThread 后当作普通失败返回 ——
// 被强杀的线程可能持有游戏内部锁 / 正在 malloc / 改全局对象, 目标进程可能已损坏,
// 调用方需要知道"状态不可信", 并避免释放它可能仍在访问的 stub/buffer。
ProcessMemory::RemoteCallResult ProcessMemory::call_remote_thread_detailed(uintptr_t stub,
                                                                           int wait_ms,
                                                                           uintptr_t result_slot) {
  RemoteCallResult rr;
  if (!is_open()) {
    rr.status = RemoteCallStatus::kFailed;
    return rr;
  }
  HANDLE h_thread = ::CreateRemoteThread(
      handle_, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(stub), nullptr, 0, nullptr);
  if (!h_thread) {
    DWORD err = ::GetLastError();
    throw std::runtime_error("CreateRemoteThread 失败: " + format_win32_error(err));
  }
  HandleGuard thread_guard(h_thread);
  DWORD r = ::WaitForSingleObject(h_thread, static_cast<DWORD>(wait_ms));
  if (r == WAIT_OBJECT_0) {
    try {
      rr.value = read_int32(result_slot);
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
  // thread_guard 析构统一 CloseHandle
  // 进程已退出时统一上报 kProcessExited(远程内存随进程结束自动回收, 不必再管)
  if (rr.status != RemoteCallStatus::kCompleted && !is_process_alive()) {
    rr.status = RemoteCallStatus::kProcessExited;
  }
  return rr;
}

std::optional<int> ProcessMemory::call_remote_thread(uintptr_t stub, int wait_ms,
                                                     uintptr_t result_slot) {
  auto rr = call_remote_thread_detailed(stub, wait_ms, result_slot);
  if (!rr.ok()) return std::nullopt;
  return rr.value;
}

uintptr_t ProcessMemory::find_export(uintptr_t module_base, const std::string& name) {
  try {
    auto mz = read(module_base, 0x40);
    if (mz[0] != 'M' || mz[1] != 'Z') return 0;
    uint32_t e_lfanew = static_cast<uint32_t>(mz[0x3C]) | (static_cast<uint32_t>(mz[0x3D]) << 8) |
                        (static_cast<uint32_t>(mz[0x3E]) << 16) |
                        (static_cast<uint32_t>(mz[0x3F]) << 24);
    auto pe = read(module_base + e_lfanew, 0x88);
    if (pe[0] != 'P' || pe[1] != 'E') return 0;
    uint16_t magic = static_cast<uint16_t>(pe[0x18] | (pe[0x19] << 8));
    if (magic != 0x10B) return 0;  // PE32 only
    auto rd32 = [](const std::vector<unsigned char>& b, size_t off) -> uint32_t {
      return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
             (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
    };
    uint32_t export_rva = rd32(pe, 0x60);
    uint32_t export_size = rd32(pe, 0x64);
    if (!export_rva) return 0;
    auto exp = read(module_base + export_rva, 0x28);
    uint32_t number_of_names = rd32(exp, 0x18);
    uint32_t addr_of_functions = rd32(exp, 0x1C);
    uint32_t addr_of_names = rd32(exp, 0x20);
    uint32_t addr_of_ordinals = rd32(exp, 0x24);
    uint32_t max_names = std::min<uint32_t>(number_of_names, 0x4000);
    for (uint32_t i = 0; i < max_names; ++i) {
      try {
        uint32_t name_rva = read_u_int32(module_base + addr_of_names + i * 4);
        auto nm = read_string_gbk(module_base + name_rva, 64, 0);
        if (nm && *nm == name) {
          uint16_t ordinal = read_word(module_base + addr_of_ordinals + i * 2);
          uint32_t func_rva =
              read_u_int32(module_base + addr_of_functions + static_cast<uint32_t>(ordinal) * 4);
          // 转发器检测:func RVA 落在导出目录内(指向名字字符串)
          if (func_rva >= export_rva && func_rva < export_rva + export_size) return 0;
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

uintptr_t ProcessMemory::alloc_malloc(unsigned int pid, uint32_t size) {
  if (!is_open()) return 0;
  // 1) 找 CRT 的 malloc
  uintptr_t malloc_addr = 0;
  static const char* kCrtModules[] = {
      "msvcrt.dll",   "msvcr100.dll", "msvcr110.dll",
      "msvcr120.dll", "ucrtbase.dll", "api-ms-win-crt-heap-l1-1-0.dll",
  };
  for (const char* mod : kCrtModules) {
    auto base = find_module_base(pid, mod);
    if (!base) continue;
    malloc_addr = find_export(*base, "malloc");
    if (!malloc_addr) malloc_addr = find_export(*base, "_malloc_base");
    if (malloc_addr) break;
  }
  if (!malloc_addr) return 0;
  // 2) ExitThread(kernel32)
  auto kbase = find_module_base(pid, "kernel32.dll");
  uintptr_t exit_addr = 0;
  if (kbase) exit_addr = find_export(*kbase, "ExitThread");
  if (!exit_addr) return 0;
  // 3) 分配 stub + 结果槽(可执行内存)
  uintptr_t stub = 0;
  try {
    stub = alloc(0x100);
  } catch (...) {
    return 0;
  }
  uintptr_t result_addr = stub + 0x80;
  uintptr_t result = 0;
  try {
    write_u_int32(result_addr, 0);
    std::vector<unsigned char> code;
    code.push_back(0xB8);
    append_le32(code, static_cast<uint32_t>(malloc_addr));  // mov eax, malloc
    code.push_back(0x68);
    append_le32(code, size);  // push size
    code.push_back(0xFF);
    code.push_back(0xD0);  // call eax
    code.push_back(0x83);
    code.push_back(0xC4);
    code.push_back(0x04);  // add esp,4
    code.push_back(0xA3);
    append_le32(code, static_cast<uint32_t>(result_addr));  // mov [result],eax
    code.push_back(0xB8);
    append_le32(code, static_cast<uint32_t>(exit_addr));  // mov eax, ExitThread
    code.push_back(0x6A);
    code.push_back(0x00);  // push 0
    code.push_back(0xFF);
    code.push_back(0xD0);  // call eax
    write(stub, code);
    HANDLE h_thread = ::CreateRemoteThread(
        handle_, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(stub), nullptr, 0, nullptr);
    if (!h_thread) {
      free(stub);
      return 0;
    }
    HandleGuard thread_guard(h_thread);
    ::WaitForSingleObject(h_thread, 5000);
    result = read_u_int32(result_addr);
  } catch (...) {
    result = 0;
  }
  try {
    free(stub);
  } catch (...) {
  }
  return result;
}

uintptr_t ProcessMemory::parse_address(const std::string& s_in) {
  std::string s = trim(s_in);
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
  if (s.empty()) return 0;
  return static_cast<uintptr_t>(std::strtoull(s.c_str(), nullptr, 16));
}

}  // namespace poq
