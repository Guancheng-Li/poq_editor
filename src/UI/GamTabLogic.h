#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "Core/GamFile.h"

namespace poq {

struct App;

// GAM tab 命令逻辑(打开/保存/剪贴板/创建), 与 TabGam.cpp 的 ImGui 渲染分离。
void gam_on_open(App& app);
void gam_on_save(App& app);
void gam_on_copy_item(App& app);
void gam_on_paste_item(App& app);
void gam_on_copy_attr(App& app);
void gam_on_paste_attr(App& app);
void gam_on_create(App& app);

void gam_copy_item_at(App& app, int idx);
void gam_paste_into(App& app, int ci);

int gam_sel_mode(const App& app);
bool gam_is_non_equipment(const GamItem& it);
std::string gam_item_disp_name(const App& app, const std::string& alias);
std::string gam_format_item_tip(const App& app, const std::vector<uint8_t>& rec);
void gam_commit_g_info(App& app, GamItem& it);

const char* gam_field_to_item_label(const std::string& name);
std::string attr_g_info_alias_u8(const std::string& n);
std::string attr_bare_name_u8(const std::string& disp);

}  // namespace poq
