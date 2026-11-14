#pragma once
// 秦殇物品修改器 —— 主窗口 / 帮助弹窗的跨文件声明(阶段二 §4 从 App.h 拆出)。
// 本头只前置声明 App, 不含任何 Core 依赖: 调用方需要 App 定义时再 include "UI/App.h"。

#include <cstdint>
#include <optional>
#include <string>

namespace poq {

struct App;

// ---- MainWindow.cpp: 主窗口 ----
void apply_blue_theme();
void render_main_window(App& app);

// ---- Help.cpp: 版本/欢迎弹窗/消息队列/使用说明 ----
void render_welcome_dialog(App& app);
void render_msg_queue();
void render_help_popup(App& app);

}  // namespace poq
