#pragma once

#include <string>

namespace poq {

struct App;

int load_gm_cmd_list(App& app, bool keep_previous_on_error);
void tick_gm(App& app, float dt);
void gm_refresh_ready(App& app);
void append_gm_log(App& app, const std::string& text);
void on_gm_toggle(App& app);
void on_gm_exec(App& app);

}  // namespace poq
