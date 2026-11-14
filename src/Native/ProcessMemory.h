#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace poq {

/// One enumerated process entry. Mirrors the C# `ProcessInfo` class.
struct ProcessInfo {
  unsigned int pid = 0;
  std::string name;  // e.g. "POQ.EXE" or DxWnd host exe

  /// When the host exe is not POQ.EXE, names the loaded game module (e.g. POQ.exe).
  std::string game_module;

  /// display string for the process combo box.
  std::string display() const {
    std::string s = name;
    if (!game_module.empty()) s += "  [" + game_module + "]";
    return s + "  (PID=" + std::to_string(pid) + ")";
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
  bool is_open() const { return handle_ != 0; }

  /// Enumerate all processes, optionally filtered by a case-insensitive name
  /// substring.
  static std::vector<ProcessInfo> enumerate_processes(const std::string& name_filter = "");

  /// Processes that run 秦殇: direct POQ.EXE or a wrapper (e.g. DxWnd) with POQ/EasyRPG loaded.
  static std::vector<ProcessInfo> enumerate_poq_targets();

  /// Enumerate the modules loaded in `pid` (SNAPMODULE|SNAPMODULE32 so a 64-bit
  /// caller can see the 32-bit target's modules).
  static std::vector<ModuleInfo> enumerate_modules(unsigned int pid);

  /// find a module's base address by name (case-insensitive basename match).
  /// Returns nullopt if not found.
  static std::optional<uintptr_t> find_module_base(unsigned int pid, const std::string& name);

  /// find a module's full path (W->UTF-8) by name (case-insensitive basename
  /// match). Returns nullopt if not found.
  static std::optional<std::string> find_module_path(unsigned int pid, const std::string& name);

  /// open the process with the given access mask. Returns true on success.
  /// Closes any previously-opened handle first.
  bool open(unsigned int pid, unsigned int access = kProcessAllAccess);

  /// close the current handle, if any.
  void close();

  /// Enumerate the target process's committed memory regions whose protection
  /// allows reading (exact Protect in {1,2,4,0x20,0x40,0x80}, matching
  /// memlib.py iter_regions). Scans 0..0x7FFFFFFF (32-bit target space).
  std::vector<RegionInfo> enumerate_regions();

  /// 写 `data` 到目标进程(数据)。校验 VirtualProtectEx / bytes_written, 并恢复页面保护。
  void write(uintptr_t base_address, const std::vector<unsigned char>& data);

  /// 写机器码: 同 write, 成功后额外 FlushInstructionCache(否则目标 CPU 可能仍执行旧指令)。
  void write_code(uintptr_t base_address, const std::vector<unsigned char>& code);

  /// write/write_code 的共同实现(is_code 决定是否 flush 指令缓存)。
  void write_internal(uintptr_t base_address, const unsigned char* data, size_t size, bool is_code);

  /// read `n_size` bytes from `base_address`. Throws std::runtime_error on failure.
  std::vector<unsigned char> read(uintptr_t base_address, int n_size);

  /// P2: 读进调用方缓冲, 不分配 —— 高频字段读取(如词条节点)用它。
  /// 成功返回 true; 失败返回 false(不抛异常, 便于批量读取里跳过坏地址)。
  bool read_buf(uintptr_t base_address, void* out, int n_size) noexcept;

  /// read a 4-byte little-endian DWORD.
  unsigned int read_u_int32(uintptr_t base_address);

  /// read a 4-byte signed int.
  int read_int32(uintptr_t base_address) { return static_cast<int>(read_u_int32(base_address)); }

  /// read a 2-byte little-endian WORD.
  uint16_t read_word(uintptr_t base_address);

  /// read a 4-byte little-endian float.
  float read_float(uintptr_t base_address);

  /// read a NUL-terminated string at `base_address` (up to `max_len` bytes) and
  /// decode it (gbk / utf-8 / latin-1 fallback). Returns UTF-8. `codepage` 0 or
  /// 936 means CP_ACP (GBK on Chinese systems). Returns nullopt on failure or
  /// if the decoded text is empty/garbage (caller shows "未知物品").
  std::optional<std::string> read_string_gbk(uintptr_t base_address, int max_len, int codepage);

  /// write a 4-byte DWORD.
  void write_u_int32(uintptr_t base_address, unsigned int value);

  /// write a 2-byte WORD.
  void write_word(uintptr_t base_address, uint16_t value);

  /// write a 4-byte float.
  void write_float(uintptr_t base_address, float value);

  /// parse the PE OptionalHeader ImageBase of the module loaded at
  /// `module_base` inside the target process (PE32/PE32+). nullopt on failure.
  std::optional<uintptr_t> read_pe_image_base(uintptr_t module_base);

  /// Allocate `size` bytes of executable memory in the target process. When
  /// `near` is nonzero, first try to allocate near it (useful for a 32-bit
  /// rel32 jmp within ±2GB). Throws std::runtime_error on failure.
  uintptr_t alloc(size_t size, uintptr_t near_addr = 0);

  /// free memory previously allocated with alloc (MEM_RELEASE).
  void free(uintptr_t address);

  /// run `stub` in a remote thread and wait up to `wait_ms` milliseconds. On
  /// completion, read the int32 at `result_slot` as the return value. Returns
  /// nullopt on timeout (the thread is terminated) or if the read fails.
  // 计划 P1/P2(#8): 远程线程调用结果不再用 nullopt 混表 timeout/失败/未知。
  enum class RemoteCallStatus {
    kCompleted,             // 正常返回, value 有效
    kFailed,                // 未拿到返回值(读取失败等)
    kTimedOutStillRunning,  // 超时且线程被强制终止 —— 目标进程状态已不可信
    kProcessExited,         // 目标进程已退出
  };
  struct RemoteCallResult {
    RemoteCallStatus status = RemoteCallStatus::kFailed;
    int value = 0;
    bool ok() const { return status == RemoteCallStatus::kCompleted; }
  };
  RemoteCallResult call_remote_thread_detailed(uintptr_t stub, int wait_ms, uintptr_t result_slot);

  std::optional<int> call_remote_thread(uintptr_t stub, int wait_ms, uintptr_t result_slot);

  /// Look up an exported function address (PE32) inside a target-process module.
  /// Returns 0 if the module/export is missing or the entry is a forwarder.
  uintptr_t find_export(uintptr_t module_base, const std::string& name);

  /// Call the CRT `malloc` inside the target process via a remote-thread stub,
  /// returning the allocated address (0 on failure). Depends on the target
  /// having loaded a CRT with a usable `malloc` export.
  uintptr_t alloc_malloc(unsigned int pid, uint32_t size);

  /// parse a hex address string ("0x901FF0" or "901FF0") into an address.
  static uintptr_t parse_address(const std::string& s);

  /// 目标进程是否仍存活(句柄有效且未退出)。
  bool is_process_alive() const;

  /// open() 时记录的目标 PID; 未打开时为 0。
  unsigned int target_pid() const { return pid_; }

private:
  void* handle_ = nullptr;  // HANDLE
  unsigned int pid_ = 0;    // 目标进程 id(判断进程是否退出用)
};

}  // namespace poq
