// GM Tab —— gm_cmd.txt 解析、激活/执行/周期刷新逻辑。

#include "UI/GmTabLogic.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "Core/GameModder.h"
#include "Core/GmCmd.h"
#include "Native/ProcessMemory.h"
#include "UI/App.h"
#include "UI/AppCfgState.h"

namespace poq {

// 解析 gm_cmd.txt 文本 -> (选项列表, 格式错误行数),对齐 python
// gm_tab._parse_cmd_list:每行 "{命令}#{显示名}";空行忽略;行首 # 注释;
// 无 # 则命令=显示名;多个 # 取第一个分割;命令为空(格式不符) -> 计为错误行。
std::pair<std::vector<std::pair<std::string, std::string>>, int> parse_gm_cmd_text(
    const std::string& raw) {
  std::vector<std::pair<std::string, std::string>> items;
  int errors = 0;
  std::string cur;
  auto add_line = [&]() {
    std::string s = trim(cur);
    cur.clear();
    if (s.empty() || s[0] == '#') return;
    std::string cmd = s, disp = s;
    size_t pos = s.find('#');
    if (pos != std::string::npos) {
      cmd = trim(s.substr(0, pos));
      disp = trim(s.substr(pos + 1));
      if (disp.empty()) disp = cmd;
    }
    if (!cmd.empty())
      items.emplace_back(cmd, disp);
    else
      ++errors;  // 命令为空(格式不符) -> 错误行
  };
  for (size_t i = 0; i <= raw.size(); ++i) {
    char c = (i < raw.size()) ? raw[i] : '\n';
    if (c == '\r') continue;
    if (c != '\n') {
      cur.push_back(c);
      continue;
    }
    add_line();
  }
  if (!cur.empty()) add_line();
  return {std::move(items), errors};
}

// 读取 gm_cmd.txt(工具目录,UTF-8)填充"常用命令"下拉,返回格式错误行数。
// keep_previous_on_error=true 且存在错误行时保留原列表不变(调用方弹窗提示);
// 否则用解析结果替换。文件不存在/不可读 -> 空表、0 错误(不报错)。
int load_gm_cmd_list(App& app, bool keep_previous_on_error) {
  FILE* f = _wfopen((app_dir_w() + L"\\gm_cmd.txt").c_str(), L"rb");
  if (!f) return 0;
  std::string raw;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
  fclose(f);
  auto parsed = parse_gm_cmd_text(raw);
  if (parsed.second > 0 && keep_previous_on_error) return parsed.second;  // 保留旧列表
  app.gm_cmd_list = std::move(parsed.first);
  if (app.gm_cmdlist_sel >= static_cast<int>(app.gm_cmd_list.size())) app.gm_cmdlist_sel = 0;
  return parsed.second;
}

void append_gm_log(App& app, const std::string& text) {
  if (text.empty()) return;
  if (!app.gm_log.empty()) app.gm_log += "\n";
  app.gm_log += text;
}

// 解析 GM 调用上下文(对齐 python gm_tab._call 的 pid/版本解析 + run_summon_tool 模式):
// pid 优先取已初始化(modder 打开)的,否则取顶部下拉选中并临时 OpenProcess;
// 版本由当前模式决定(mode0=1.30 / mode1=steam),按该版本模块名找 rpg 基址,
// 找不到 -> 抛"版本不符"错误;exe = POQ.exe 基址。
void resolve_gm_ctx(App& app, ProcessMemory& tmp_mem, ProcessMemory*& mem, unsigned int& pid,
                    uintptr_t& rpg, uintptr_t& exe, const gm_cmd::GmVersion*& ver) {
  mem = nullptr;
  if (app.modder().mem().is_open()) {
    pid = app.modder().pid();
    mem = &app.modder().mem();
  } else {
    auto sel = try_get_selected_pid(app);
    if (!sel) throw std::runtime_error("未选择进程：请先在顶部选择 POQ.EXE 并初始化。");
    pid = *sel;
    if (!tmp_mem.open(pid)) throw std::runtime_error("OpenProcess 失败（请以管理员身份运行）。");
    mem = &tmp_mem;
  }
  ver =
      (app.cfg().cfg.current_mode == "mode1") ? &gm_cmd::steam_version() : &gm_cmd::v130_version();
  auto base_opt = ProcessMemory::find_module_base(pid, ver->mod);
  if (!base_opt) {
    throw std::runtime_error(
        "未找到模块 " + ver->mod + "：当前运行的游戏版本与所选版本（" +
        app.cfg().cfg.current().name +
        "）不符，或游戏未运行。请确认游戏正在运行，并在顶部选择正确的游戏版本后重新初始化。");
  }
  rpg = *base_opt;
  auto exe_opt = ProcessMemory::find_module_base(pid, "POQ.exe");
  exe = exe_opt.value_or(0);
}

// 静默刷新 GM 激活状态(对齐 python _refresh_status:读内存判断,不弹窗,失败=false)。
void gm_refresh_ready(App& app) {
  bool ready = false;
  try {
    ProcessMemory tmp_mem;
    ProcessMemory* mem = nullptr;
    unsigned int pid = 0;
    uintptr_t rpg = 0, exe = 0;
    const gm_cmd::GmVersion* ver = nullptr;
    resolve_gm_ctx(app, tmp_mem, mem, pid, rpg, exe, ver);
    gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
    ready = gm_cmd::is_gm_ready(ctx);
  } catch (...) {
    ready = false;
  }
  app.gm_active = ready;
}

static void gm_exec_finish(App& app, const std::string& cmd, int count) {
  if (app.gm_history.empty() || app.gm_history.back() != cmd) {
    app.gm_history.push_back(cmd);
    if (app.gm_history.size() > 30) app.gm_history.erase(app.gm_history.begin());
  }
  app.gm_hist_idx = -1;
  gm_refresh_ready(app);
  app.set_status("已执行 " + std::to_string(count) + " 次命令：" + cmd);
  app.gm_exec_pending = false;
  app.gm_exec_cmd.clear();
  app.gm_exec_remaining = 0;
  app.gm_exec_total = 0;
  app.gm_exec_gap_accum = 0.0f;
}

static void gm_exec_abort(App& app, const std::string& msg) {
  app.gm_exec_pending = false;
  app.gm_exec_cmd.clear();
  app.gm_exec_remaining = 0;
  app.gm_exec_total = 0;
  app.gm_exec_gap_accum = 0.0f;
  msg_box("执行失败", msg, MB_ICONERROR);
  append_gm_log(app, "执行失败：" + msg);
  app.set_status("执行失败：" + msg);
}

// 连发剩余次数; 与 python 一致每次间隔 100ms, 但不阻塞 UI 线程。
static void tick_gm_exec_burst(App& app, float dt) {
  if (!app.gm_exec_pending) return;
  app.gm_exec_gap_accum += dt;
  if (app.gm_exec_gap_accum < 0.1f) return;
  app.gm_exec_gap_accum = 0.0f;
  try {
    ProcessMemory tmp_mem;
    ProcessMemory* mem = nullptr;
    unsigned int pid = 0;
    uintptr_t rpg = 0, exe = 0;
    const gm_cmd::GmVersion* ver = nullptr;
    resolve_gm_ctx(app, tmp_mem, mem, pid, rpg, exe, ver);
    gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
    append_gm_log(app, gm_cmd::send(ctx, app.gm_exec_cmd));
    --app.gm_exec_remaining;
    const int done = app.gm_exec_total - app.gm_exec_remaining;
    if (app.gm_exec_remaining <= 0) {
      gm_exec_finish(app, app.gm_exec_cmd, app.gm_exec_total);
      return;
    }
    app.set_status("正在执行命令 (" + std::to_string(done) + "/" +
                   std::to_string(app.gm_exec_total) + ")…");
  } catch (const std::exception& ex) {
    gm_exec_abort(app, sanitize(ex.what(), !app.hidden));
  }
}

// GM 状态周期刷新(对齐 python QTimer 2s):仅 GM tab 可见时跑。
void tick_gm(App& app, float dt) {
  tick_gm_exec_burst(app, dt);
  if (!app.gm_visible) {
    app.gm_timer_accum = 0.0f;
    return;
  }
  app.gm_timer_accum += dt;
  if (app.gm_timer_accum < 2.0f) return;
  app.gm_timer_accum = 0.0f;
  gm_refresh_ready(app);
}

void on_gm_restore(App& app);

void on_gm_toggle(App& app) {
  if (app.gm_active) {
    on_gm_restore(app);
    return;
  }
  try {
    ProcessMemory tmp_mem;
    ProcessMemory* mem = nullptr;
    unsigned int pid = 0;
    uintptr_t rpg = 0, exe = 0;
    const gm_cmd::GmVersion* ver = nullptr;
    resolve_gm_ctx(app, tmp_mem, mem, pid, rpg, exe, ver);
    gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
    gm_cmd::Method method = (app.gm_method == 1) ? gm_cmd::Method::R9 : gm_cmd::Method::Memory;
    // 激活/写 wizard.lst 的日志不进界面,进 F12 调试日志
    append_gm_log(app, gm_cmd::activate(ctx, method));
    gm_refresh_ready(app);
    if (app.gm_active)
      app.set_status("GM 权限已激活。");
    else
      app.set_status("激活已执行，但未检测到 GM 权限（游戏内需读档进入后生效）。");
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("激活失败", msg, MB_ICONERROR);
    append_gm_log(app, "激活失败：" + msg);
    app.set_status("激活失败：" + msg);
  }
}

void on_gm_restore(App& app) {
  try {
    ProcessMemory tmp_mem;
    ProcessMemory* mem = nullptr;
    unsigned int pid = 0;
    uintptr_t rpg = 0, exe = 0;
    const gm_cmd::GmVersion* ver = nullptr;
    resolve_gm_ctx(app, tmp_mem, mem, pid, rpg, exe, ver);
    gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
    append_gm_log(app, gm_cmd::disable(ctx));
    gm_refresh_ready(app);
    app.set_status("GM 权限已还原。");
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("还原失败", msg, MB_ICONERROR);
    append_gm_log(app, "还原失败：" + msg);
    app.set_status("还原失败：" + msg);
  }
}

// 执行次数:未勾选=1;勾选则校验 1~20(对齐 python _parse_count)。非法返回 -1(已弹窗)。
int gm_parse_count(App& app) {
  if (!app.ck_count) return 1;
  std::string txt = trim(app.gm_count_buf);
  char* end = nullptr;
  long n = std::strtol(txt.c_str(), &end, 10);
  if (end == txt.c_str() || *end != '\0') {
    msg_box("输入错误", "执行次数必须是整数。", MB_ICONWARNING);
    app.set_status("输入错误：执行次数必须是整数。");
    return -1;
  }
  if (n < 1 || n > 20) {
    msg_box("输入错误", "执行次数必须在 1 ~ 20 之间。", MB_ICONWARNING);
    app.set_status("输入错误：执行次数必须在 1 ~ 20 之间。");
    return -1;
  }
  return static_cast<int>(n);
}

void on_gm_exec(App& app) {
  if (app.gm_exec_pending) return;
  std::string cmd = trim(app.gm_cmd_buf);
  if (cmd.empty()) {
    msg_box("命令为空", "请输入要执行的 GM 命令。", MB_ICONWARNING);
    app.set_status("命令为空");
    return;
  }
  int count = gm_parse_count(app);
  if (count < 0) return;  // 已弹窗
  try {
    ProcessMemory tmp_mem;
    ProcessMemory* mem = nullptr;
    unsigned int pid = 0;
    uintptr_t rpg = 0, exe = 0;
    const gm_cmd::GmVersion* ver = nullptr;
    resolve_gm_ctx(app, tmp_mem, mem, pid, rpg, exe, ver);
    gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
    append_gm_log(app, gm_cmd::send(ctx, cmd));
    if (count <= 1) {
      gm_exec_finish(app, cmd, 1);
      return;
    }
    app.gm_exec_pending = true;
    app.gm_exec_cmd = cmd;
    app.gm_exec_remaining = count - 1;
    app.gm_exec_total = count;
    app.gm_exec_gap_accum = 0.0f;
    app.set_status("正在执行命令 (1/" + std::to_string(count) + ")…");
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("执行失败", msg, MB_ICONERROR);
    append_gm_log(app, "执行失败：" + msg);
    app.set_status("执行失败：" + msg);
  }
}
}  // namespace poq
