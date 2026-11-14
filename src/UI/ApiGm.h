#pragma once
// 秦殇物品修改器 —— GM 命令 tab 的跨文件声明(阶段二 §4 从 App.h 拆出)。
// 本头只前置声明 App, 不含任何 Core 依赖: 调用方需要 App 定义时再 include "UI/App.h"。

#include <cstdint>
#include <optional>
#include <string>

namespace poq {

struct App;

// ---- GmTabLogic.cpp / GmTabPanel.cpp: GM 命令 ----
int load_gm_cmd_list(App& app, bool keep_previous_on_error);
void tick_gm(App& app, float dt);
void render_gm_tab(App& app);

}  // namespace poq
