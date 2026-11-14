#pragma once
// 秦殇物品修改器 —— 人物 / 道具 tab 的跨文件声明(阶段二 §4 从 App.h 拆出)。
// 本头只前置声明 App, 不含任何 Core 依赖: 调用方需要 App 定义时再 include "UI/App.h"。

#include <cstdint>
#include <optional>
#include <string>

namespace poq {

struct App;
struct CharFieldCfg;

// ---- TabChar.cpp: 进程/初始化/道具逻辑/道具 UI ----
std::optional<unsigned int> try_get_selected_pid(const App& app);
std::optional<unsigned int> find_poq_pid(const App& app);
void on_init(App& app);
void on_raw_read(App& app);
void on_raw_write(App& app);
void on_save_cfg(App& app);
std::string field_value_display(const App& app, const char* label, int v);

// ---- ItemTabLogic.cpp: 道具 tab 读写/模板/复制 ----
void on_read(App& app);
void tick_item_read(App& app, float dt);
void on_save(App& app);
void on_copy_prev_item(App& app);
void on_load_tpl(App& app);
void on_save_as(App& app);

// ---- CharacterTabLogic.cpp: 人物 tab 读写/锁定 ----
bool is_char_field_editable(const App& app, const CharFieldCfg& field);
int find_char_field(const std::string& label);
void rebuild_char_widgets(App& app);
void on_char_read(App& app);
void on_char_save(App& app);
void on_money_apply(App& app);
void on_invincible_toggled(App& app, bool checked);
void on_kill_toggled(App& app, bool checked);
void tick_lock(App& app, float dt);
void render_basic_fields(App& app);
void render_attr_table(App& app, float height);
void render_char_tab(App& app);

}  // namespace poq
