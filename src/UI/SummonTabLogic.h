#pragma once

#include <string>
#include <vector>

namespace poq {

struct App;

std::vector<std::string> build_summon_args(const App& app);
void append_summon_log(App& app, const std::string& text);
void on_summon_activate(App& app);
void on_summon_refresh(App& app);
/// v1.30: 刷新朱厌/善翼类型表候选(需 POQ 进程); 失败则清空下拉。
void refresh_beast_wild_list(App& app);
void refresh_misc_summon_list(App& app);
/// 激活态: 队伍里召唤单位当前生命为 0 时同步为上限(修复空血槽 UI)。
void tick_summon(App& app, float dt);

}  // namespace poq
