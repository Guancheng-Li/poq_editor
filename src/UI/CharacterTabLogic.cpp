// 人物修改 Tab —— 读写/校验/无敌·一击必杀锁定(计划 P3 #18.1 自 TabChar.cpp 拆出)。

#include <windows.h>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Core/Characters.h"
#include "Core/GameModder.h"
#include "UI/App.h"
#include "UI/AppCfgState.h"
#include "UI/AppCharState.h"

namespace poq {
// ============================================================================
// 属性修改(角色)逻辑(对应 Python main_window.py 的 _on_char_* / _lock_*)
// ============================================================================

// 无敌模式涉及的防御字段(普通防御 + 五行防御)。
const std::set<std::string>& def_labels() {
  static const std::set<std::string> s = {"普通防御", "金防御", "木防御",
                                          "水防御",   "火防御", "土防御"};
  return s;
}
// 一击必杀涉及的攻击字段(普通攻击 + 五行攻击,均为 pair)。
const std::set<std::string>& atk_labels() {
  static const std::set<std::string> s = {"普通攻击", "金攻击", "木攻击",
                                          "水攻击",   "火攻击", "土攻击"};
  return s;
}

namespace {

// 无敌/一击必杀模式下的字段锁定(与字段固有 writable 无关)。
bool is_char_mode_locked(const App& app, const std::string& label) {
  if (app.char_state().ck_invincible) {
    if (label == "生命" || label == "精气" || label == "闪避率" || def_labels().count(label))
      return true;
  }
  if (app.char_state().ck_kill) {
    if (label == "命中率" || atk_labels().count(label)) return true;
  }
  return false;
}

}  // namespace

bool is_char_field_editable(const App& app, const CharFieldCfg& field) {
  if (!field.writable) return false;
  return !is_char_mode_locked(app, field.label);
}

// 按 label 查 char_fields 索引(找不到返回 -1)。
int find_char_field(const std::string& label) {
  const auto& fs = char_fields();
  for (size_t i = 0; i < fs.size(); ++i)
    if (fs[i].label == label) return static_cast<int>(i);
  return -1;
}

// 为当前选中角色重建字段控件(从内存读取并填充)。
void rebuild_char_widgets(App& app) {
  if (app.char_state().char_names.empty() || app.char_state().char_selected < 0 ||
      static_cast<size_t>(app.char_state().char_selected) >= app.char_state().char_names.size()) {
    app.char_state().char_widgets.clear();
    return;
  }
  const std::string& name =
      app.char_state().char_names[static_cast<size_t>(app.char_state().char_selected)];
  auto it = app.char_state().char_addrs.find(name);
  if (it == app.char_state().char_addrs.end()) {
    app.char_state().char_widgets.clear();
    return;
  }
  uintptr_t base = it->second;
  app.char_state().char_widgets.assign(char_fields().size(), CharWidget());
  auto values = read_all_char(app.modder().mem(), base);
  for (size_t i = 0; i < char_fields().size(); ++i) {
    const auto& f = char_fields()[i];
    CharWidget& w = app.char_state().char_widgets[i];
    auto vit = values.find(f.label);
    if (vit == values.end()) continue;  // 读取失败: loaded 保持 false -> 不会被保存(计划 P1 #4)
    const CharValue& v = vit->second;
    if (f.kind == CharKind::StringPtr) {
      std::snprintf(w.strbuf, sizeof(w.strbuf), "%s", v.str.c_str());
    } else if (f.kind == CharKind::Pair) {
      w.pair_lo = v.lo;
      w.pair_hi = v.hi;
    } else {
      w.intval = v.lo;
    }
    w.loaded = true;  // 只有真正读到才允许保存
  }
}

// 读取全部队员:用固定偏移定位(按当前模式的引擎模块选偏移表),填充下拉框 + 全局属性。
void on_char_read(App& app) {
  if (!app.modder().mem().is_open()) {
    app.char_state().char_status = "请先点击\"初始化\"。";
    app.set_status("请先点击\"初始化\"。");
    return;
  }
  try {
    const std::string& module = app.cfg().cfg.current().hook.module;  // steam/v1.30
    auto located = locate_by_offsets(app.modder().mem(), app.modder().pid(), module,
                                     app.char_state().char_filter);
    if (app.hidden)
      app.char_state().char_slot_probe.clear();
    else
      app.char_state().char_slot_probe =
          probe_slot_names(app.modder().mem(), app.modder().pid(), module);
    if (located.empty()) {
      app.char_state().char_status = "未定位到队员(可尝试取消过滤后再次读取属性)。";
      app.set_status("未定位到队员(可尝试取消过滤后再次读取属性)。");
      return;
    }
    // 记住读取前选中的角色: 读取会刷新所有人, 但选中项不跳
    std::string prev_sel =
        (app.char_state().char_selected >= 0 &&
         static_cast<size_t>(app.char_state().char_selected) < app.char_state().char_names.size())
            ? app.char_state().char_names[static_cast<size_t>(app.char_state().char_selected)]
            : "";
    app.char_state().char_addrs.clear();
    app.char_state().char_names.clear();
    for (auto& kv : located) {
      app.char_state().char_addrs[kv.first] = kv.second;  // map 供按名查找
      app.char_state().char_names.push_back(kv.first);    // 槽位顺序保留下拉框
    }
    // 选中保持: 还在列表里就维持原选择; 不在(角色离队/被过滤)才默认第一项
    app.char_state().char_selected = 0;
    for (size_t i = 0; i < app.char_state().char_names.size(); ++i)
      if (app.char_state().char_names[i] == prev_sel) {
        app.char_state().char_selected = static_cast<int>(i);
        break;
      }
    // 全局属性:从第 1 人读
    const std::string& first = app.char_state().char_names.front();
    auto g = read_global_char(app.modder().mem(), app.char_state().char_addrs[first]);
    auto mit = g.find("金钱");
    if (mit != g.end())
      std::snprintf(app.char_state().money_buf, sizeof(app.char_state().money_buf), "%d",
                    mit->second.lo);
    rebuild_char_widgets(app);
    app.char_state().char_status = "已定位 " + std::to_string(located.size()) + " 人。";
    app.set_status("读取完成:定位 " + std::to_string(located.size()) + " 人。");
  } catch (const std::exception& ex) {
    app.char_state().char_status = "读取失败:" + sanitize(ex.what(), !app.hidden);
    app.set_status("读取失败:" + sanitize(ex.what(), !app.hidden));
  }
}

// 保存当前选中角色属性:收集 -> 校验(有非法则整体拒绝) -> 写入。
void on_char_save(App& app) {
  if (!app.modder().mem().is_open()) {
    app.set_status("请先点击\"初始化\"。");
    return;
  }
  if (app.char_state().char_addrs.empty()) {
    app.set_status("请先\"读取属性\"定位队员。");
    return;
  }
  if (app.char_state().char_selected < 0 ||
      static_cast<size_t>(app.char_state().char_selected) >= app.char_state().char_names.size()) {
    app.set_status("请先选择角色。");
    return;
  }
  const std::string& name =
      app.char_state().char_names[static_cast<size_t>(app.char_state().char_selected)];
  uintptr_t base = app.char_state().char_addrs[name];
  // 1) 收集字段值: 只收 "可写 且 读取成功" 的字段(计划 P1 #4)
  //    未加载的字段跳过 -> 不会把"读取失败"当成"值=0"写回去
  std::map<std::string, CharValue> values;
  int skipped_unloaded = 0;
  for (size_t i = 0; i < char_fields().size(); ++i) {
    const auto& f = char_fields()[i];
    const CharWidget& w = app.char_state().char_widgets[i];
    if (!f.writable || !w.loaded) {
      if (!f.writable || !w.loaded) ++skipped_unloaded;
      continue;
    }
    CharValue v;
    if (f.kind == CharKind::StringPtr) {
      v.str = w.strbuf;
    } else if (f.kind == CharKind::Pair) {
      v.is_pair = true;
      v.lo = w.pair_lo;
      v.hi = w.pair_hi;
    } else {
      v.lo = w.intval;
    }
    values[f.label] = v;
  }
  // 2) 校验收集到的字段(有非法则弹窗并拒绝,整次不写)
  std::vector<std::string> errors;
  for (const auto& f : char_fields()) {
    auto vit = values.find(f.label);
    if (vit == values.end()) continue;  // 未收集(只读/未加载)
    auto err = validate_value(f, vit->second);
    if (err) errors.push_back(*err);
  }
  if (!errors.empty()) {
    std::string msg = "以下字段不合法,未保存:\n\n";
    for (const auto& e : errors) msg += e + "\n";
    msg_box("校验失败", msg, MB_ICONERROR);
    app.set_status("校验失败,未保存:" + name + "。");
    return;
  }
  // 3) 校验通过,逐字段写入并统计失败(计划 P2 #11: 部分失败要如实报告)
  int saved = 0;
  std::vector<std::string> failed;
  for (const auto& f : char_fields()) {
    auto vit = values.find(f.label);
    if (vit == values.end()) continue;  // 未收集(只读/未加载)
    if (write_char_field(app.modder().mem(), base, f, vit->second))
      ++saved;
    else
      failed.push_back(f.label);
  }
  std::string tail_msg;
  if (skipped_unloaded > 0) {
    tail_msg = "\n\n跳过 " + std::to_string(skipped_unloaded) + " 个字段(只读或读取失败, 未写入)";
  }
  if (!failed.empty()) {
    std::string m = "成功写入 " + std::to_string(saved) + " 个,失败 " +
                    std::to_string(failed.size()) + " 个。\n失败字段:";
    for (const auto& f : failed) m += " " + f;
    m += tail_msg;
    m += "，移动速度和视野需要切换场景（进入任意房间）后生效";
    app.char_state().char_status = "保存部分失败:" + name;
    app.set_status("保存部分失败:" + name + "。");
    msg_box("保存完成(部分失败)", m, MB_ICONWARNING);
    return;
  }
  app.char_state().char_status =
      "保存完成:" + name + " " + std::to_string(saved) + " 个字段已写入。请在游戏中验证。";
  app.set_status("角色属性保存完成:" + name + "。");
  msg_box("保存完成",
          "请存档后重新读档查看变化，部分属性词条可能因道具属性加成与设定值不同" + tail_msg +
              "，移动速度和视野需要切换场景（进入任意房间）后生效",
          MB_ICONINFORMATION);
}

// 修改金钱按钮:校验后仅写第 1 人 +0x2C。
void on_money_apply(App& app) {
  if (!app.modder().mem().is_open() || app.char_state().char_addrs.empty()) {
    app.set_status("请先初始化并读取属性。");
    return;
  }
  const auto& gf = global_fields().front();  // 金钱
  std::string t = trim(app.char_state().money_buf);
  char* end = nullptr;
  long long val = std::strtoll(t.c_str(), &end, 10);
  if (end == t.c_str() || *end != '\0') {
    msg_box("输入错误", "金钱必须是整数(0 ~ 99999999)。", MB_ICONWARNING);
    return;
  }
  CharValue v;
  v.lo = static_cast<int>(val);
  auto err = validate_value(gf, v);
  if (err) {
    msg_box("输入错误", *err, MB_ICONWARNING);
    return;
  }
  const std::string& first = app.char_state().char_names.front();
  // 计划 P2(#12): 单字段路径也要检查写入结果, 不能忽略失败仍提示成功
  if (!write_char_field(app.modder().mem(), app.char_state().char_addrs[first], gf, v)) {
    app.set_status("金钱写入失败。");
    msg_box("写入失败", "金钱写入失败(目标进程可能已退出或页面不可写)。", MB_ICONERROR);
    return;
  }
  msg_box("金钱已修改", "修改后丢弃金钱、购买商品、捡起金钱，数额才会更新", MB_ICONINFORMATION);
}

// ---- 无敌 / 一击必杀锁 ----
// 立即把已启用锁的字段写为 9999(对所有队员,不等定时器)。
void lock_write_once(App& app) {
  if (!app.modder().mem().is_open() || app.char_state().char_addrs.empty()) return;
  if (!app.char_state().ck_invincible && !app.char_state().ck_kill) return;
  // 计划 260929 #13: 用户主动写入不能静默吞失败 —— 统计并上报(仍逐字段尽力写,
  // 单个字段失败不中断锁定)。
  int ok_n = 0, fail_n = 0;
  for (const auto& kv : app.char_state().char_addrs) {
    uintptr_t base = kv.second;
    try {
      if (app.char_state().ck_invincible) {
        app.modder().mem().write_u_int32(base + 0x30, 9999);  // 当前生命
        app.modder().mem().write_u_int32(base + 0x38, 9999);  // 当前精气
        for (const auto& f : char_fields()) {
          if (def_labels().count(f.label) || f.label == "闪避率") {
            CharValue v;
            v.lo = 9999;
            if (write_char_field(app.modder().mem(), base, f, v))
              ++ok_n;
            else
              ++fail_n;
          }
        }
      }
      if (app.char_state().ck_kill) {
        for (const auto& f : char_fields()) {
          if (atk_labels().count(f.label)) {
            CharValue v;
            v.is_pair = true;
            v.lo = 9999;
            v.hi = 9999;
            if (write_char_field(app.modder().mem(), base, f, v))
              ++ok_n;
            else
              ++fail_n;
          } else if (f.label == "命中率") {
            CharValue v;
            v.lo = 9999;
            if (write_char_field(app.modder().mem(), base, f, v))
              ++ok_n;
            else
              ++fail_n;
          }
        }
      }
    } catch (const std::exception&) {
      ++fail_n;  // 该角色整体不可写(如已退出地图), 继续其余角色
    }
  }
  if (fail_n > 0)
    app.set_status("锁定写入: 成功 " + std::to_string(ok_n) + " 个, 失败 " +
                   std::to_string(fail_n) + " 个(角色可能未加载)");
}

// 无敌模式勾选/取消。
void on_invincible_toggled(App& app, bool checked) {
  if (checked) {
    // 缓存防御/闪避原值(所有队员)
    app.char_state().def_cache.clear();
    for (const auto& kv : app.char_state().char_addrs) {
      for (const auto& f : char_fields()) {
        if (def_labels().count(f.label) || f.label == "闪避率") {
          auto v = read_char_field(app.modder().mem(), kv.second, f);
          if (v) app.char_state().def_cache[kv.first][f.label] = *v;
        }
      }
    }
    lock_write_once(app);  // 立即写 9999
  } else {
    // 取消:生命=最大生命、精气=最大精气、防御+闪避=缓存值
    for (const auto& kv : app.char_state().char_addrs) {
      uintptr_t base = kv.second;
      try {
        unsigned int mx_hp = app.modder().mem().read_u_int32(base + 0x34);
        app.modder().mem().write_u_int32(base + 0x30, mx_hp);
        unsigned int mx_mp = app.modder().mem().read_u_int32(base + 0x3C);
        app.modder().mem().write_u_int32(base + 0x38, mx_mp);
        for (const auto& f : char_fields()) {
          if (def_labels().count(f.label) || f.label == "闪避率") {
            auto cit = app.char_state().def_cache.find(kv.first);
            if (cit != app.char_state().def_cache.end()) {
              auto fit = cit->second.find(f.label);
              if (fit != cit->second.end())
                write_char_field(app.modder().mem(), base, f, fit->second);
            }
          }
        }
      } catch (const std::exception&) {
        app.set_status("解除无敌: 角色 " + kv.first + " 恢复失败(生命/精气未还原)");
      }
    }
    app.char_state().def_cache.clear();
  }
}

// 一击必杀勾选/取消。
void on_kill_toggled(App& app, bool checked) {
  if (checked) {
    // 缓存攻击/命中率原值(所有队员)
    app.char_state().hit_cache.clear();
    for (const auto& kv : app.char_state().char_addrs) {
      for (const auto& f : char_fields()) {
        if (atk_labels().count(f.label) || f.label == "命中率") {
          auto v = read_char_field(app.modder().mem(), kv.second, f);
          if (v) app.char_state().hit_cache[kv.first][f.label] = *v;
        }
      }
    }
    lock_write_once(app);  // 立即写 9999
  } else {
    // 取消:攻击/命中率恢复原值
    for (const auto& kv : app.char_state().char_addrs) {
      uintptr_t base = kv.second;
      try {
        for (const auto& f : char_fields()) {
          if (atk_labels().count(f.label) || f.label == "命中率") {
            auto cit = app.char_state().hit_cache.find(kv.first);
            if (cit != app.char_state().hit_cache.end()) {
              auto fit = cit->second.find(f.label);
              if (fit != cit->second.end())
                write_char_field(app.modder().mem(), base, f, fit->second);
            }
          }
        }
      } catch (const std::exception&) {
        app.set_status("解除一击必杀: 角色 " + kv.first + " 攻击/命中率恢复失败");
      }
    }
    app.char_state().hit_cache.clear();
  }
}

// 锁定时器:每 500ms 把已启用锁的字段写回 9999(对所有队员)。
void tick_lock(App& app, float dt) {
  if (!app.char_state().ck_invincible && !app.char_state().ck_kill) {
    app.char_state().lock_accum = 0.0f;
    return;
  }
  app.char_state().lock_accum += dt;
  if (app.char_state().lock_accum < 0.5f) return;
  app.char_state().lock_accum = 0.0f;
  lock_write_once(app);
}
}  // namespace poq
