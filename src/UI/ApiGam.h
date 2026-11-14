#pragma once
// 秦殇物品修改器 —— GAM 存档修改 tab 的跨文件声明(阶段二 §4 从 App.h 拆出)。
// 本头只前置声明 App, 不含任何 Core 依赖: 调用方需要 App 定义时再 include "UI/App.h"。

#include <cstdint>
#include <optional>
#include <string>

namespace poq {

struct App;

// ---- TabGam.cpp: GAM 存档修改 ----
std::wstring timestamped_bak_path(const std::wstring& path);
std::string gam_load_prefs();
void render_gam_tab(App& app);

}  // namespace poq
