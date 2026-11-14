// 召唤 Tab —— 进程解析与激活/取消/刷新。

#include "UI/SummonTabLogic.h"
#include <stdexcept>
#include <string>
#include <vector>
#include "Core/GameModder.h"
#include "Core/OffsetsConfig.h"
#include "Core/Summon130.h"
#include "Core/SummonSteam.h"
#include "Native/ProcessMemory.h"
#include "UI/App.h"
#include "UI/AppCfgState.h"

namespace poq {

namespace {

std::string format_summon_args(const std::vector<std::string>& args) {
  std::string s;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) s += ' ';
    s += args[i];
  }
  return s;
}

}  // namespace

void append_summon_log(App& app, const std::string& text) {
  if (text.empty()) return;
  if (!app.summon_log.empty()) app.summon_log += "\n";
  app.summon_log += text;
}

// 调用对应版本工具模块(进程内,不调 python)。
// 进程 pid:已初始化(modder 打开)优先用 pid();未初始化用顶部下拉选中的进程(对齐 python
// summon_tab._resolve_pid)并临时 OpenProcess(对齐 python 每次自开 mem)。版本检查在下面:
// 所选版本的模块找不到 -> 弹窗提示版本不符。
std::string run_summon_tool(App& app, const std::vector<std::string>& args) {
  unsigned int pid = 0;
  ProcessMemory tmp_mem;  // 未初始化时的临时句柄(作用域结束自动关闭)
  ProcessMemory* mem = nullptr;
  if (app.modder().mem().is_open()) {
    pid = app.modder().pid();
    mem = &app.modder().mem();
  } else {
    auto sel = try_get_selected_pid(app);
    if (!sel) {
      throw std::runtime_error("未选择进程：请先在顶部选择 POQ.EXE 并初始化。");
    }
    pid = *sel;
    if (!tmp_mem.open(pid)) {
      throw std::runtime_error("OpenProcess 失败（请以管理员身份运行）。");
    }
    mem = &tmp_mem;
  }
  const ModeCfg& m = app.cfg().cfg.current();
  auto base_opt = ProcessMemory::find_module_base(pid, m.hook.module);
  if (!base_opt) {
    throw std::runtime_error(
        "未找到模块 " + m.hook.module + "：当前运行的游戏版本与所选版本（" + m.name +
        "）不符，或游戏未运行。请确认游戏正在运行，并在顶部选择正确的游戏版本后重新初始化。");
  }
  // 同名 UI 参数; 实现完全分离 —— Steam→SummonSteam / v1.30→Summon130, 互不调用。
  if (app.cfg().cfg.current_mode == "mode1") return summon_steam::cmd_summon(*mem, *base_opt, args);
  return summon_130::cmd_summon(*mem, *base_opt, args);
}

void on_summon_cancel(App& app);  // 前向声明(on_summon_activate 定义在其之前)

void on_summon_activate(App& app) {
  if (app.summon_active) {
    on_summon_cancel(app);
    return;
  }
  try {
    std::vector<std::string> args = build_summon_args(app);
    if (!app.hidden) append_summon_log(app, "  [参数] " + format_summon_args(args));
    std::string text = run_summon_tool(app, args);
    append_summon_log(app, text);
    app.summon_active = true;
    app.set_status("召唤修改已激活：力士使用\"烽火狼烟\"即可召唤。");
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("激活失败", msg, MB_ICONERROR);
    append_summon_log(app, "激活失败：" + msg);
    app.set_status("激活失败：" + msg);
  }
}

void on_summon_cancel(App& app) {
  try {
    unsigned int pid = 0;
    ProcessMemory tmp_mem;
    ProcessMemory* mem = nullptr;
    if (app.modder().mem().is_open()) {
      pid = app.modder().pid();
      mem = &app.modder().mem();
    } else {
      auto sel = try_get_selected_pid(app);
      if (!sel) throw std::runtime_error("未选择进程");
      pid = *sel;
      if (!tmp_mem.open(pid)) throw std::runtime_error("OpenProcess 失败");
      mem = &tmp_mem;
    }
    auto base = ProcessMemory::find_module_base(pid, app.cfg().cfg.current().hook.module);
    if (!base) throw std::runtime_error("未找到 EasyRPG116.dll");
    std::string text = summon_130::cmd_restore_vanilla(*mem, *base);
    append_summon_log(app, text);
    app.summon_active = false;
    app.set_status("已取消：召唤补丁已还原，恢复游戏默认烽火狼烟。");
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("取消失败", msg, MB_ICONERROR);
    append_summon_log(app, "取消失败：" + msg);
    app.set_status("取消失败：" + msg);
  }
}

void refresh_misc_summon_list(App& app) {
  app.sm_misc_items.clear();
  app.sm_misc_recs.clear();
  const bool is_steam = (app.cfg().cfg.current_mode == "mode1");
  try {
    unsigned int pid = 0;
    ProcessMemory tmp_mem;
    ProcessMemory* mem = nullptr;
    if (app.modder().mem().is_open()) {
      pid = app.modder().pid();
      mem = &app.modder().mem();
    } else {
      auto sel = try_get_selected_pid(app);
      if (!sel) return;
      pid = *sel;
      if (!tmp_mem.open(pid)) return;
      mem = &tmp_mem;
    }
    auto base_opt = ProcessMemory::find_module_base(pid, app.cfg().cfg.current().hook.module);
    if (!base_opt) return;
    // Steam 版与 v1.30 的类型表结构相同但偏移不同, 各用各的扫描函数。
    auto rows = is_steam ? summon_steam::list_misc_type_table(*mem, *base_opt)
                         : summon_130::list_misc_type_table(*mem, *base_opt);
    for (const auto& r : rows) {
      app.sm_misc_recs.push_back(r.rec);
      std::string line = r.field0;
      if (!r.field1.empty()) line += " / " + r.field1;
      line += " [rec" + std::to_string(r.rec) + "]";
      app.sm_misc_items.push_back(std::move(line));
    }
    if (app.sm_misc >= static_cast<int>(app.sm_misc_items.size())) app.sm_misc = 0;
  } catch (...) {
    app.sm_misc_items.clear();
    app.sm_misc_recs.clear();
  }
}

void refresh_beast_wild_list(App& app) {
  app.sm_beast_wild_items.clear();
  if (app.cfg().cfg.current_mode == "mode1") return;
  if (app.sm_mon != 8 && app.sm_mon != 9) return;
  try {
    unsigned int pid = 0;
    ProcessMemory tmp_mem;
    ProcessMemory* mem = nullptr;
    if (app.modder().mem().is_open()) {
      pid = app.modder().pid();
      mem = &app.modder().mem();
    } else {
      auto sel = try_get_selected_pid(app);
      if (!sel) return;
      pid = *sel;
      if (!tmp_mem.open(pid)) return;
      mem = &tmp_mem;
    }
    auto base_opt = ProcessMemory::find_module_base(pid, app.cfg().cfg.current().hook.module);
    if (!base_opt) return;
    auto rows = summon_130::list_beast_type_table(*mem, *base_opt, app.sm_mon);
    for (const auto& r : rows) {
      std::string line = "rec" + std::to_string(r.rec) + " " + r.field0;
      if (!r.field1.empty()) line += " / " + r.field1;
      app.sm_beast_wild_items.push_back(std::move(line));
    }
    if (app.sm_beast_sub >= static_cast<int>(app.sm_beast_wild_items.size())) app.sm_beast_sub = 0;
  } catch (...) {
    app.sm_beast_wild_items.clear();
  }
}

void on_summon_refresh(App& app) {
  if (!app.summon_active) return;
  try {
    std::vector<std::string> args = build_summon_args(app);
    if (!app.hidden) append_summon_log(app, "  [参数] " + format_summon_args(args));
    std::string text = run_summon_tool(app, args);
    append_summon_log(app, text);
    app.set_status("已重新设置当前界面选择的怪物类型。");
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("刷新失败", msg, MB_ICONERROR);
    append_summon_log(app, "刷新失败：" + msg);
    app.set_status("刷新失败：" + msg);
  }
}

void tick_summon(App& app, float dt) {
  (void)app;
  (void)dt;
  // 周期扫堆写 HP 曾误伤内存导致「召不出来」; 待安全方案再开.
}
}  // namespace poq
