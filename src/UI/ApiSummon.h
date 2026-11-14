#pragma once
// 秦殇物品修改器 —— 召唤修改 tab 的跨文件声明(阶段二 §4 从 App.h 拆出)。
// 本头只前置声明 App, 不含任何 Core 依赖: 调用方需要 App 定义时再 include "UI/App.h"。

#include <cstdint>
#include <optional>
#include <string>

namespace poq {

struct App;

// ---- SummonTab*.cpp: 召唤修改 ----
void render_summon_tab(App& app);
void tick_summon(App& app, float dt);

}  // namespace poq
