// 召唤兽修改 —— v1.30 版(EasyRPG116.dll)独立逻辑。
//
// 原则: 磁盘上的 EasyRPG116.dll 只读、只分析 (见 data/py/analyze_116_summon.py);
//       所有改动经修改器对 Poq.exe 进程内已加载模块 WriteProcessMemory, 不写游戏目录 DLL.
//
// 1:1 移植 data/py/summon_tool_130.py 的 cmd_summon 路径(任务 task7 只用到
// summon 命令)。与 Steam 版(SummonSteam.cpp)完全隔离,不共享任何召唤逻辑。
//
// 机制(仅 EasyRPG116 / summon_tool_130.py; 勿与 Steam/EasyRPG.dll 混用):
// - 召唤结果主要由 CC 槽 + 名字槽决定; 偏移相对 EasyRPG116 基址, 与 Steam 不同。
// - 朱厌/善翼均 type=10 → dec 后索引 9, 读 21319C 名字; 须同时写 213190+21319C 为目标长名。
//   勿写 MB+0x213184(213190-0xC)。勿写 37307C/373080(创建参数, 非 CC)。
// - 前 8 种兽不写名字槽; 秦兵走类型表 field[0]/槽10 标签(见下)。
// - 秦兵 1-6 大类走 field[0] 单命中必中(长名<=10B 直接放槽10, >10B 重定向
//   field[0] 到 scratch 唯一标签 Q###)。
// - 秦兵类型表指针临时改写需备份:写 %TEMP%\poq_summon_qin_backup_130.json,
//   下次激活/刷新前还原(发布目录不写任何文件)。

#include "Core/Summon130.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <vector>
#include "Core/Json.h"
#include "Core/SummonScratch.h"
#include "Core/TextCodec.h"
#include "Native/ProcessMemoryDetail.h"

namespace poq {
namespace summon_130 {

namespace {

// ---------------------------------------------------------------------------
// 常量(对齐 summon_tool_130.py)
// ---------------------------------------------------------------------------
constexpr uintptr_t kOffFriend = 0x20CD6;   // 强制 Friend: push 1; imm 在 +1
constexpr uintptr_t kOffAigroup = 0x20D23;  // 敌方召唤: push imm G_AIGroup
// EasyRPG116.dll @MB+0x20D23 磁盘原版 (勿用旧误值 …16 01, 会写坏召唤链).
constexpr unsigned char kAigroupOrig[] = {0x50, 0x68, 0x18, 0xbb, 0x20, 0x10};
constexpr uintptr_t kOffControl = 0x20D69;  // 可控性: cmp eax,imm
constexpr uintptr_t kOffSkip = 0x20D6C;     // 跳过注册: jg 7f 19 -> jmp eb 19
constexpr uintptr_t kOffBeastIndexMaxMov = 0x209D0;  // mov eax,10; cmp ecx,eax; jge → 拒索引10(善翼)
constexpr uintptr_t kOffBound = 0x20B39;    // 召唤类型上限: cmp eax,imm (原版 imm=6)
constexpr uintptr_t kOffQinJge = 0x20B3C;  // 紧跟 cmp: jge -> 秦兵/高 CC 分支 (离线 PE 分析)
constexpr uintptr_t kOffType = 0x20687;     // 强制类型: call -> mov eax,<slot>
constexpr uintptr_t kTypeOrigCallOff = 0x1A4082;  // 原版 call 目标 (reset 重建用, 对齐 python)
constexpr uintptr_t kCcGlobalOff = 0x373078;
// 召唤"数量": 技能参数三 -> setter MB+0x206C0 计算后写入 MB+0x373080;
// 创建循环在 MB+0x20AC1 处 cmp [373080],edi / jl -> body(MB+0x20B34) 决定造几只。
// 想限制只造 N 只, 安全做法是改这个比较(只改循环上界, 不动技能参数解析);
// 覆盖数量 setter 会破坏参数解析 -> 只出技能特效不出单位(已废弃)。
constexpr uintptr_t kOffCountCmp = 0x20AC1;  // 3B 3D 80 30 37 10 : cmp [373080],edi
constexpr size_t kCountCmpLen = 6;
// 数量参数 setter(MB+0x206C0, 共 0x1E 字节)。本工具早期版本曾整段覆盖过它(导致
// "只出技能特效不出单位"), 这里保留还原点, 保证每次激活都会把它从磁盘 DLL 还原,
// 即使游戏进程里还残留旧补丁也能自愈。
constexpr uintptr_t kOffCountSetter = 0x206C0;
constexpr size_t kCountSetterLen = 0x1E;
constexpr uintptr_t kCcGlobalSync[] = {0x373078, 0x37307C, 0x373080};
constexpr uintptr_t kOffSpawnCcJneA = 0x20A89;  // 39 2d [373078] 后 jne (勿 patch 0x20A88)
constexpr uintptr_t kOffSpawnCcJneB = 0x20B9C;  // 低路径 jne
constexpr uintptr_t kOffSpawnHighEbpSync = 0x20BED;  // 高路径: cmp eax,ebp; jne → mov ebp,eax (CC=10)
constexpr unsigned char kCcLoadA1[] = {0xa1, 0x78, 0x30, 0x37, 0x10};
constexpr unsigned char kCcLoadEcx[] = {0x8b, 0x0d, 0x78, 0x30, 0x37, 0x10};
constexpr unsigned char kCcLoadEdx7c[] = {0x8b, 0x15, 0x7c, 0x30, 0x37, 0x10};
constexpr uintptr_t kSpuriousMatcherImm = 0x204D2;  // 早期扫描误改的 push imm (非匹配器)
constexpr uintptr_t kNameSlotStride = 0xC;  // 116.dll 内三槽步长(与 Steam 模块偏移无对应关系)
constexpr uintptr_t kSlotOff10 = 0x213190;  // 召唤兽善翼 (.rdata 名字表 +0)
constexpr uintptr_t kSlotOffZhuYan = 0x21319C;  // 召唤兽朱厌 (+0xC, 非 槽9-0xC)
constexpr uintptr_t kSlotOff9 = kSlotOff10 - kNameSlotStride;  // 仅日志/旧路径, 非朱厌
constexpr uintptr_t kTabOff = 0x3B45F0;     // 类型表计数
constexpr uintptr_t kTabRecOff = 0x3B45F8;  // 记录起点 = TAB+8
constexpr uint32_t kStride = 0x140;
constexpr uint32_t kMaxRec = 600;

// mon_type 用户映射 -> CC 槽 + 名字槽(仅 1.30; Steam 见 SummonSteamInternal kMonSlots)
struct MonMapEntry {
  int cc;
  const char* slot_name;  // need_slot 时写入的名字(UTF-8)
  const char* label;      // 日志/UI 短名
  bool need_slot;
  uintptr_t slot_off;     // MB+offset; 0 表示不写槽
};
const MonMapEntry kMonMap[10] = {
    {1, nullptr, "狼", false, 0},     {2, nullptr, "虎", false, 0},
    {3, nullptr, "熊", false, 0},     {5, nullptr, "白狼", false, 0},
    {6, nullptr, "木偶", false, 0},   {7, nullptr, "狗", false, 0},
    {8, nullptr, "虎蛟", false, 0},   {9, nullptr, "狍鸮", false, 0},
    {10, "召唤兽朱厌", "朱厌", true, kSlotOff10},
    {10, "召唤兽善翼", "善翼", true, kSlotOff10},  // 同 type10+索引9, 21319C/213190 均写善翼
};

// patch_type 的 slot 参数 = 技能类型值 (非 dec 后索引)
const char* kBeastSlots[12] = {"",     "狼", "老虎", "熊",   "鸟",   "白狼",
                               "木偶", "狗", "虎蛟", "狍鸮", "朱厌", "善翼"};

// 六大类 (soldier_type 1-6):(大类名, 短名, [(子类名, 长名包含子串)...]).
// 1.30 队长有 '旧重剑'(steam 无); UI 层不显示旧重剑(对齐 task7 #4 备注)。
struct QinSub {
  const char* name;
  const char* long_name;
};
struct QinGroup {
  const char* name;
  const char* short_name;
  std::vector<QinSub> subs;
};
const std::vector<QinGroup>& qin_groups() {
  static const std::vector<QinGroup> g = {
      {"普通兵",
       "秦兵",
       {{"刀盾", "秦军士兵刀盾"}, {"长戈", "秦军士兵长戈"}, {"弓箭", "秦军士兵弓箭"}}},
      {"队长",
       "秦兵队长",
       {{"长斧", "秦军头目长斧"}, {"旧重剑", "秦军头目旧重剑"}, {"重剑", "秦军头目重剑"}}},
      {"大将",
       "秦兵大将",
       {{"A", "秦兵大将A"},
        {"B", "秦兵大将B"},
        {"C", "秦兵大将C"},
        {"D", "秦兵大将D"},
        {"E", "秦兵大将E"}}},
      {"官员", "秦军官员", {{"官员", "秦军官员"}}},
      {"法师",
       "秦军法师",
       {{"A", "秦军法师A"},
        {"B", "秦军法师B"},
        {"C", "秦军法师C"},
        {"D", "秦军法师D"},
        {"E", "秦军法师E"}}},
      {"大法师",
       "秦军大法师",
       {{"A", "秦军大法师A"},
        {"B", "秦军大法师B"},
        {"C", "秦军大法师C"},
        {"D", "秦军大法师D"},
        {"E", "秦军大法师E"}}},
  };
  return g;
}

// ---------------------------------------------------------------------------
// 小工具(模块自包含)
// ---------------------------------------------------------------------------
std::string to_lower(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string hex8(uintptr_t a) {
  char b[32];
  std::snprintf(b, sizeof(b), "%08llX", static_cast<unsigned long long>(a));
  return b;
}

std::string hex8_bytes(const std::vector<unsigned char>& b) {
  std::string out;
  for (unsigned char c : b) {
    char t[4];
    std::snprintf(t, sizeof(t), "%02x ", c);
    out += t;
  }
  if (!out.empty()) out.pop_back();
  return out;
}

// 读 GBK 字符串(UTF-8 返回)。与 python rstr 等价:失败返回 "?",全 0 返回 ""。
std::string r_str(ProcessMemory& mem, uintptr_t a, int n) {
  try {
    std::vector<unsigned char> b = mem.read(a, n);
    size_t z = 0;
    while (z < b.size() && b[z] != 0) ++z;
    if (z == 0) return "";
    int wn = ::MultiByteToWideChar(936, 0, reinterpret_cast<const char*>(b.data()),
                                   static_cast<int>(z), nullptr, 0);
    if (wn <= 0) return "?";
    std::wstring w(static_cast<size_t>(wn), L'\0');
    ::MultiByteToWideChar(936, 0, reinterpret_cast<const char*>(b.data()), static_cast<int>(z),
                          &w[0], wn);
    int un = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    if (un <= 0) return "?";
    std::string out(static_cast<size_t>(un), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &out[0], un, nullptr, nullptr);
    return out;
  } catch (...) {
    return "?";
  }
}

std::vector<unsigned char> gbk_bytes(const std::string& s) {
  std::string g = utf8_to_gbk(s);
  return std::vector<unsigned char>(g.begin(), g.end());
}

// %TEMP% 下的备份文件路径(发布目录不写文件)。
std::wstring backup_path() {
  wchar_t tmp[MAX_PATH];
  ::GetTempPathW(MAX_PATH, tmp);
  return std::wstring(tmp) + L"poq_summon_qin_backup_130.json";
}

bool write_text_file_utf8(const std::wstring& path, const std::string& text) {
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  fclose(f);
  return ok;
}

std::string read_text_file_utf8(const std::wstring& path) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return "";
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

// ---------------------------------------------------------------------------
// 从磁盘 DLL 读原版字节 → 写回进程内模块 (不改文件; 修复旧版误 patch 的 E8 等)
// ---------------------------------------------------------------------------
static uint32_t pe_u32(const std::vector<unsigned char>& d, size_t i) {
  return static_cast<uint32_t>(d[i]) | (static_cast<uint32_t>(d[i + 1]) << 8) |
         (static_cast<uint32_t>(d[i + 2]) << 16) | (static_cast<uint32_t>(d[i + 3]) << 24);
}

static bool pe_rva_to_file_off(const std::vector<unsigned char>& d, uintptr_t rva, size_t& out) {
  if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return false;
  uint32_t pe = pe_u32(d, 0x3C);
  if (pe + 0x18 > d.size()) return false;
  uint16_t nsec = static_cast<uint16_t>(d[pe + 6]) | (static_cast<uint16_t>(d[pe + 7]) << 8);
  uint16_t opt_sz = static_cast<uint16_t>(d[pe + 0x14]) | (static_cast<uint16_t>(d[pe + 0x15]) << 8);
  size_t sec = pe + 0x18 + opt_sz;
  for (uint16_t i = 0; i < nsec; ++i) {
    size_t s = sec + static_cast<size_t>(i) * 40;
    if (s + 40 > d.size()) return false;
    uint32_t vaddr = pe_u32(d, s + 12);
    uint32_t raw_size = pe_u32(d, s + 16);
    uint32_t raw_off = pe_u32(d, s + 20);
    if (rva < vaddr || rva >= vaddr + raw_size) continue;
    out = static_cast<size_t>(raw_off + (rva - vaddr));
    return out < d.size();
  }
  return false;
}

static std::optional<std::vector<unsigned char>> pe_read_rva(const std::vector<unsigned char>& d,
                                                              uintptr_t rva, size_t len) {
  size_t fo = 0;
  if (!pe_rva_to_file_off(d, rva, fo) || fo + len > d.size()) return std::nullopt;
  return std::vector<unsigned char>(d.begin() + static_cast<std::ptrdiff_t>(fo),
                                    d.begin() + static_cast<std::ptrdiff_t>(fo + len));
}

void restore_summon_vanilla_from_disk(ProcessMemory& mem, uintptr_t mb, unsigned int pid,
                                      std::string& log) {
  if (pid == 0) return;
  std::optional<std::string> path = ProcessMemory::find_module_path(pid, "EasyRPG116.dll");
  if (!path) {
    log += "  (未找到 EasyRPG116.dll 路径, 跳过磁盘原版还原)\n";
    return;
  }
  std::wstring wp = mem_detail::utf8_to_wide(*path);
  FILE* f = _wfopen(wp.c_str(), L"rb");
  if (!f) {
    log += "  [!!] 无法打开 DLL: " + *path + "\n";
    return;
  }
  std::vector<unsigned char> file;
  unsigned char buf[65536];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
  fclose(f);
  if (file.empty()) return;

  struct Site {
    uintptr_t rva;
    size_t len;
  };
  // 仅还原 summon_tool_130 会改的 5 处 + 旧实验遗留的 type 误改点; 勿批量还原 20Bxx 簇(磁盘与
  // 运行时不一致时会破坏「未激活可召、激活后全灭」).
  const Site kSites[] = {
      {0x20353, 5}, {0x20373, 5}, {0x206B3, 5}, {0x206D3, 5}, {0x20687, 5},
      {0x209D0, 5}, {0x20B39, 3}, {0x20BED, 5}, {0x20CD6, 2}, {0x20D23, 6}, {0x20D69, 3},
      {0x20D6C, 2},
  };
  // 注意: 数量比较(0x20AC1)与数量 setter(0x206C0) 属于 20Ax 簇, 磁盘与运行时可能不一致,
  // **不要**从磁盘还原它们(否则会出现"不能召唤太多了(0)"); 这两处由本文件自己保存
  // 运行时原字节并原样还原(见 restore_count_patch)。
  int restored = 0;
  for (const Site& s : kSites) {
    std::optional<std::vector<unsigned char>> ref = pe_read_rva(file, s.rva, s.len);
    if (!ref) continue;
    try {
      std::vector<unsigned char> cur = mem.read(mb + s.rva, static_cast<int>(s.len));
      if (cur == *ref) continue;
      mem.write_code(mb + s.rva, *ref);
      if (mem.read(mb + s.rva, static_cast<int>(s.len)) == *ref) ++restored;
    } catch (...) {
    }
  }
  if (restored > 0) {
    log += "  已从磁盘 DLL 还原 " + std::to_string(restored) +
           " 处召唤相关代码 (仅进程内存, 文件未改)\n";
  }
  // v2 误写 CC 到 37307C/373080 会污染 newpersonex, 进程不重启也会残留 —— 按磁盘映像初值还原。
  const uintptr_t kDataRv[] = {kCcGlobalOff, 0x37307C, 0x373080};
  int data_n = 0;
  for (uintptr_t rva : kDataRv) {
    std::optional<std::vector<unsigned char>> ref = pe_read_rva(file, rva, 4);
    if (!ref || ref->size() != 4) continue;
    try {
      std::vector<unsigned char> cur = mem.read(mb + rva, 4);
      if (cur == *ref) continue;
      mem.write(mb + rva, *ref);
      if (mem.read(mb + rva, 4) == *ref) ++data_n;
    } catch (...) {
    }
  }
  if (data_n > 0) {
    log += "  已还原 " + std::to_string(data_n) + " 个召唤全局(373078/7C/80)为磁盘初值\n";
  }
}

// ---------------------------------------------------------------------------
// 补丁(写后回读校验,失败仅记日志,与 python patch_bytes 一致)
// ---------------------------------------------------------------------------
bool patch_bytes(ProcessMemory& mem, uintptr_t addr, const std::vector<unsigned char>& data,
                 const std::string& label, uintptr_t mb, std::string& log) {
  std::string off = mb ? " (MB+0x" + hex8(addr - mb) + ")" : "";
  try {
    std::vector<unsigned char> old = mem.read(addr, static_cast<int>(data.size()));
    const bool in_module = mb && addr >= mb && addr < mb + 0x02000000;
    if (in_module)
      mem.write_code(addr, data);
    else
      mem.write(addr, data);
    std::vector<unsigned char> nw = mem.read(addr, static_cast<int>(data.size()));
    if (nw == data) {
      log += "  [OK] " + label + " @0x" + hex8(addr) + off;
      log += ": " + hex8_bytes(old) + " -> " + hex8_bytes(data) + "\n";
      return true;
    }
    log +=
        "  [!!] " + label + " 写入后校验失败: " + hex8_bytes(nw) + " != " + hex8_bytes(data) + "\n";
  } catch (const std::exception& e) {
    log += "  [!!] " + label + " 写入异常: " + e.what() + "\n";
  }
  return false;
}

// 恢复误改 MB+0x204D2 (不在召唤补丁簇内).
void restore_spurious_matcher_patch(ProcessMemory& mem, uintptr_t mb, std::string& log) {
  try {
    if (mem.read(mb + kSpuriousMatcherImm - 1, 1)[0] != 0x6a) return;
    if (mem.read(mb + kSpuriousMatcherImm, 1)[0] != 0x00) return;
    patch_bytes(mem, mb + kSpuriousMatcherImm, {0x01}, "恢复误改 push imm MB+0x204D2", mb, log);
  } catch (...) {
  }
}

void patch_type(ProcessMemory& mem, uintptr_t mb, int slot, std::string& log) {
  // 与 summon_tool_130.py patch_type 一致: mov eax,<slot> (setter 内 a3 写 373078)
  std::vector<unsigned char> data;
  uint32_t v = static_cast<uint32_t>(slot & 0xFFFFFFFF);
  data.push_back(0xb8);
  data.push_back(static_cast<unsigned char>(v & 0xFF));
  data.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
  data.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
  data.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
  const char* nm = (slot >= 0 && slot < static_cast<int>(sizeof(kBeastSlots) / sizeof(kBeastSlots[0])))
                       ? kBeastSlots[slot]
                       : "?";
  patch_bytes(mem, mb + kOffType, data,
              "强制召唤类型 slot=" + std::to_string(slot) + "(" + nm + ")", mb, log);
}

std::vector<unsigned char> imm_patch_for_reg(int slot, uint8_t op) {
  uint32_t v = static_cast<uint32_t>(slot & 0xFFFFFFFF);
  std::vector<unsigned char> out = {op};
  out.push_back(static_cast<unsigned char>(v & 0xFF));
  out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
  out.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
  out.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
  return out;
}

void patch_cc_cluster_immediates(ProcessMemory& mem, uintptr_t mb, int slot, std::string& log) {
  struct RefSite {
    uintptr_t off;
    const unsigned char* expect;
    size_t expect_len;
    uint8_t mov_op;  // b8 eax / b9 ecx / ba edx
    bool pad_nop;
    const char* tag;
  };
  const RefSite refs[] = {
      {0x209BD, kCcLoadA1, 5, 0xb8, false, "CC簇 a1→eax"},
      {0x20A5C, kCcLoadA1, 5, 0xb8, false, "CC簇 a1→eax"},
      {0x20B34, kCcLoadA1, 5, 0xb8, false, "生成 a1→eax"},
      {0x20BE8, kCcLoadA1, 5, 0xb8, false, "CC簇 a1→eax"},
      {0x20C13, kCcLoadA1, 5, 0xb8, false, "CC簇 a1→eax"},
      {0x20D61, kCcLoadA1, 5, 0xb8, false, "注册 a1→eax"},
      {0x209CA, kCcLoadEcx, 6, 0xb9, true, "CC簇 ecx←mem"},
      {0x20B6F, kCcLoadEcx, 6, 0xb9, true, "匹配 ecx←mem"},
      {0x20C37, kCcLoadEcx, 6, 0xb9, true, "CC簇 ecx←mem"},
      {0x20C18, kCcLoadEdx7c, 6, 0xba, true, "CC簇 edx←37307C"},
  };
  for (const RefSite& r : refs) {
    try {
      std::vector<unsigned char> cur =
          mem.read(mb + r.off, static_cast<int>(r.pad_nop ? 6 : 5));
      std::vector<unsigned char> data = imm_patch_for_reg(slot, r.mov_op);
      if (r.pad_nop) data.push_back(0x90);
      const size_t n = data.size();
      bool already = cur.size() >= n && cur[0] == r.mov_op;
      if (!already) {
        std::vector<unsigned char> exp(r.expect, r.expect + r.expect_len);
        if (cur.size() < r.expect_len ||
            !std::equal(exp.begin(), exp.end(), cur.begin())) {
          log += "  [!!] " + std::string(r.tag) + " MB+0x" + hex8(r.off) + " 非预期: " +
                 hex8_bytes(cur) + " 跳过\n";
          continue;
        }
      }
      patch_bytes(mem, mb + r.off, data,
                  std::string(r.tag) + " →mov r," + std::to_string(slot), mb, log);
    } catch (const std::exception& e) {
      log += std::string("  [!!] ") + r.tag + ": " + e.what() + "\n";
    }
  }
}

// 高路径 a1 CC 后 eax=CC, 但 ebp 常为栈上旧值 → 原版 jne 跳过刷怪; 用 mov ebp,eax 同步 (勿 NOP 0x20BF0 会切坏 jne).
void patch_spawn_high_ebp_sync(ProcessMemory& mem, uintptr_t mb, std::string& log) {
  const unsigned char kOrig[] = {0x3b, 0xc5, 0x75, 0x27, 0x6a};
  const std::vector<unsigned char> kPatch = {0x89, 0xc5, 0x90, 0x90, 0x90};
  try {
    std::vector<unsigned char> cur = mem.read(mb + kOffSpawnHighEbpSync, 5);
    if (cur == kPatch) return;
    if (cur.size() < 5 || !std::equal(std::begin(kOrig), std::end(kOrig), cur.begin())) {
      log += "  [!!] 高路径 ebp 同步 MB+0x" + hex8(kOffSpawnHighEbpSync) + " 非预期: " +
             hex8_bytes(cur) + " 跳过\n";
      return;
    }
    patch_bytes(mem, mb + kOffSpawnHighEbpSync, kPatch, "高路径 mov ebp,eax (CC=朱厌/善翼等)", mb,
                log);
  } catch (const std::exception& e) {
    log += std::string("  [!!] 高路径 ebp 同步: ") + e.what() + "\n";
  }
}

// 低路径: CC 全局与 ebp 不一致时 jne 跳过刷怪 (只 NOP jne, 不 NOP 匹配 je —— 强行走失败匹配会触发游戏「召唤失败」).
void patch_spawn_cc_mismatch_jne(ProcessMemory& mem, uintptr_t mb, std::string& log) {
  const struct {
    uintptr_t off;
    const char* tag;
  } sites[] = {
      {kOffSpawnCcJneA, "NOP 低路径 CC≠ebp jne@20A89"},
      {kOffSpawnCcJneB, "NOP 低路径 CC≠ebp jne@20B9C"},
  };
  for (const auto& s : sites) {
    try {
      std::vector<unsigned char> cur = mem.read(mb + s.off, 2);
      if (cur[0] == 0x90 && cur[1] == 0x90) continue;
      if (cur[0] != 0x75) {
        log += std::string("  [!!] ") + s.tag + " 非 jne: " + hex8_bytes(cur) + " 跳过\n";
        continue;
      }
      patch_bytes(mem, mb + s.off, {0x90, 0x90}, s.tag, mb, log);
    } catch (const std::exception& e) {
      log += std::string("  [!!] ") + s.tag + ": " + e.what() + "\n";
    }
  }
}

void patch_bound(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
  std::vector<unsigned char> data = {0x83, 0xf8, static_cast<unsigned char>(val & 0xFF)};
  patch_bytes(mem, mb + kOffBound, data, "type bound=0x" + hex8(val), mb, log);
}

// 放宽 dec 后索引上界 (原版 mov eax,10 → index>=10 失败, 善翼需要 index=10)。
void patch_beast_table_index_max(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
  // 仅善翼(type 11 → dec 后 index 10) 需要; 勿对全部召唤改 209D0(易误伤其它逻辑)。
  try {
    std::vector<unsigned char> cur = mem.read(mb + kOffBeastIndexMaxMov, 5);
    if (cur.size() < 2 || cur[0] != 0xb8) {
      log += "  [!!] MB+0x209D0 非 mov eax,imm, 跳过野兽表 index 上界\n";
      return;
    }
    if (cur[1] == static_cast<unsigned char>(val & 0xFF)) return;
    cur[1] = static_cast<unsigned char>(val & 0xFF);
    patch_bytes(mem, mb + kOffBeastIndexMaxMov, cur, "野兽表 index 上界 mov eax", mb, log);
  } catch (const std::exception& e) {
    log += std::string("  [!!] 野兽表 index 上界: ") + e.what() + "\n";
  }
}

// 原版 cmp eax,6 + jge: CC>=6(含善翼/朱厌) 进秦兵路径; NOP 掉 jge 让高 CC 走野兽名匹配。
void patch_qin_jge_skip(ProcessMemory& mem, uintptr_t mb, bool on, std::string& log) {
  if (on) {
    patch_bytes(mem, mb + kOffQinJge, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                "禁用 CC>=6 跳秦兵(jge NOP)", mb, log);
  } else {
    patch_bytes(mem, mb + kOffQinJge, {0x0f, 0x8d, 0xab, 0x00, 0x00, 0x00},
                "恢复 CC>=6 跳秦兵(jge)", mb, log);
  }
}

void patch_control(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
  std::vector<unsigned char> data = {0x83, 0xf8, static_cast<unsigned char>(val & 0xFF)};
  patch_bytes(mem, mb + kOffControl, data, "可控性 cmp 0x" + hex8(val), mb, log);
}

void patch_friend(ProcessMemory& mem, uintptr_t mb, int val, std::string& log) {
  // push 1 的 imm 在 opcode 0x6a 后 +1
  std::vector<unsigned char> data = {static_cast<unsigned char>(val & 0xFF)};
  patch_bytes(mem, mb + kOffFriend + 1, data, "召唤强制 Friend imm", mb, log);
}

void patch_skip(ProcessMemory& mem, uintptr_t mb, bool on, std::string& log) {
  if (on) {
    patch_bytes(mem, mb + kOffSkip, {0xeb, 0x19}, "跳过 AddMember 注册(不可控)", mb, log);
  } else {
    patch_bytes(mem, mb + kOffSkip, {0x7f, 0x19}, "恢复 AddMember 注册", mb, log);
  }
}

void patch_aigroup(ProcessMemory& mem, uintptr_t mb, uint32_t ag, std::string& log) {
  std::vector<unsigned char> data = {0x68};
  data.push_back(static_cast<unsigned char>(ag & 0xFF));
  data.push_back(static_cast<unsigned char>((ag >> 8) & 0xFF));
  data.push_back(static_cast<unsigned char>((ag >> 16) & 0xFF));
  data.push_back(static_cast<unsigned char>((ag >> 24) & 0xFF));
  data.push_back(0x90);
  patch_bytes(mem, mb + kOffAigroup, data, "敌方 G_AIGroup=" + std::to_string(ag), mb, log);
}

void restore_aigroup(ProcessMemory& mem, uintptr_t mb, std::string& log) {
  patch_bytes(mem, mb + kOffAigroup,
              std::vector<unsigned char>(std::begin(kAigroupOrig), std::end(kAigroupOrig)),
              "还原 G_AIGroup 复制", mb, log);
}

bool verify_runtime_130(ProcessMemory& mem, uintptr_t mb, std::string& log) {
  log += "  EasyRPG116.dll 基址 = 0x" + hex8(mb) + "\n";
  restore_summon_vanilla_from_disk(mem, mb, mem.target_pid(), log);
  try {
    std::vector<unsigned char> t = mem.read(mb + kOffType, 5);
    log += "  type @MB+0x" + hex8(kOffType) + ": " + hex8_bytes(t) + "\n";
    if (t[0] != 0xe8) {
      log += "  [!!] type 非原版 call(e8) —— 与磁盘 DLL 或偏移不符, 中止\n";
      log += "  提示: 确认顶部版本为 v1.30 且进程内为 EasyRPG116.dll\n";
      return false;
    }
    if (std::optional<std::string> dll_path =
            ProcessMemory::find_module_path(mem.target_pid(), "EasyRPG116.dll")) {
      std::wstring wp = mem_detail::utf8_to_wide(*dll_path);
      FILE* df = _wfopen(wp.c_str(), L"rb");
      if (df) {
        std::vector<unsigned char> file;
        unsigned char buf[4096];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), df)) > 0) file.insert(file.end(), buf, buf + n);
        fclose(df);
        if (auto ref = pe_read_rva(file, kOffType, 5)) {
          if (*ref != t) {
            log += "  [!!] 进程内 type 字节与磁盘 DLL 不一致 —— 可能非本机游戏目录 DLL\n";
          }
        }
      }
    }
  } catch (const std::exception& e) {
    log += std::string("  [!!] 读取 type 失败: ") + e.what() + " —— 中止\n";
    return false;
  }
  try {
    uint32_t cnt = mem.read_u_int32(mb + kTabOff);
    if (cnt == 0 || cnt > 10000) {
      log += "  [!!] 类型表 cnt=" + std::to_string(cnt) + " 异常 —— 中止\n";
      return false;
    }
    int named = 0;
    uint32_t limit = cnt < kMaxRec ? cnt : kMaxRec;
    for (uint32_t i = 0; i < limit; ++i) {
      uint32_t p0 = mem.read_u_int32(mb + kTabRecOff + static_cast<uintptr_t>(i) * kStride);
      if (!p0) continue;
      std::string s0 = r_str(mem, p0, 20);
      if (s0.find("善翼") != std::string::npos || s0.find("朱厌") != std::string::npos) ++named;
    }
    log += "  类型表 cnt=" + std::to_string(cnt) + ", 含善翼/朱厌长名 " + std::to_string(named) +
           " 条 ✓\n";
  } catch (const std::exception& e) {
    log += std::string("  [!!] 类型表自检: ") + e.what() + " —— 中止\n";
    return false;
  }
  return true;
}

void patch_matcher(ProcessMemory& /*mem*/, uintptr_t /*mb*/, bool /*use_field1_short*/,
                   std::string& /*log*/) {
  // EasyRPG116 与 summon_tool_130.py 一致: 不 patch 模块内 push 0/1 匹配器.
}

// 清除秦兵 field[0]=Q### 残留(无备份时曾改表会导致槽10 长名仍匹配秦兵)。
int clear_stale_qin_field0_tags(ProcessMemory& mem, uintptr_t mb, std::string& log) {
  int n = 0;
  uint32_t cnt = 0;
  try {
    cnt = mem.read_u_int32(mb + kTabOff);
  } catch (...) {
    return 0;
  }
  uint32_t limit = cnt < kMaxRec ? cnt : kMaxRec;
  for (uint32_t i = 0; i < limit; ++i) {
    uintptr_t base = mb + kTabRecOff + static_cast<uintptr_t>(i) * kStride;
    uint32_t p0 = 0;
    try {
      p0 = mem.read_u_int32(base);
    } catch (...) {
      continue;
    }
    if (!p0 || p0 < 0x10000) continue;
    std::string s0 = r_str(mem, p0, 8);
    if (s0.size() >= 2 && s0[0] == 'Q' && s0.size() <= 5) {
      ++n;
    }
  }
  if (n > 0) {
    log += "  [!!] 类型表仍有 " + std::to_string(n) +
           " 条 field[0]=Q### 残留; 请先读档/重启游戏, 或再激活一次秦兵后取消\n";
  }
  return n;
}

void patch_cc_global(ProcessMemory& mem, uintptr_t mb, int cc_store, std::string& log) {
  uint32_t v = static_cast<uint32_t>(cc_store);
  for (uintptr_t cc_off : kCcGlobalSync) {
    try {
      uint32_t old = mem.read_u_int32(mb + cc_off);
      mem.write_u_int32(mb + cc_off, v);
      uint32_t nw = mem.read_u_int32(mb + cc_off);
      if (nw == v) {
        log += "  [OK] CC 全局 MB+0x" + hex8(cc_off) + " = " + std::to_string(cc_store) +
               " (原 " + std::to_string(old) + ")\n";
      } else {
        log += "  [!!] CC 全局 MB+0x" + hex8(cc_off) + " 写入校验失败\n";
      }
    } catch (const std::exception& e) {
      log += std::string("  [!!] CC 全局 MB+0x") + hex8(cc_off) + ": " + e.what() + "\n";
    }
  }
}

// 限制"一次召唤造几只": 把创建循环的次数比较改成固定上限。
// 原指令 6 字节: 3B 3D 80 30 37 10 (cmp [0x10373080],edi) + jl
// 改成       : 83 FF NN 90 90 90          (cmp edi,NN) + jl   —— 语义一致, 只改上界。
// 注意: 该点在 20Ax 簇里, 磁盘字节与运行时可能不同, 因此这里保存**运行时原字节**,
// 取消/刷新时原样写回(不经过磁盘还原表)。
namespace {
std::vector<unsigned char> g_count_orig;
uintptr_t g_count_site = 0;
}  // namespace

std::string restore_count_patch(ProcessMemory& mem, uintptr_t /*mb*/) {
  std::string log;
  if (g_count_orig.empty() || g_count_site == 0) return log;
  try {
    mem.write_code(g_count_site, g_count_orig);
    log += "  已还原召唤数量比较为运行时原字节\n";
  } catch (const std::exception& e) {
    log += std::string("  [!!] 还原数量比较失败: ") + e.what() + "\n";
  }
  g_count_orig.clear();
  g_count_site = 0;
  return log;
}

void patch_summon_count(ProcessMemory& mem, uintptr_t mb, int count, std::string& log) {
  if (count <= 0) return;
  std::vector<unsigned char> data = {0x83, 0xff, static_cast<unsigned char>(count & 0xFF), 0x90,
                                     0x90, 0x90};
  try {
    std::vector<unsigned char> cur = mem.read(mb + kOffCountCmp, kCountCmpLen);
    const bool already = cur.size() == kCountCmpLen && cur[0] == 0x83 && cur[1] == 0xff &&
                         cur[2] == data[2] && cur[3] == 0x90;
    if (already) return;
    const bool orig = cur.size() == kCountCmpLen && cur[0] == 0x3b && cur[1] == 0x3d &&
                      cur[2] == 0x80 && cur[3] == 0x30 && cur[4] == 0x37 && cur[5] == 0x10;
    if (!orig) {
      log += "  [!!] 召唤数量比较 MB+0x" + hex8(kOffCountCmp) + " 非预期: " + hex8_bytes(cur) +
             ", 跳过(不限制数量)\n";
      return;
    }
    g_count_orig = cur;
    g_count_site = mb + kOffCountCmp;
    patch_bytes(mem, mb + kOffCountCmp, data,
                "限制单次召唤数量=" + std::to_string(count) + " 只", mb, log);
  } catch (const std::exception& e) {
    log += std::string("  [!!] 召唤数量限制: ") + e.what() + "\n";
  }
}

void apply_tail_patches(ProcessMemory& mem, uintptr_t mb, int slot, int group, int control_val,
                        int count_override, std::string& log) {
  // 与 summon_tool_130.py cmd_summon 补丁段一致.
  patch_type(mem, mb, slot, log);
  // 勿写 37307C/373080: 文档为创建用数值/数量, 强行写 type 会导致 newpersonex 属性表查不到(兽XXX)。
  patch_summon_count(mem, mb, count_override, log);
  patch_bound(mem, mb, 0x7f, log);
  patch_control(mem, mb, control_val, log);
  if (group == 2) {
    patch_friend(mem, mb, 0xff, log);
    patch_aigroup(mem, mb, 1, log);
    patch_skip(mem, mb, true, log);
  } else {
    // 与 00a4cb3 一致: 队友/盟友勿改 0x20D23 (仅「取消/激活前磁盘还原」恢复敌方补丁).
    patch_friend(mem, mb, 1, log);
    patch_skip(mem, mb, group == 1, log);
  }
  try {
    uint32_t cc = mem.read_u_int32(mb + kCcGlobalOff);
    uint8_t bnd = mem.read(mb + kOffBound + 2, 1)[0];
    uint8_t t0 = mem.read(mb + kOffType, 1)[0];
    log += "  [自检] slot=" + std::to_string(slot) + ", CC全局=" + std::to_string(cc) +
           ", bound.cmp=0x" + hex8(bnd) + ", type@20687=" +
           (t0 == 0xb8 ? "mov" : t0 == 0xe8 ? "call" : "?") + " (python 最小补丁)\n";
  } catch (...) {
  }
}

// 1.30 名字串常在堆上, MB+0x21319x 可能是镜像/指针(probe_slots); 须写游戏实际读的地址。
void push_unique(std::vector<uintptr_t>& out, uintptr_t a) {
  for (uintptr_t x : out)
    if (x == a) return;
  out.push_back(a);
}

std::vector<uintptr_t> find_gbk_cstring(ProcessMemory& mem, const std::string& utf8_name,
                                          size_t max_hits) {
  std::vector<uintptr_t> hits;
  std::string needle = utf8_to_gbk(utf8_name);
  needle.push_back('\0');
  for (const RegionInfo& r : mem.enumerate_regions()) {
    if (r.size < needle.size() || r.size > 8 * 1024 * 1024) continue;
    try {
      std::vector<unsigned char> buf = mem.read(r.base, static_cast<int>(r.size));
      for (size_t i = 0; i + needle.size() <= buf.size(); ++i) {
        if (std::memcmp(buf.data() + i, needle.data(), needle.size()) == 0) {
          push_unique(hits, r.base + i);
          if (hits.size() >= max_hits) return hits;
        }
      }
    } catch (...) {
    }
  }
  return hits;
}

// 解析应写入的名字地址: 模块槽内联 / 槽内指针 / 堆上已有 GBK 串(可多址同步写)。
std::vector<uintptr_t> resolve_name_write_targets(ProcessMemory& mem, uintptr_t mb,
                                                    uintptr_t slot_off,
                                                    const std::string& lookup_name,
                                                    std::string& log) {
  std::vector<uintptr_t> out;
  uintptr_t slot_va = mb + slot_off;
  try {
    std::string inline_s = r_str(mem, slot_va, 16);
    if (!inline_s.empty() && inline_s.find("召唤") != std::string::npos)
      push_unique(out, slot_va);
  } catch (...) {
  }
  try {
    uint32_t p = mem.read_u_int32(slot_va);
    if (p >= 0x10000 && p < 0x80000000) {
      std::string at = r_str(mem, p, 16);
      if (at.find("召唤") != std::string::npos) {
        push_unique(out, p);
        log += "  名字槽指针 MB+0x" + hex8(slot_off) + " -> 0x" + hex8(p) + " (" + at + ")\n";
      }
    }
  } catch (...) {
  }
  for (uintptr_t hit : find_gbk_cstring(mem, lookup_name, 6)) {
    push_unique(out, hit);
    log += "  堆上名字串 0x" + hex8(hit) + " (" + lookup_name + ")\n";
  }
  if (out.empty()) push_unique(out, slot_va);
  return out;
}

// 写名字槽(对齐 python write_slot: 只写 MB+slot_off, 不扫堆 —— 堆上误写会破坏其它模板名)。
bool write_slot(ProcessMemory& mem, uintptr_t mb, uintptr_t slot_off, const std::string& name,
                std::string& log) {
  std::string g = utf8_to_gbk(name);
  g.append(4, '\0');
  const char* slot_hint =
      (slot_off == kSlotOffZhuYan) ? "21319C" : (slot_off == kSlotOff10 ? "213190" : "slot");
  uintptr_t addr = mb + slot_off;
  std::vector<unsigned char> data(g.begin(), g.end());
  try {
    mem.write(addr, data);
    std::string s = r_str(mem, addr, 12);
    bool ok = (s == name);
    log += "  [" + std::string(ok ? "OK" : "!!") + "] 名字 MB+0x" + hex8(slot_off) + " (" +
           slot_hint + ") = " + name + (ok ? " ✓" : "  (读回 " + s + ")") + "\n";
    return ok;
  } catch (const std::exception& e) {
    log += std::string("  [!!] 名字槽写入异常: ") + e.what() + "\n";
    return false;
  }
}

// 清空善翼相关名字(随机秦兵: 空名 + CC=10)
bool clear_slot(ProcessMemory& mem, uintptr_t mb, std::string& log) {
  uintptr_t addr = mb + kSlotOff10;
  try {
    mem.write(addr, std::vector<unsigned char>(12, 0));
    std::string s = r_str(mem, addr, 12);
    bool ok = s.empty();
    log += "  [" + std::string(ok ? "OK" : "!!") + "] 清空名字 MB+0x" + hex8(kSlotOff10) + " " +
           (ok ? "✓" : "  (读回 " + s + ")") + "\n";
    return ok;
  } catch (const std::exception& e) {
    log += std::string("  [!!] 槽10 清空异常: ") + e.what() + "\n";
    return false;
  }
}

// type=10 → dec 后索引 9, 拷名源为 21319C; 213190 也要同步(野兽/刺客/秦兵共用)。
bool write_slot_type10_pair(ProcessMemory& mem, uintptr_t mb, const std::string& name,
                            std::string& log) {
  if (!write_slot(mem, mb, kSlotOff10, name, log)) return false;
  if (!write_slot(mem, mb, kSlotOffZhuYan, name, log)) return false;
  return true;
}

bool clear_slot_type10_pair(ProcessMemory& mem, uintptr_t mb, std::string& log) {
  bool ok10 = clear_slot(mem, mb, log);
  try {
    mem.write(mb + kSlotOffZhuYan, std::vector<unsigned char>(12, 0));
    std::string s = r_str(mem, mb + kSlotOffZhuYan, 12);
    bool ok9 = s.empty();
    log += "  [" + std::string(ok9 ? "OK" : "!!") + "] 清空名字 MB+0x" + hex8(kSlotOffZhuYan) +
           " (21319C) " + (ok9 ? "✓" : "  (读回 " + s + ")") + "\n";
    return ok10 && ok9;
  } catch (const std::exception& e) {
    log += std::string("  [!!] 21319C 清空异常: ") + e.what() + "\n";
    return false;
  }
}

std::optional<std::string> find_type_table_field0(ProcessMemory& mem, uintptr_t mb,
                                                  const char* needle) {
  uint32_t cnt = 0;
  try {
    cnt = mem.read_u_int32(mb + kTabOff);
  } catch (...) {
    return std::nullopt;
  }
  const std::string key = needle;
  uint32_t limit = cnt < kMaxRec ? cnt : kMaxRec;
  for (uint32_t i = 0; i < limit; ++i) {
    uintptr_t base = mb + kTabRecOff + static_cast<uintptr_t>(i) * kStride;
    uint32_t p0 = 0;
    try {
      p0 = mem.read_u_int32(base);
    } catch (...) {
      continue;
    }
    if (!p0 || p0 < 0x10000) continue;
    std::string s0 = r_str(mem, p0, 32);
    if (s0 == key || s0.find(key) != std::string::npos) return s0;
  }
  return std::nullopt;
}

// 刺客: 对齐 Steam —— 类型表长名「克隆刺客弓/刀」写入 21319C+213190 (type10 读索引9)。
bool cmd_cike(ProcessMemory& mem, uintptr_t mb, bool on, int assassin_type, std::string& log) {
  if (on) {
    const char* fallback = (assassin_type == 1) ? "召唤兽刺客刀" : "召唤兽刺客弓";
    const char* scan = (assassin_type == 1) ? "克隆刺客刀" : "克隆刺客弓";
    std::string name = find_type_table_field0(mem, mb, scan).value_or(fallback);
    if (!write_slot_type10_pair(mem, mb, name, log)) return false;
    log += "  刺客长名=" + name + " (type10→索引9, 21319C+213190)\n";
    return true;
  }
  if (!write_slot(mem, mb, kSlotOffZhuYan, "召唤兽朱厌", log)) return false;
  if (!write_slot(mem, mb, kSlotOff10, "召唤兽善翼", log)) return false;
  log += "  刺客名字槽已还原 (21319C=朱厌 213190=善翼)\n";
  return true;
}

// ---------------------------------------------------------------------------
// 秦兵类型表备份(写 %TEMP%\poq_summon_qin_backup_130.json)
// ---------------------------------------------------------------------------
uintptr_t g_mb = 0;  // 模块基址(相对 MB 偏移计算需要; cmd_summon 入口设置)

void save_qin_backup(ProcessMemory& mem,
                     const std::vector<std::tuple<int, int, uintptr_t>>& mutated,
                     uintptr_t scratch) {
  (void)mem;
  json::Value root = json::Value::make_object();
  json::Value recs = json::Value::make_object();
  for (const auto& m : mutated) {
    int r, off;
    uintptr_t v;
    std::tie(r, off, v) = m;
    json::Value item = json::Value::make_object();
    item.set("rec", json::Value::make_number(r));
    item.set("off", json::Value::make_number(off));
    item.set("v", json::Value::make_number(static_cast<double>(v)));
    // 键含 off: 同 rec 多偏移不互覆(对齐 python f"{r}_{off}")
    recs.set(std::to_string(r) + "_" + std::to_string(off), std::move(item));
  }
  root.set("recs", std::move(recs));
  root.set("scratch", scratch ? json::Value::make_number(static_cast<double>(scratch))
                              : json::Value::make_null());
  write_text_file_utf8(backup_path(), json::write(root, false));
}

bool restore_qin_backup(ProcessMemory& mem) {
  std::string text = read_text_file_utf8(backup_path());
  if (text.empty()) return false;
  try {
    json::Value root = json::parse(text);
    const json::Value* recs = root.find("recs");
    if (!recs || !recs->is_object()) return false;
    uintptr_t scratch = 0;
    if (const json::Value* s = root.find("scratch"))
      if (s->is_number()) scratch = static_cast<uintptr_t>(s->number);
    int n = 0;
    for (const auto& kv : recs->object) {
      double rec = 0, off = 4, v = 0;
      if (kv.second.is_object()) {
        if (const json::Value* x = kv.second.find("rec")) rec = x->number;
        if (const json::Value* x = kv.second.find("off")) off = x->number;
        if (const json::Value* x = kv.second.find("v")) v = x->number;
      } else {
        rec = std::atof(kv.first.c_str());  // 旧格式: 裸指针 = field[1]
        off = 4;
        v = kv.second.number;
      }
      try {
        mem.write_u_int32(
            g_mb + kTabRecOff + static_cast<uintptr_t>(rec) * kStride + static_cast<uintptr_t>(off),
            static_cast<uint32_t>(v));
        ++n;
      } catch (...) {
      }
    }
    if (scratch) {
      try {
        mem.free(scratch);
      } catch (...) {
      }
    }
    ::DeleteFileW(backup_path().c_str());
    return n > 0;
  } catch (...) {
    return false;
  }
}

class QinScratch {
public:
  explicit QinScratch(ProcessMemory& mem) : mem_(mem), base_(alloc_summon_scratch(mem)) {}
  // ★ 析构不释放 scratch:游戏召唤时才读 field[0] -> 标签,立即释放会导致游戏
  // 访问已释放内存而卡死。scratch 驻留到下次召唤命令的 restore_qin_backup
  // (读备份文件里的 scratch 地址统一释放,与 python 行为一致)。
  ~QinScratch() {}
  QinScratch(const QinScratch&) = delete;
  QinScratch& operator=(const QinScratch&) = delete;

  uintptr_t put(const std::string& gbk_name) {
    std::vector<unsigned char> data = gbk_bytes(gbk_name);
    data.push_back(0);
    mem_.write(base_ + off_, data);
    uintptr_t p = base_ + off_;
    off_ += data.size();
    return p;
  }
  uintptr_t base() const { return base_; }

private:
  ProcessMemory& mem_;
  uintptr_t base_ = 0;
  size_t off_ = 0;
};

// ---------------------------------------------------------------------------
// 秦兵子类匹配(扫类型表: 短名==大类短名 且 长名包含子类串)
// ---------------------------------------------------------------------------
std::optional<std::vector<int>> resolve_qin_sub(ProcessMemory& mem, int group_idx, int sub_idx) {
  const auto& groups = qin_groups();
  if (group_idx < 0 || group_idx >= static_cast<int>(groups.size())) return std::nullopt;
  const QinGroup& g = groups[static_cast<size_t>(group_idx)];
  if (sub_idx < 0 || sub_idx >= static_cast<int>(g.subs.size())) return std::nullopt;
  const char* sub_str = g.subs[static_cast<size_t>(sub_idx)].long_name;
  std::vector<int> recs;
  uint32_t cnt = 0;
  try {
    cnt = mem.read_u_int32(g_mb + kTabOff);
  } catch (...) {
    cnt = 0;
  }
  uint32_t limit = cnt < kMaxRec ? cnt : kMaxRec;
  for (uint32_t i = 0; i < limit; ++i) {
    uintptr_t base = g_mb + kTabRecOff + static_cast<uintptr_t>(i) * kStride;
    uint32_t p0 = 0, p1 = 0;
    try {
      p0 = mem.read_u_int32(base);
      p1 = mem.read_u_int32(base + 4);
    } catch (...) {
      continue;
    }
    if (!p0 || !p1 || p0 == 0xFFFFFFFF || p1 == 0xFFFFFFFF) continue;
    if (p0 < 0x200000 || p1 < 0x200000) continue;
    std::string s1 = r_str(mem, p1, 16);
    if (s1 != g.short_name) continue;
    std::string s0 = r_str(mem, p0, 32);
    if (s0.find(sub_str) != std::string::npos) recs.push_back(static_cast<int>(i));
  }
  if (recs.empty()) return std::nullopt;
  return recs;
}

std::string legal_subs(const QinGroup& g) {
  std::string out;
  for (size_t i = 0; i < g.subs.size(); ++i) {
    if (!out.empty()) out += " / ";
    out += std::to_string(i) + "=" + g.subs[i].name;
  }
  return out;
}

// 1/2/6 大类 (长名>10B): 目标子类第一条 field[0] 重定向 scratch 标签 Q###, 槽10=Q###.
bool summon_qin_field0_tag(ProcessMemory& mem, int group_idx, int sub_idx, std::string& log) {
  // 先还原上次 field[0] 残留,否则本次 resolve 扫表会读到上次的标签指针(或已释放
  // scratch)导致"无匹配记录"、召唤仍是上次的怪。(python 仅无匹配时才 restore,这里
  // 提前到开头更稳健:连续召唤同子类也能命中)
  restore_qin_backup(mem);
  patch_matcher(mem, g_mb, false, log);
  auto recs = resolve_qin_sub(mem, group_idx, sub_idx);
  const auto& groups = qin_groups();
  const QinGroup& g = groups[static_cast<size_t>(group_idx)];
  if (!recs) {
    restore_qin_backup(mem);
    log += "  大类[" + std::to_string(group_idx) + "]=" + g.name +
           " 无匹配记录, 或子类越界 (该版本子类: " + legal_subs(g) + ")\n";
    return false;
  }
  int rec = (*recs)[0];
  char tagbuf[16];
  std::snprintf(tagbuf, sizeof(tagbuf), "Q%03d", rec);
  std::string tag = tagbuf;
  QinScratch scr(mem);
  uintptr_t tp = scr.put(tag);
  uintptr_t base = g_mb + kTabRecOff + static_cast<uintptr_t>(rec) * kStride;
  uint32_t orig = mem.read_u_int32(base);
  std::string s0 = r_str(mem, orig, 32);
  mem.write_u_int32(base, static_cast<uint32_t>(tp));  // field[0] -> 标签
  write_slot_type10_pair(mem, g_mb, tag, log);
  save_qin_backup(mem, {{rec, 0, orig}}, scr.base());
  // 自校验
  std::string s10;
  try {
    s10 = r_str(mem, g_mb + kSlotOffZhuYan, 8);
  } catch (const std::exception& e) {
    log += std::string("  [!!] 自校验读取异常: ") + e.what() + "\n";
  }
  bool ok = (s10 == tag);
  if (!ok) {
    log += "  [!!] 自校验失败 槽10=" + s10 + " (期望 " + tag + ")\n";
    return false;
  }
  log += std::string("  [秦兵·") + g.name + "/" + g.subs[static_cast<size_t>(sub_idx)].name +
         "] rec" + std::to_string(rec) + " (" + s0 + ") -> field[0]=" + tag + ", 槽10=" + tag +
         " ✓ 单命中必中\n";
  return true;
}

// 3/4/5 大类 (长名<=10B): 长名单直接放槽10, 前缀匹配单命中.
// 长名>10B 返回 false(调用方直接 return, 与 python 返回 None 行为一致)。
bool summon_qin_longname(ProcessMemory& mem, int group_idx, int sub_idx, std::string& log) {
  auto recs = resolve_qin_sub(mem, group_idx, sub_idx);
  const auto& groups = qin_groups();
  const QinGroup& g = groups[static_cast<size_t>(group_idx)];
  if (!recs) {
    restore_qin_backup(mem);
    log += "  大类[" + std::to_string(group_idx) + "]=" + g.name +
           " 无匹配记录, 或子类越界 (该版本子类: " + legal_subs(g) + ")\n";
    return false;
  }
  int rec = (*recs)[0];
  uint32_t p0 = mem.read_u_int32(g_mb + kTabRecOff + static_cast<uintptr_t>(rec) * kStride);
  std::string long_name = r_str(mem, p0, 32);
  if (utf8_to_gbk(long_name).size() > 10) return false;  // 长名放不进槽10
  restore_qin_backup(mem);
  patch_matcher(mem, g_mb, false, log);
  write_slot_type10_pair(mem, g_mb, long_name, log);
  std::string s10 = r_str(mem, g_mb + kSlotOffZhuYan, 12);
  bool ok = (s10 == long_name);
  if (!ok) {
    log += "  [!!] 自校验失败 槽10=" + s10 + " (期望 " + long_name + ")\n";
    return false;
  }
  log += std::string("  [秦兵·") + g.name + "/" + g.subs[static_cast<size_t>(sub_idx)].name +
         "] rec" + std::to_string(rec) + " 长名=" + long_name + " -> 槽10 ✓ 单命中必中\n";
  return true;
}

bool summon_type_table_by_rec(ProcessMemory& mem, uintptr_t mb, int rec, std::string& log) {
  restore_qin_backup(mem);
  patch_matcher(mem, mb, false, log);
  uintptr_t row = mb + kTabRecOff + static_cast<uintptr_t>(rec) * kStride;
  uint32_t p0 = mem.read_u_int32(row);
  if (!p0 || p0 < 0x10000) {
    log += "  [!!] 类型表 rec" + std::to_string(rec) + " field[0] 无效\n";
    return false;
  }
  std::string long_name = r_str(mem, p0, 48);
  if (long_name.empty()) {
    log += "  [!!] 类型表 rec" + std::to_string(rec) + " 长名为空\n";
    return false;
  }
  const size_t gl = utf8_to_gbk(long_name).size();
  if (gl <= 10) {
    if (!write_slot_type10_pair(mem, mb, long_name, log)) return false;
    log += "  [类型表 rec" + std::to_string(rec) + "] 长名=" + long_name + " -> 双槽 ✓\n";
    return true;
  }
  char tagbuf[16];
  std::snprintf(tagbuf, sizeof(tagbuf), "Q%03d", rec);
  std::string tag = tagbuf;
  QinScratch scr(mem);
  uintptr_t tp = scr.put(tag);
  uint32_t orig = p0;
  mem.write_u_int32(row, static_cast<uint32_t>(tp));
  if (!write_slot_type10_pair(mem, mb, tag, log)) return false;
  save_qin_backup(mem, {{rec, 0, orig}}, scr.base());
  std::string s9 = r_str(mem, mb + kSlotOffZhuYan, 12);
  if (s9 != tag) {
    log += "  [!!] 类型表 rec" + std::to_string(rec) + " 槽校验失败\n";
    return false;
  }
  log += "  [类型表 rec" + std::to_string(rec) + "] (" + long_name + ") -> field[0]=" + tag +
         " 双槽 ✓\n";
  return true;
}

std::map<std::string, std::string> parse_args(const std::vector<std::string>& args) {
  std::map<std::string, std::string> d;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
      std::string k = a.substr(2);
      std::string v;
      size_t eq = k.find('=');
      if (eq != std::string::npos) {
        v = k.substr(eq + 1);
        k = k.substr(0, eq);
      } else if (i + 1 < args.size()) {
        v = args[++i];
      }
      d[to_lower(k)] = v;
    }
  }
  return d;
}

int parse_int(const std::string& s) {
  return static_cast<int>(std::strtol(s.c_str(), nullptr, 0));
}

}  // namespace

namespace {

void dedupe_wild_beast_rows_by_field0(std::vector<WildBeastRow>& rows) {
  std::vector<WildBeastRow> kept;
  kept.reserve(rows.size());
  for (const WildBeastRow& row : rows) {
    bool dup = false;
    for (const WildBeastRow& k : kept) {
      if (k.field0 == row.field0) {
        dup = true;
        break;
      }
    }
    if (!dup) kept.push_back(row);
  }
  rows.swap(kept);
}

}  // namespace

std::vector<WildBeastRow> list_beast_type_table(ProcessMemory& mem, uintptr_t mb, int mon_type) {
  std::vector<WildBeastRow> out;
  if (mon_type != 8 && mon_type != 9) return out;
  g_mb = mb;
  const char* key = (mon_type == 8) ? "朱厌" : "善翼";
  uint32_t cnt = 0;
  try {
    cnt = mem.read_u_int32(mb + kTabOff);
  } catch (...) {
    return out;
  }
  uint32_t limit = cnt < kMaxRec ? cnt : kMaxRec;
  for (uint32_t rec = 0; rec < limit; ++rec) {
    uintptr_t base = mb + kTabRecOff + static_cast<uintptr_t>(rec) * kStride;
    uint32_t p0 = 0, p1 = 0;
    try {
      p0 = mem.read_u_int32(base);
      p1 = mem.read_u_int32(base + 4);
    } catch (...) {
      continue;
    }
    if (!p0 || !p1 || p0 < 0x10000 || p1 < 0x10000) continue;
    std::string s0 = r_str(mem, p0, 48);
    std::string s1 = r_str(mem, p1, 24);
    if (s0.find(key) == std::string::npos) continue;
    WildBeastRow row;
    row.rec = static_cast<int>(rec);
    row.field0 = s0;
    row.field1 = s1;
    row.gbk_len = static_cast<int>(utf8_to_gbk(s0).size());
    out.push_back(std::move(row));
  }
  dedupe_wild_beast_rows_by_field0(out);
  std::sort(out.begin(), out.end(),
            [](const WildBeastRow& a, const WildBeastRow& b) { return a.field0 < b.field0; });
  return out;
}

std::vector<WildBeastRow> list_misc_type_table(ProcessMemory& mem, uintptr_t mb) {
  std::vector<WildBeastRow> out;
  g_mb = mb;
  uint32_t cnt = 0;
  try {
    cnt = mem.read_u_int32(mb + kTabOff);
  } catch (...) {
    return out;
  }
  uint32_t limit = cnt < kMaxRec ? cnt : kMaxRec;
  for (uint32_t rec = 0; rec < limit; ++rec) {
    uintptr_t base = mb + kTabRecOff + static_cast<uintptr_t>(rec) * kStride;
    uint32_t p0 = 0, p1 = 0;
    try {
      p0 = mem.read_u_int32(base);
      p1 = mem.read_u_int32(base + 4);
    } catch (...) {
      continue;
    }
    if (!p0 || !p1 || p0 < 0x10000 || p1 < 0x10000) continue;
    std::string s0 = r_str(mem, p0, 48);
    std::string s1 = r_str(mem, p1, 24);
    if (s0.empty() && s1.empty()) continue;
    WildBeastRow row;
    row.rec = static_cast<int>(rec);
    row.field0 = s0;
    row.field1 = s1;
    row.gbk_len = static_cast<int>(utf8_to_gbk(s0).size());
    out.push_back(std::move(row));
  }
  std::sort(out.begin(), out.end(), [](const WildBeastRow& a, const WildBeastRow& b) {
    if (a.field0 != b.field0) return a.field0 < b.field0;
    if (a.field1 != b.field1) return a.field1 < b.field1;
    return a.rec < b.rec;
  });
  // 按需求: 这里不做任何过滤/去重 —— 类型表里每条记录都列出来(靠模糊搜索找)。
  return out;
}

// ---------------------------------------------------------------------------
// 公开接口:cmd_summon(对齐 python)
// ---------------------------------------------------------------------------
std::string cmd_restore_vanilla(ProcessMemory& mem, uintptr_t mb) {
  std::string log;
  if (mb == 0) throw std::runtime_error("v1.30 模块基址无效(EasyRPG116.dll 未找到?)");
  g_mb = mb;
  restore_spurious_matcher_patch(mem, mb, log);
  log += restore_count_patch(mem, mb);  // 数量比较: 用运行时原字节还原
  restore_summon_vanilla_from_disk(mem, mb, mem.target_pid(), log);
  try {
    uint8_t t0 = mem.read(mb + kOffType, 1)[0];
    log += "  [取消] type@20687=" + std::string(t0 == 0xe8 ? "call(原版)" : "已补丁 mov") + "\n";
    log += "  已恢复进程内召唤补丁点为磁盘原版; 请再试烽火狼烟(游戏默认逻辑).\n";
  } catch (...) {
  }
  return log;
}

std::string cmd_summon(ProcessMemory& mem, uintptr_t mb, const std::vector<std::string>& args) {
  std::string log;
  if (mb == 0) {
    throw std::runtime_error("v1.30 模块基址无效(EasyRPG116.dll 未找到?)");
  }
  g_mb = mb;
  restore_spurious_matcher_patch(mem, mb, log);
  // 先把上一轮自己打的「数量限制」还原(用运行时原字节), 再走磁盘还原表。
  log += restore_count_patch(mem, mb);
  if (!verify_runtime_130(mem, mb, log)) return log;

  std::map<std::string, std::string> d = parse_args(args);
  // 冲突检测: 三类互斥, 冲突回退 mon_type=0
  int given = 0;
  for (const char* k : {"mon_type", "assassin_type", "soldier_type", "type_table_rec"})
    if (d.count(k)) ++given;
  if (given >= 2) {
    log += "  [!!] 冲突: 同时指定多类, 按规则回退到 --mon_type 0 (狼)\n";
    std::string g = d.count("group") ? d["group"] : "1";
    d.clear();
    d["group"] = g;
  }
  int group = d.count("group") ? parse_int(d["group"]) : 1;  // 1=盟友不可控(默认)
  if (group != 0 && group != 1 && group != 2) {
    log += "  --group 须 0=队友 / 1=盟友 / 2=敌方\n";
    return log;
  }
  // --count N: 一次召唤最多造几只(0/缺省=不改, 用技能的原始数量, 烽火狼烟是 5)。
  int count_override = d.count("count") ? parse_int(d["count"]) : 0;
  if (count_override < 0 || count_override > 5) {
    log += "  --count 须 0-5 (0=用技能原始数量)\n";
    return log;
  }

  int slot = 0;
  int control_val = 0x7f;
  std::string label;
  if (d.count("soldier_type")) {
    int st = parse_int(d["soldier_type"]);
    if (st < 0 || st > 6) {
      log += "  --soldier_type 须 0-6: 0=随机秦兵, 1-6=普通兵/队长/大将/官员/法师/大法师\n";
      return log;
    }
    if (st == 0) {
      if (d.count("soldier_sub_type")) {
        log += "  [!!] 随机秦兵(soldier_type=0) 忽略 --soldier_sub_type (随机无子类)\n";
        return log;
      }
      // 随机秦兵: 清空槽10(名无匹配 -> 类别11秦兵随机) + slot=10(CC=10 善翼位置)
      restore_qin_backup(mem);
      patch_matcher(mem, mb, false, log);
      if (!cmd_cike(mem, mb, false, 0, log)) {
        log += "  [!!] 随机秦兵: 槽10 还原失败, 已中止\n";
        return log;
      }
      if (!clear_slot_type10_pair(mem, mb, log)) {
        log += "  [!!] 随机秦兵: 名字槽清空失败, 已中止\n";
        return log;
      }
      slot = 10;
      label = "随机秦兵 (游戏系统随机)";
    } else {
      int sub_idx = d.count("soldier_sub_type") ? parse_int(d["soldier_sub_type"]) : 0;
      const QinGroup& g = qin_groups()[static_cast<size_t>(st - 1)];
      if (d.count("soldier_sub_type") &&
          (sub_idx < 0 || sub_idx >= static_cast<int>(g.subs.size()))) {
        log += "  --soldier_sub_type " + std::to_string(sub_idx) + " 非法: 大类[" +
               std::to_string(st) + "]=" + g.name + " 该版本子类须 " + legal_subs(g) + "\n";
        return log;
      }
      label = std::string(g.name) + "/" + g.subs[static_cast<size_t>(sub_idx)].name;
      bool ok;
      if (st == 3 || st == 4 || st == 5)
        ok = summon_qin_longname(mem, st - 1, sub_idx, log);
      else
        ok = summon_qin_field0_tag(mem, st - 1, sub_idx, log);
      if (!ok) return log;
      slot = 10;
    }
  } else if (d.count("assassin_type")) {
    int at = parse_int(d["assassin_type"]);
    if (at != 0 && at != 1) {
      log += "  --assassin_type 须 0=弓箭 / 1=带刀\n";
      return log;
    }
    restore_qin_backup(mem);
    clear_stale_qin_field0_tags(mem, mb, log);
    patch_matcher(mem, mb, false, log);
    if (!cmd_cike(mem, mb, true, at, log)) {
      log += "  [!!] 刺客名字槽写入失败, 已中止 (空槽10 会像随机秦兵)\n";
      return log;
    }
    slot = 10;
    label = at == 0 ? "弓箭刺客" : "带刀刺客";
  } else if (d.count("type_table_rec")) {
    int rec = parse_int(d["type_table_rec"]);
    if (rec < 0 || rec >= static_cast<int>(kMaxRec)) {
      log += "  --type_table_rec 越界\n";
      return log;
    }
    restore_qin_backup(mem);
    clear_stale_qin_field0_tags(mem, mb, log);
    patch_matcher(mem, mb, false, log);
    uintptr_t row = mb + kTabRecOff + static_cast<uintptr_t>(rec) * kStride;
    uint32_t p0 = mem.read_u_int32(row);
    std::string preview = p0 ? r_str(mem, p0, 48) : "";
    if (!summon_type_table_by_rec(mem, mb, rec, log)) return log;
    slot = 10;
    label = preview.empty() ? ("类型表 rec" + std::to_string(rec)) : preview + " (rec" + std::to_string(rec) + ")";
  } else {
    int mt = d.count("mon_type") ? parse_int(d["mon_type"]) : 0;
    if (mt < 0 || mt > 9) {
      log += "  --mon_type 须 0-9 (1.30 仅 0-7 可用, 8朱厌/9善翼 复用槽10): ";
      log += "0=狼 1=虎 2=熊 3=白狼 4=木偶 5=狗 6=虎蛟 7=狍鸮\n";
      return log;
    }
    const MonMapEntry& e = kMonMap[mt];
    restore_qin_backup(mem);
    clear_stale_qin_field0_tags(mem, mb, log);
    patch_matcher(mem, mb, false, log);
    const bool beast_wild = d.count("beast_wild") && parse_int(d["beast_wild"]) != 0;
    if (beast_wild && (mt == 8 || mt == 9)) {
      auto picks = list_beast_type_table(mem, mb, mt);
      if (picks.empty()) {
        log += "  [!!] 类型表无含「" + std::string(mt == 8 ? "朱厌" : "善翼") +
               "」的 field[0] 记录\n";
        return log;
      }
      int sub = d.count("beast_sub_type") ? parse_int(d["beast_sub_type"]) : 0;
      if (sub < 0 || sub >= static_cast<int>(picks.size())) {
        log += "  [!!] --beast_sub_type 越界 (0.." + std::to_string(picks.size() - 1) + ")\n";
        return log;
      }
      const WildBeastRow& pick = picks[static_cast<size_t>(sub)];
      if (!summon_type_table_by_rec(mem, mb, pick.rec, log)) return log;
      slot = 10;
      label = pick.field0 + " (rec" + std::to_string(pick.rec) + ")";
    } else if (e.need_slot) {
      if (!write_slot_type10_pair(mem, mb, e.slot_name, log)) {
        log += "  [!!] 名字槽写入失败, 已中止\n";
        return log;
      }
      slot = e.cc;
      label = e.label;
    } else {
      slot = e.cc;
      label = e.label;
    }
  }

  // ---- 召唤补丁(游戏召唤时读取)----
  apply_tail_patches(mem, mb, slot, group, control_val, count_override, log);
  const char* group_txt =
      group == 0 ? "队友·可控" : group == 1 ? "盟友·不可控·被动攻击" : "敌方·Friend=-1";
  log += "\n  [召唤] 目标=" + label + ", group=" + std::to_string(group) + " (" + group_txt + ")";
  if (count_override > 0) log += ", 每次最多 " + std::to_string(count_override) + " 只";
  log += "\n";
  log += "  现在去游戏里按召唤键即可 (补丁对下次召唤生效)\n";
  return log;
}

}  // namespace summon_130
}  // namespace poq
