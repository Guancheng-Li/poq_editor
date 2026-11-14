#include "Core/SummonSteamInternal.h"
#include <windows.h>
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

namespace poq {
namespace summon_steam {
namespace detail {


// ---------------------------------------------------------------------------
// 常量(对齐 summon_tool_steam.py)
// ---------------------------------------------------------------------------
constexpr uintptr_t kOffFriendPush = 0x35BAA;  // 召唤强制 Friend imm(push 1 的 01)
constexpr uintptr_t kOffBound = 0x3586D;       // 召唤类型上限 cmp eax,imm
constexpr uintptr_t kOffControlCmp = 0x35C32;  // 可控性 cmp [flag],imm / 跳过注册
constexpr uintptr_t kOffType = 0x3585F;        // 强制召唤类型 mov eax,slot+1
constexpr uintptr_t kOffMatcherImm = 0x6FAC6;  // 名字匹配器 push 0/1
// 数据偏移(EasyRPG.dll 镜像内, 2026-10-16 ASLR 实测; 括号为旧绝对地址快照)
constexpr uintptr_t kOffNamePaoXiao = 0x2371A4;  // 名字槽 slot8(狍鸮)   [旧 0x539771A4]
constexpr uintptr_t kOffNameZhuYan = 0x2371B0;   // 名字槽 slot9(朱厌源) [旧 0x539771B0]
// kOffNameShanYi 见 SummonSteamInternal.h
constexpr uintptr_t kOffRecBow = 0x4DF5C0;       // 刺客弓记录  [旧 0x53C1F5C0]
constexpr uintptr_t kOffRecKnife = 0x4DF700;     // 刺客刀记录  [旧 0x53C1F700]
constexpr uintptr_t kOffQinTable = 0x4BAB78;     // 秦兵类型表  [旧 0x53bfab78]
constexpr uintptr_t kOffQinNullPtr = 0x4BA410;   // 空指针哨兵  [旧 0x53bfa410]
// kOffFlag 见 SummonSteamInternal.h
constexpr uint32_t kQinStride = 0x140;
constexpr uint32_t kRecBowIdx = 469;  // (kOffRecBow-8-kOffQinTable)/kQinStride, 刀=+1
constexpr int kMaxRec = 600;

// 模块基址(cmd_summon 入口设置; 同 Summon130.cpp 的 g_mb 模式): rt(偏移)=运行时地址
uintptr_t g_mb = 0;
uint32_t g_flag = 0;

// mon_type 映射ID -> 程序槽位(映射ID 3=白狼 跳过鸟=槽3,即 [0,1,2,4,5,6,7,8,9,10])
const int kMonSlots[10] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10};
const char* kMonNames[10] = {"狼", "虎",   "熊",   "白狼", "木偶",
                             "狗", "虎蛟", "狍鸮", "朱厌", "善翼"};
// 野兽槽位名(slot 0-10)
const char* kBeastSlots[11] = {"狼", "虎",   "熊",   "鸟",   "白狼", "木偶",
                               "狗", "虎蛟", "狍鸮", "朱厌", "善翼"};

// 六大类 (soldier_type 1-6):(大类名, 短名, [(子类名, 长名包含子串)...]).
// 队长大类的 '旧重剑' 不支持召唤:工具层数据完整保留(UI 层不显示,对齐 task7 #3.3.1)。
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
  return std::wstring(tmp) + L"poq_summon_qin_backup.json";
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

// ---------------------------------------------------------------------------
// 补丁(写后回读校验,失败仅记日志,与 python patch_bytes 一致)
// ---------------------------------------------------------------------------

// 刺客名字槽: on=克隆刺客长名写进朱厌/善翼槽; off=写回原版野兽长名。
void cmd_cike(ProcessMemory& mem, bool on, std::string& log) {
  if (on) {
    std::vector<unsigned char> bow = mem.read(mem.read_u_int32(rt(kOffRecBow)), 12);
    std::vector<unsigned char> knife = mem.read(mem.read_u_int32(rt(kOffRecKnife)), 12);
    mem.write(rt(kOffNameShanYi), bow);    // 善翼槽 -> 克隆刺客弓
    mem.write(rt(kOffNameZhuYan), knife);  // 朱厌槽 -> 克隆刺客刀
    log += "  刺客名字槽已打: 0x" + hex8(rt(kOffNameShanYi)) + "=克隆刺客弓, 0x" +
           hex8(rt(kOffNameZhuYan)) + "=克隆刺客刀\n";
    log += "  用法: type 10 -> cike_bow(弓); type 9 -> cike_knife(刀)\n";
  } else {
    // 直接写回原版野兽长名(不依赖备份文件): 只写 3 个槽地址, 不碰槽外数据
    struct SlotName {
      uintptr_t addr;
      const char* name;
    };
    const SlotName kRestore[] = {{rt(kOffNamePaoXiao), "召唤兽狍鸮"},
                                 {rt(kOffNameZhuYan), "召唤兽朱厌"},
                                 {rt(kOffNameShanYi), "召唤兽善翼"}};
    for (const auto& s : kRestore) {
      std::string g = utf8_to_gbk(s.name);
      g.push_back('\0');
      g.push_back('\0');  // 见 write_slot10:不可用 +"\x00\x00" 拼接
      mem.write(s.addr, std::vector<unsigned char>(g.begin(), g.end()));
    }
    log += "  刺客名字槽已还原: slot8=狍鸮 slot9=朱厌 slot10=善翼 (原版野兽长名)\n";
  }
}

// ---------------------------------------------------------------------------
// 秦兵类型表备份(写 %TEMP%\poq_summon_qin_backup.json)
// ---------------------------------------------------------------------------
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
    item.set("off", json::Value::make_number(off));
    item.set("v", json::Value::make_number(static_cast<double>(v)));
    recs.set(std::to_string(r), std::move(item));  // 键 = rec(steam 版)
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
    // 加固(2026-10-16 召唤卡死排查): 备份可能跨游戏会话残留 —— 新会话里类型表
    // 是全新的(field[0] 本来就是原值), 此时还原无害, 但**释放旧 scratch 地址极
    // 危险**: 那块内存可能已被别的东西占用, VirtualFreeEx 掉它 = 破坏游戏内存。
    // 判据: 仅当 field 值仍指向旧 scratch 区间 [scratch, scratch+0x4000)(重定向
    // 还在)才允许释放; 否则跳过释放(宁可泄漏 16KB 的死地址, 不冒释放别人的险)。
    bool redirect_active = false;
    if (scratch) {
      for (const auto& kv : recs->object) {
        double off = 4;
        if (kv.second.is_object()) {
          if (const json::Value* x = kv.second.find("off")) off = x->number;
        }
        int rec = std::atoi(kv.first.c_str());
        try {
          uint32_t cur =
              mem.read_u_int32(rt(kOffQinTable) + 8 + static_cast<uintptr_t>(rec) * kQinStride +
                               static_cast<uintptr_t>(off));
          if (cur >= scratch && cur < scratch + 0x4000) {
            redirect_active = true;
            break;
          }
        } catch (...) {
        }
      }
    }
    int n = 0;
    for (const auto& kv : recs->object) {
      double off = 4, v = 0;
      if (kv.second.is_object()) {
        if (const json::Value* x = kv.second.find("off")) off = x->number;
        if (const json::Value* x = kv.second.find("v")) v = x->number;
      } else if (kv.second.is_number()) {
        v = kv.second.number;  // 旧格式: 裸指针 = field[1]
      }
      int rec = std::atoi(kv.first.c_str());
      try {
        mem.write_u_int32(rt(kOffQinTable) + 8 + static_cast<uintptr_t>(rec) * kQinStride +
                              static_cast<uintptr_t>(off),
                          static_cast<uint32_t>(v));
        ++n;
      } catch (...) {
      }
    }
    if (scratch && redirect_active) {
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
    cnt = mem.read_u_int32(rt(kOffQinTable));
  } catch (...) {
    cnt = 0;
  }
  uint32_t limit = cnt < kMaxRec ? cnt : kMaxRec;
  for (uint32_t i = 0; i < limit; ++i) {
    uintptr_t base = rt(kOffQinTable) + 8 + static_cast<uintptr_t>(i) * kQinStride;
    uint32_t p0 = 0, p1 = 0;
    try {
      p0 = mem.read_u_int32(base);
      p1 = mem.read_u_int32(base + 4);
    } catch (...) {
      continue;
    }
    if (!p0 || !p1 || p0 == rt(kOffQinNullPtr) || p1 == rt(kOffQinNullPtr)) continue;
    if (!(0x20000000 <= p0 && p0 < 0x90000000) || !(0x20000000 <= p1 && p1 < 0x90000000)) continue;
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

// 写槽10(善翼源)名字。
// 注意:结尾 \0 必须用 push_back/append 追加——`std::string + "\x00"` 会把 C 字符串
// "\x00" 按 strlen(=0) 拼接,导致终止符丢失,槽10 残留旧名导致自校验失败/召唤错怪。
void write_slot10(ProcessMemory& mem, const std::string& name, std::string& log) {
  (void)log;  // 该函数不产生日志(调用方自行记录自校验结果)
  std::string g = utf8_to_gbk(name);
  g.push_back('\0');
  mem.write(rt(kOffNameShanYi), std::vector<unsigned char>(g.begin(), g.end()));
}

// 1/2/6 大类(长名>10B): 目标子类第一条 field[0] 重定向 scratch 标签 Q###, 槽10=Q###.
bool summon_qin_field0_tag(ProcessMemory& mem, uintptr_t mb, int group_idx, int sub_idx,
                           std::string& log) {
  // 先还原上次 field[0] 残留,否则本次 resolve 扫表会读到上次的标签指针(或已释放
  // scratch)导致"无匹配记录"、召唤仍是上次的怪。(python 仅无匹配时才 restore,这里
  // 提前到开头更稳健:连续召唤同子类也能命中)
  restore_qin_backup(mem);
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
  uintptr_t base = rt(kOffQinTable) + 8 + static_cast<uintptr_t>(rec) * kQinStride;
  uint32_t orig = mem.read_u_int32(base);
  std::string s0 = r_str(mem, orig, 32);
  mem.write_u_int32(base, static_cast<uint32_t>(tp));  // field[0] -> 唯一标签
  patch_matcher(mem, mb, false, log);                  // 匹配器保持 field[0] (0)
  write_slot10(mem, tag, log);                         // 槽10 = 标签
  save_qin_backup(mem, {{rec, 0, orig}}, scr.base());
  // 自校验 (防 restore 残留导致 0 命中)
  uint8_t imm = 0;
  std::string s10, s0now;
  try {
    imm = mem.read(mb + kOffMatcherImm, 1)[0];
    s10 = r_str(mem, rt(kOffNameShanYi), 8);
    s0now = r_str(mem, mem.read_u_int32(base), 32);
  } catch (const std::exception& e) {
    log += std::string("  [!!] 自校验读取异常: ") + e.what() + "\n";
  }
  if (imm != 0 || s10 != tag || s0now != tag) {
    log += "  [!!] 自校验失败 matcher=" + std::to_string(imm) + " 槽10=" + s10 + " f0=" + s0now +
           " (期望 0/" + tag + ")";
    // 附原始字节,便于定位写入是否落盘
    try {
      auto b10 = mem.read(rt(kOffNameShanYi), 8);
      auto bf = mem.read(mem.read_u_int32(base), 8);
      log += " 槽10hex=" + hex8_bytes(b10) + " f0hex=" + hex8_bytes(bf);
    } catch (...) {
    }
    log += "\n";
    return false;
  }
  log += std::string("  [秦兵·") + g.name + "/" + g.subs[static_cast<size_t>(sub_idx)].name +
         "] rec" + std::to_string(rec) + " (" + s0 + ") -> field[0]=" + tag + ", 槽10=" + tag +
         " ✓ 单命中必中 (匹配器保持0)\n";
  return true;
}

// 3/4/5 大类(长名<=10B): 长名单直接放槽10, 前缀匹配单命中.
// 长名>10B 返回 false(调用方直接 return, 与 python 返回 None 行为一致)。
bool summon_qin_longname(ProcessMemory& mem, uintptr_t mb, int group_idx, int sub_idx,
                         std::string& log) {
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
  uint32_t p0 = mem.read_u_int32(rt(kOffQinTable) + 8 + static_cast<uintptr_t>(rec) * kQinStride);
  std::string long_name = r_str(mem, p0, 32);
  if (utf8_to_gbk(long_name).size() > 10) return false;  // 长名放不进槽10
  restore_qin_backup(mem);
  patch_matcher(mem, mb, false, log);
  write_slot10(mem, long_name, log);
  uint8_t imm = 0;
  std::string s10;
  try {
    imm = mem.read(mb + kOffMatcherImm, 1)[0];
    s10 = r_str(mem, rt(kOffNameShanYi), 12);
  } catch (...) {
  }
  if (imm != 0 || s10 != long_name) {
    log += "  [!!] 自校验失败 matcher=" + std::to_string(imm) + " 槽10=" + s10 + " (期望 0/" +
           long_name + ")\n";
    return false;
  }
  log += std::string("  [秦兵·") + g.name + "/" + g.subs[static_cast<size_t>(sub_idx)].name +
         "] rec" + std::to_string(rec) + " 长名=" + long_name + " -> 槽10 ✓ 单命中必中\n";
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

// 运行时定位自检:偏移过期(游戏更新)时立刻中止,而不是把补丁写进无关内存崩游戏
// —— 这正是本次 ASLR 修复要根除的故障模式,所以入口必须先验证。
bool verify_runtime(ProcessMemory& mem, std::string& log) {
  log += "  EasyRPG.dll 基址 = 0x" + hex8(g_mb) + "\n";
  try {
    // 控制标志实测:权威来源 = MB+0x3585F 的原版 `A1 <imm32>`(mov eax,[flag],本工具
    // 打补丁前从不改写它, 游戏重定位已把立即数填成运行时真值)。MB+0x35C32 的
    // cmp [flag],imm 会被本工具自己覆盖, 只能作辅证不能作准。
    auto t = mem.read(g_mb + kOffType, 5);
    if (t[0] == 0xA1) {
      uint32_t tflag = t[1] | (t[2] << 8) | (t[3] << 16) | (t[4] << 24);
      if (tflag != rt(kOffFlag)) {
        log += "  [!!] 控制标志实测 0x" + hex8(tflag) + " != 偏移推导 0x" + hex8(rt(kOffFlag)) +
               " —— 以实测(A1)为准\n";
      }
      g_flag = tflag;
    } else {
      g_flag = static_cast<uint32_t>(rt(kOffFlag));
      auto op = mem.read(g_mb + kOffControlCmp, 7);
      if (op[0] == 0x83 && op[1] == 0x3D) {
        uint32_t observed = op[2] | (op[3] << 8) | (op[4] << 16) | (op[5] << 24);
        if (observed != g_flag)
          log += "  [??] cmp 现场地址 0x" + hex8(observed) + " != 推导 0x" + hex8(g_flag) +
                 " (mov 指令已被补丁覆盖, 无法交叉验证, 保持推导)\n";
      }
    }
    uint32_t cnt = mem.read_u_int32(rt(kOffQinTable));
    if (cnt == 0 || cnt > 10000) {
      log += "  [!!] 秦兵表 cnt=" + std::to_string(cnt) + " 异常 —— 偏移已过期, 中止\n";
      return false;
    }
    uint32_t p0 =
        mem.read_u_int32(rt(kOffQinTable) + 8 + static_cast<uintptr_t>(kRecBowIdx) * kQinStride);
    std::string s0 = r_str(mem, p0, 32);
    log += "  秦兵表 cnt=" + std::to_string(cnt) + ", rec" + std::to_string(kRecBowIdx) +
           " 长名='" + s0 + "'";
    if (s0.find("刺客弓") == std::string::npos) {
      log += " (期望含'刺客弓') [!!] 偏移已过期, 中止\n";
      return false;
    }
    log += " ✓ 运行时定位自检通过\n";
    return true;
  } catch (const std::exception& e) {
    log += std::string("  [!!] 运行时定位自检读取失败: ") + e.what() + " —— 中止\n";
    return false;
  }
}

}  // namespace detail
}  // namespace summon_steam
}  // namespace poq
