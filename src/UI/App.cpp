// 秦殇物品修改器 —— App 的方法实现(从 App.h 拆出, 阶段二 §4)。
// 目的: App.h 只留"数据成员 + 声明", 头文件更轻、依赖更少。

#include "UI/App.h"
#include <cstdio>
#include <cstring>
#include "Core/Characters.h"
#include "Core/GameModder.h"
#include "Core/ItemData.h"
#include "Core/OffsetsConfig.h"
#include "Native/ProcessMemory.h"
#include "UI/ApiGam.h"
#include "UI/AppCfgState.h"
#include "UI/AppCharState.h"
#include "UI/AppGamState.h"
#include "UI/AppItemState.h"
#include "UI/AppProcessState.h"

namespace poq {

App::~App() = default;

AppGamState& App::gam() noexcept {
  return *gam_;
}
const AppGamState& App::gam() const noexcept {
  return *gam_;
}

AppCharState& App::char_state() noexcept {
  return *char_;
}
const AppCharState& App::char_state() const noexcept {
  return *char_;
}

AppCfgState& App::cfg() noexcept {
  return *cfg_;
}
const AppCfgState& App::cfg() const noexcept {
  return *cfg_;
}

AppItemState& App::item() noexcept {
  return *item_;
}
const AppItemState& App::item() const noexcept {
  return *item_;
}

AppProcessState& App::proc() noexcept {
  return *proc_;
}
const AppProcessState& App::proc() const noexcept {
  return *proc_;
}

GameModder& App::modder() noexcept {
  return *modder_;
}
const GameModder& App::modder() const noexcept {
  return *modder_;
}

App::App()
    : cfg_(std::make_unique<AppCfgState>()),
      modder_(std::make_unique<GameModder>(cfg().cfg)),
      gam_(std::make_unique<AppGamState>()),
      char_(std::make_unique<AppCharState>()),
      item_(std::make_unique<AppItemState>()),
      proc_(std::make_unique<AppProcessState>()) {
  cfg().basic_fields = cfg().cfg.current().basic_fields;
  rebuild_basic();
  char_state().char_widgets.assign(char_fields().size(), CharWidget());
  for (const auto& kv : ItemData::attr_names()) item().attr_ids.push_back(kv.first);
  for (const auto& kv : ItemData::color_names()) item().color_ids.push_back(kv.first);
  for (const auto& kv : ItemData::percent_names()) item().percent_ids.push_back(kv.first);
  reload_cfg_buffers();
  std::snprintf(gm_count_buf, sizeof(gm_count_buf), "1");  // 执行次数默认 1
  gam_->gam_last_dir = gam_load_prefs();
  status = "就绪。请以管理员身份运行,初始化后在游戏中拾起物品。";
}

void App::ensure_gam_data_loaded() {
  AppGamState& g = gam();
  if (g.gam_data_load_tried) return;
  g.gam_data_load_tried = true;
  if (g.gam_catalog.load({wide_to_utf8(app_dir_w() + L"\\gam_catalog.bin"),
                          wide_to_utf8(app_dir_w() + L"\\data\\gam_catalog.bin"),
                          wide_to_utf8(app_dir_w() + L"\\..\\..\\data\\gam_catalog.bin")},
                         g.gam_catalog)) {
    g.gam_cat_loaded = true;
  }
  if (g.gam_scene_cat.load({wide_to_utf8(app_dir_w() + L"\\scene_catalog.bin"),
                            wide_to_utf8(app_dir_w() + L"\\data\\scene_catalog.bin"),
                            wide_to_utf8(app_dir_w() + L"\\..\\..\\data\\scene_catalog.bin")},
                           g.gam_scene_cat)) {
    g.gam_scene_loaded = true;
  }
  if (g.gam_item_names.load({wide_to_utf8(app_dir_w() + L"\\item_names.bin"),
                             wide_to_utf8(app_dir_w() + L"\\data\\item_names.bin"),
                             wide_to_utf8(app_dir_w() + L"\\..\\..\\data\\item_names.bin")},
                            g.gam_item_names)) {
    g.gam_item_names_loaded = true;
  }
}

void App::ensure_process_list() {
  if (proc().list_ready) return;
  proc().list_ready = true;
  refresh_processes();
}

void App::rebuild_basic() {
  AppItemState& it = item();
  it.basic.clear();
  it.basic_enum_options.resize(cfg().basic_fields.size());
  for (size_t i = 0; i < cfg().basic_fields.size(); ++i)
    it.basic_enum_options[i].build(cfg().basic_fields[i].enum_map,
                                   cfg().basic_fields[i].enum_display_value);
  for (const auto& f : cfg().basic_fields) {
    AppItemState::BasicWidget w;
    w.is_enum = !f.enum_map.empty();
    it.basic.push_back(w);
  }
}

void App::refresh_processes() {
  AppProcessState& pr = proc();
  pr.processes.clear();
  pr.processes = ProcessMemory::enumerate_poq_targets();
  pr.proc_selected = pr.processes.empty() ? -1 : 0;
  std::string msg = "已列出 " + std::to_string(pr.processes.size()) + " 个游戏进程。";
  if (pr.processes.empty())
    msg += "未找到 POQ/DxWnd 等含 POQ 模块的进程,请先启动游戏后点\"刷新进程\"。";
  status = msg;
}

void App::reload_cfg_buffers() {
  AppCfgState& c = cfg();
  c.mode_ids.clear();
  for (const auto& kv : c.cfg.modes) c.mode_ids.push_back(kv.first);
  // 定位当前模式
  c.mode_selected = 0;
  for (size_t i = 0; i < c.mode_ids.size(); ++i)
    if (c.mode_ids[i] == c.cfg.current_mode) {
      c.mode_selected = static_cast<int>(i);
      break;
    }
  const ModeCfg& m = c.cfg.current();
  c.gam_enum_options.resize(m.basic_fields.size());
  for (size_t i = 0; i < m.basic_fields.size(); ++i)
    c.gam_enum_options[i].build(m.basic_fields[i].enum_map, m.basic_fields[i].enum_display_value);
  const HookCfg& hk = m.hook;
  const AttrListCfg& al = m.attr_list;
  std::snprintf(c.cfg_module, sizeof(c.cfg_module), "%s", hk.module.c_str());
  std::snprintf(c.cfg_offset, sizeof(c.cfg_offset), "%s", hex_str(hk.offset).c_str());
  std::snprintf(c.cfg_aob, sizeof(c.cfg_aob), "%s", hk.aob.c_str());
  std::snprintf(c.cfg_weight_offset, sizeof(c.cfg_weight_offset), "%s",
                hex_str(hk.weight_offset).c_str());
  std::snprintf(c.cfg_weight_max, sizeof(c.cfg_weight_max), "%u", hk.weight_max);
  std::snprintf(c.cfg_item_adjust, sizeof(c.cfg_item_adjust), "%d", m.item_base_adjust);
  std::snprintf(c.cfg_head, sizeof(c.cfg_head), "%s", hex_str(al.head_offset).c_str());
  std::snprintf(c.cfg_next, sizeof(c.cfg_next), "%s", hex_str(al.next_offset).c_str());
  std::snprintf(c.cfg_id, sizeof(c.cfg_id), "%s", hex_str(al.id_offset).c_str());
  std::snprintf(c.cfg_color, sizeof(c.cfg_color), "%s", hex_str(al.color_offset).c_str());
  std::snprintf(c.cfg_precent, sizeof(c.cfg_precent), "%s", hex_str(al.precent_offset).c_str());
  std::snprintf(c.cfg_value, sizeof(c.cfg_value), "%s", hex_str(al.value_offset).c_str());
}

std::optional<int> App::current_type_value() const {
  for (size_t i = 0; i < cfg().basic_fields.size(); ++i) {
    if (cfg().basic_fields[i].label == "类型") {
      const AppItemState& it = item();
      return it.basic[i].is_enum ? it.basic[i].enum_val : it.basic[i].intval;
    }
  }
  return std::nullopt;
}

void App::set_status(const std::string& msg) {
  status = sanitize(msg, !hidden);
}

}  // namespace poq
