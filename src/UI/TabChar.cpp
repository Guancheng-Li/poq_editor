// 秦殇物品修改器 (PoqEditor) —— 进程初始化 / 原始读写 / 配置保存(逻辑层)。
// 道具 UI → ItemTabPanels.cpp；角色 UI → CharTabPanel.cpp；读写 → *TabLogic.cpp。

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include "Core/GameModder.h"
#include "Core/Json.h"
#include "Core/JsonTemplate.h"
#include "Native/ProcessMemory.h"
#include "UI/App.h"
#include "UI/AppCfgState.h"
#include "UI/AppItemState.h"
#include "UI/AppProcessState.h"

namespace poq {

// ============================================================================
// 进程 / 业务逻辑(对应 cpp MainWindow 的 slot)
// ============================================================================

std::optional<unsigned int> try_get_selected_pid(const App& app) {
  if (app.proc().proc_selected >= 0 &&
      static_cast<size_t>(app.proc().proc_selected) < app.proc().processes.size())
    return app.proc().processes[static_cast<size_t>(app.proc().proc_selected)].pid;
  return std::nullopt;
}

std::optional<unsigned int> find_poq_pid(const App& app) {
  if (!app.proc().processes.empty()) return app.proc().processes.front().pid;
  return std::nullopt;
}

// 按游戏实际加载的引擎模块自动选版本模式:EasyRPG.dll=Steam(mode1),EasyRPG116.dll=v1.30(mode0)。
// 两版本 hook/flag 偏移不同,自动选避免用户手选错。
static void auto_select_mode(App& app, unsigned int pid) {
  std::string want;
  if (ProcessMemory::find_module_base(pid, "EasyRPG.dll"))
    want = "mode1";
  else if (ProcessMemory::find_module_base(pid, "EasyRPG116.dll"))
    want = "mode0";
  if (want.empty() || want == app.cfg().cfg.current_mode) return;
  app.cfg().cfg.set_current(want);
  app.modder().config() = app.cfg().cfg;
  app.cfg().basic_fields = app.cfg().cfg.current().basic_fields;
  app.rebuild_basic();
  app.reload_cfg_buffers();
  for (size_t i = 0; i < app.cfg().mode_ids.size(); ++i)
    if (app.cfg().mode_ids[i] == want) {
      app.cfg().mode_selected = static_cast<int>(i);
      break;
    }
}

void on_init(App& app) {
  auto pid = try_get_selected_pid(app);
  if (!pid) {
    app.set_status("请先选择一个进程。");
    return;
  }
  auto_select_mode(app, *pid);  // 按引擎模块自动选 steam / v1.30
  bool restarted = false;
  uintptr_t ptr = 0;
  try {
    try {
      ptr = app.modder().initialize(*pid);
    } catch (const std::exception& ex) {
      // 游戏可能已重启,选中 PID 失效:刷新进程列表后按名重试一次
      if (std::string(ex.what()).find("OpenProcess") == std::string::npos) throw;
      app.refresh_processes();
      auto new_pid = find_poq_pid(app);
      if (!new_pid || *new_pid == *pid) throw;
      pid = new_pid;
      for (size_t i = 0; i < app.proc().processes.size(); ++i)
        if (app.proc().processes[i].pid == *new_pid) {
          app.proc().proc_selected = static_cast<int>(i);
          break;
        }
      auto_select_mode(app, *new_pid);  // 重启后按新进程引擎模块重选版本
      ptr = app.modder().initialize(*pid);
      restarted = true;
    }
    uintptr_t slot = app.modder().cur_item_slot_addr();
    std::snprintf(app.item().raw_addr, sizeof(app.item().raw_addr), "0x%s", hex8(slot).c_str());
    const bool dbg = !app.hidden;
    std::string prefix = restarted ? "检测到游戏重启,已自动重新选择进程。" : "";
    const bool injected = app.modder().is_injected();
    if (dbg) {
      std::string base_info = "hook模块=" + app.cfg().cfg.current().hook.module + " hook@0x" +
                              hex8(app.modder().hook_addr()) + " cur_item槽=0x" + hex8(slot);
      if (!injected) {
        app.set_status(prefix + "初始化成功(注入失败,道具修改不可用)。请重新初始化。" + base_info);
      } else if (ptr == 0 || ptr == 0xFFFFFFFF) {
        app.set_status(prefix +
                       "初始化成功(已注入)。当前无手持物品,请在游戏中拾起物品后点\"读取属性\"。" +
                       base_info);
      } else {
        app.set_status(prefix + "初始化成功(已注入)。手持物品指针 = 0x" + hex8(ptr) + base_info);
      }
    } else {
      if (!injected) {
        app.set_status(prefix + "初始化成功(注入失败,道具修改不可用)。请重新初始化。");
      } else if (ptr == 0 || ptr == 0xFFFFFFFF) {
        app.set_status(prefix +
                       "初始化成功(已注入)。当前无手持物品,请在游戏中拾起物品后点\"读取属性\"。");
      } else {
        app.set_status(prefix + "初始化成功(已注入)。");
      }
    }
    // 初始化后自动加载角色属性(对齐 Python _on_init)
    try {
      on_char_read(app);
    } catch (...) {
    }
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("初始化失败", msg, MB_ICONERROR);
    app.set_status("初始化失败:" + msg);
  }
}

// 道具读/写/模板/复制见 ItemTabLogic.cpp
void on_raw_read(App& app) {
  if (!app.modder().mem().is_open()) {
    app.set_status("请先\"初始化\"。");
    return;
  }
  try {
    uintptr_t addr = ProcessMemory::parse_address(trim(app.item().raw_addr));
    int size = 64;
    char* end = nullptr;
    long parsed = std::strtol(app.item().raw_size, &end, 10);
    if (end != app.item().raw_size && *end == '\0' && parsed > 0) size = static_cast<int>(parsed);
    if (size > 4000) size = 4000;  // 防 buffer 溢出
    auto data = app.modder().read_memory_dump(addr, size);
    std::string dump = format_hex_dump(addr, data);
    std::snprintf(app.item().raw_hex, sizeof(app.item().raw_hex), "%s", dump.c_str());
    app.set_status("已读取 0x" + hex8(addr) + " 共 " + std::to_string(data.size()) + " 字节。");
  } catch (const std::exception& ex) {
    app.set_status("读取失败:" + sanitize(ex.what(), !app.hidden));
  }
}

void on_raw_write(App& app) {
  if (!app.modder().mem().is_open()) {
    app.set_status("请先\"初始化\"。");
    return;
  }
  try {
    uintptr_t addr = ProcessMemory::parse_address(trim(app.item().raw_addr));
    auto data = parse_hex_dump(app.item().raw_hex);
    if (data.empty()) {
      app.set_status("没有可写入的十六进制字节。");
      return;
    }
    app.modder().mem().write(addr, data);
    app.set_status("已写入 0x" + hex8(addr) + " 共 " + std::to_string(data.size()) + " 字节。");
  } catch (const std::exception& ex) {
    app.set_status("写入失败:" + sanitize(ex.what(), !app.hidden));
  }
}

void on_save_cfg(App& app) {
  try {
    if (app.cfg().mode_selected < 0 ||
        static_cast<size_t>(app.cfg().mode_selected) >= app.cfg().mode_ids.size())
      throw std::runtime_error("无效模式");
    const std::string& mid = app.cfg().mode_ids[static_cast<size_t>(app.cfg().mode_selected)];
    auto mit = app.cfg().cfg.modes.find(mid);
    if (mit == app.cfg().cfg.modes.end()) throw std::runtime_error("无效模式");
    ModeCfg& m = mit->second;
    auto parse_hex = [](const char* s) -> uint32_t {
      return static_cast<uint32_t>(OffsetsConfig::parse_long(trim(s)));
    };
    auto parse_dec = [](const char* key, const char* s) -> long long {
      std::string t = trim(s);
      if (t.empty()) throw std::runtime_error(std::string(key) + " 不是整数");
      char* end = nullptr;
      long long v = std::strtoll(t.c_str(), &end, 10);
      if (end == t.c_str() || *end != '\0')
        throw std::runtime_error(std::string(key) + " 不是整数");
      return v;
    };

    m.hook.module = app.cfg().cfg_module;
    m.hook.offset = parse_hex(app.cfg().cfg_offset);
    m.hook.aob = app.cfg().cfg_aob;
    m.hook.weight_offset = parse_hex(app.cfg().cfg_weight_offset);
    m.hook.weight_max =
        static_cast<uint32_t>(parse_dec("hook.weight_max", app.cfg().cfg_weight_max));
    m.item_base_adjust = static_cast<int>(parse_dec("item_base_adjust", app.cfg().cfg_item_adjust));
    AttrListCfg& al = m.attr_list;
    al.head_offset = parse_hex(app.cfg().cfg_head);
    al.next_offset = parse_hex(app.cfg().cfg_next);
    al.id_offset = parse_hex(app.cfg().cfg_id);
    al.color_offset = parse_hex(app.cfg().cfg_color);
    al.precent_offset = parse_hex(app.cfg().cfg_precent);
    al.value_offset = parse_hex(app.cfg().cfg_value);
    app.cfg().cfg.set_current(mid);
    // 单文件版:偏移只保存在内存中,不写配置文件(重启回到内置默认)。
    app.modder().config() = app.cfg().cfg;
    app.cfg().basic_fields = app.cfg().cfg.current().basic_fields;
    app.rebuild_basic();
    app.reload_cfg_buffers();
    app.set_status("配置已应用(仅本次会话,重启后回到内置默认)。");
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("保存配置失败", msg, MB_ICONERROR);
    app.set_status("保存配置失败:" + msg);
  }
}
}  // namespace poq
