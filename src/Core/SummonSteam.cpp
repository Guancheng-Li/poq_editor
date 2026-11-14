// 召唤兽修改 —— Steam 版 cmd_summon 入口。
#include "Core/SummonSteam.h"
#include "Core/SummonSteamInternal.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "Core/TextCodec.h"

namespace poq {
namespace summon_steam {

namespace {
/// 读远端 GBK 字符串(遇到 NUL 或读满 maxn), 返回 UTF-8; 失败返回空串。
std::string read_gbk(ProcessMemory& mem, uintptr_t addr, int maxn) {
  std::string raw;
  try {
    const std::vector<unsigned char> b = mem.read(addr, maxn);
    for (unsigned char c : b) {
      if (c == 0) break;
      raw.push_back(static_cast<char>(c));
    }
  } catch (...) {
    return std::string();
  }
  return raw.empty() ? std::string() : gbk_to_utf8(raw);
}
}  // namespace

using detail::QinGroup;
using detail::cmd_cike;
using detail::g_mb;
using detail::kOffNameShanYi;
using detail::kMonNames;
using detail::kMonSlots;
using detail::legal_subs;
using detail::parse_args;
using detail::parse_int;
using detail::patch_bound;
using detail::patch_control;
using detail::patch_friend;
using detail::patch_matcher;
using detail::patch_skip_member;
using detail::patch_type;
using detail::qin_groups;
using detail::restore_qin_backup;
using detail::rt;
using detail::summon_qin_field0_tag;
using detail::summon_qin_longname;
using detail::verify_runtime;

// ---------------------------------------------------------------------------
// 类型表扫描(供"召唤其它(NPC/类型表)"列表; Steam 偏移见 SummonSteamInternal.h)
// 不做任何过滤: 能读出名字的记录都返回, 由 UI 的模糊搜索筛选。
// ---------------------------------------------------------------------------
std::vector<summon_130::WildBeastRow> list_misc_type_table(ProcessMemory& mem,
                                                           uintptr_t module_base) {
  std::vector<summon_130::WildBeastRow> out;
  if (module_base == 0) return out;
  g_mb = module_base;
  int cnt = 0;
  try {
    cnt = static_cast<int>(mem.read_u_int32(rt(detail::kTabCountOff)));
  } catch (...) {
    return out;
  }
  if (cnt <= 0 || cnt > detail::kTabMaxRec) cnt = detail::kTabMaxRec;
  for (int rec = 0; rec < cnt; ++rec) {
    const uintptr_t row =
        rt(detail::kTabRecOff) + static_cast<uintptr_t>(rec) * detail::kTabStride;
    uint32_t p0 = 0, p1 = 0;
    try {
      p0 = mem.read_u_int32(row);
      p1 = mem.read_u_int32(row + 4);
    } catch (...) {
      continue;
    }
    if (p0 < 0x10000 && p1 < 0x10000) continue;
    summon_130::WildBeastRow r;
    r.rec = rec;
    if (p0 >= 0x10000) r.field0 = read_gbk(mem, p0, 48);
    if (p1 >= 0x10000) r.field1 = read_gbk(mem, p1, 24);
    if (r.field0.empty() && r.field1.empty()) continue;
    r.gbk_len = static_cast<int>(utf8_to_gbk(r.field0).size());
    out.push_back(std::move(r));
  }
  std::sort(out.begin(), out.end(),
            [](const summon_130::WildBeastRow& a, const summon_130::WildBeastRow& b) {
              if (a.field0 != b.field0) return a.field0 < b.field0;
              if (a.field1 != b.field1) return a.field1 < b.field1;
              return a.rec < b.rec;
            });
  return out;
}

// ---------------------------------------------------------------------------
// 公开接口:cmd_summon(对齐 python)
// ---------------------------------------------------------------------------
std::string cmd_summon(ProcessMemory& mem, uintptr_t mb, const std::vector<std::string>& args) {
  std::string log;
  if (mb == 0) {
    throw std::runtime_error("Steam 版模块基址无效(EasyRPG.dll 未找到?)");
  }
  g_mb = mb;
  if (!verify_runtime(mem, log)) return log;

  std::map<std::string, std::string> d = parse_args(args);
  // 冲突检测: 三类互斥, 冲突回退 mon_type=0
  int given = 0;
  for (const char* k : {"mon_type", "assassin_type", "soldier_type", "type_table_rec"})
    if (d.count(k)) ++given;
  if (given >= 2) {
    log += "  [!!] 冲突: 同时指定多类, 按规则回退到 --mon_type 0 (狼)\n";
    std::string g = d.count("group") ? d["group"] : "0";
    d.clear();
    d["group"] = g;
  }
  int group = d.count("group") ? parse_int(d["group"]) : 0;  // 0=队友可控(默认)
  if (group != 0 && group != 1) {
    log += "  --group 须 0=队友可控 / 1=盟友不可控\n";
    return log;
  }

  int slot = 0;
  std::string label;
  if (d.count("soldier_type")) {
    int st = parse_int(d["soldier_type"]);
    if (st < 0 || st > 6) {
      log += "  --soldier_type 须 0-6: 0=随机秦兵, 1-6=普通兵/队长/大将/官员/法师/大法师\n";
      return log;
    }
    if (st == 0) {
      if (d.count("soldier_sub_type")) {
        log += "  [!!] 随机秦兵(soldier_type=0) 不接受 --soldier_sub_type (无大类子类)\n";
        return log;
      }
      // 复刻类别11路径: cmd_cike off + 槽10, 匹配器 field[0] (游戏系统随机)
      restore_qin_backup(mem);
      patch_matcher(mem, mb, false, log);
      cmd_cike(mem, false, log);
      mem.write(rt(kOffNameShanYi), std::vector<unsigned char>(12, 0));  // 清空善翼槽
      slot = 10;
      label = "随机秦兵 (类别11路径, 游戏系统随机)";
    } else {
      int sub_idx = d.count("soldier_sub_type") ? parse_int(d["soldier_sub_type"]) : 0;
      const QinGroup& g = qin_groups()[static_cast<size_t>(st - 1)];
      if (d.count("soldier_sub_type") &&
          (sub_idx < 0 || sub_idx >= static_cast<int>(g.subs.size()))) {
        log += "  --soldier_sub_type " + std::to_string(sub_idx) + " 非法: 大类[" +
               std::to_string(st) + "]=" + g.name + " 子类须 " + legal_subs(g) + "\n";
        return log;
      }
      label = std::string(g.name) + "/" + g.subs[static_cast<size_t>(sub_idx)].name;
      // 全部 1-6 都走 field[0] 匹配: 长名<=10B(3/4/5)直接放槽; >10B(1/2/6)重定向标签
      bool ok;
      if (st == 3 || st == 4 || st == 5)
        ok = summon_qin_longname(mem, mb, st - 1, sub_idx, log);
      else
        ok = summon_qin_field0_tag(mem, mb, st - 1, sub_idx, log);
      if (!ok) return log;
      slot = 10;
    }
  } else if (d.count("assassin_type")) {
    int at = parse_int(d["assassin_type"]);
    if (at != 0 && at != 1) {
      log += "  --assassin_type 须 0=弓箭刺客 / 1=带刀刺客\n";
      return log;
    }
    restore_qin_backup(mem);
    patch_matcher(mem, mb, false, log);
    cmd_cike(mem, true, log);
    slot = (at == 0) ? 10 : 9;  // 弓箭=善翼槽10, 带刀=朱厌槽9
    label = (at == 0) ? "弓箭刺客 (cike_bow)" : "带刀刺客 (cike_knife)";
  } else if (d.count("type_table_rec")) {
    // 召唤类型表里任意记录(Steam 版): 把该记录 field[0] 长名写进召唤名槽(槽10),
    // 引擎按名字命中那条记录 -> 属性/元素/技能都来自它自己的模板。
    const int rec = parse_int(d["type_table_rec"]);
    if (rec < 0 || rec >= detail::kTabMaxRec) {
      log += "  --type_table_rec 须 0-" + std::to_string(detail::kTabMaxRec - 1) + "\n";
      return log;
    }
    restore_qin_backup(mem);
    patch_matcher(mem, mb, false, log);  // 按 field[0] 长名匹配(原版行为)
    cmd_cike(mem, false, log);           // 关掉刺客改名, 免得占用名字槽
    const uintptr_t row = rt(detail::kTabRecOff) + static_cast<uintptr_t>(rec) * detail::kTabStride;
    uint32_t p0 = 0;
    try {
      p0 = mem.read_u_int32(row);
    } catch (...) {
      p0 = 0;
    }
    if (p0 < 0x10000) {
      log += "  [!!] 类型表 rec" + std::to_string(rec) + " field[0] 无效(空记录)\n";
      return log;
    }
    const std::string name = read_gbk(mem, p0, 48);
    if (name.empty()) {
      log += "  [!!] 类型表 rec" + std::to_string(rec) + " 长名为空\n";
      return log;
    }
    const std::string gbk = utf8_to_gbk(name);
    if (gbk.empty() || gbk.size() > 10) {
      log += "  [!!] 「" + name + "」GBK " + std::to_string(gbk.size()) +
             " 字节 > 10, 放不进召唤名槽; Steam 版不做改名(避免残留), 换个短名目标\n";
      return log;
    }
    std::vector<unsigned char> slotbuf(12, 0);
    for (size_t i = 0; i < gbk.size() && i < 11; ++i) {
      slotbuf[i] = static_cast<unsigned char>(gbk[i]);
    }
    try {
      mem.write(rt(kOffNameShanYi), slotbuf);
    } catch (const std::exception& e) {
      log += std::string("  [!!] 写名字槽失败: ") + e.what() + "\n";
      return log;
    }
    const std::string back = read_gbk(mem, rt(kOffNameShanYi), 12);
    log += "  [类型表] rec" + std::to_string(rec) + " 长名=" + name + " -> 槽10=" + back +
           (back == name ? " ✓" : " ✗(不一致)") + "\n";
    if (back != name) return log;
    slot = 10;
    label = name + " (rec" + std::to_string(rec) + ")";
  } else {
    int mt = d.count("mon_type") ? parse_int(d["mon_type"]) : 0;
    if (mt < 0 || mt > 9) {
      log +=
          "  --mon_type 须 0-9: 0=狼 1=虎 2=熊 3=白狼 4=木偶 5=狗 6=虎蛟 7=狍鸮 "
          "8=朱厌 9=善翼\n";
      return log;
    }
    restore_qin_backup(mem);
    patch_matcher(mem, mb, false, log);
    cmd_cike(mem, false, log);
    slot = kMonSlots[mt];
    label = kMonNames[mt];
  }

  // ---- 召唤补丁(游戏召唤时读取);全部类型放行 = enable 最大召唤数量9(task7 #8)----
  patch_type(mem, mb, slot, log);
  patch_bound(mem, mb, 0x7f, log);
  patch_control(mem, mb, 0x7f, log);
  patch_friend(mem, mb, 1, log);
  patch_skip_member(mem, mb, group == 1, log);
  log += "\n  [召唤] 目标=" + label + ", group=" + std::to_string(group) + " (" +
         (group == 0 ? "队友·可控" : "盟友·不可控·被动攻击敌人") + ")\n";
  log += "  现在去游戏里按召唤键即可 (补丁对下次召唤生效)\n";
  return log;
}


}  // namespace summon_steam
}  // namespace poq
