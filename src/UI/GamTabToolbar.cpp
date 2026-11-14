// GAM tab —— 顶栏命令按钮与「创建物品」行。

#include "Core/GamCatalog.h"
#include "UI/UiTabCommon.h"

namespace poq {

void render_gam_command_bar(App& app) {
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
  const float line_max_x = ImGui::GetContentRegionAvail().x;
  auto put_btn = [&](const char* text, auto fn) {
    float bw = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    if (ImGui::GetCursorPosX() + bw + 12.0f > line_max_x) ImGui::NewLine();
    if (ImGui::Button(text)) fn();
    ImGui::SameLine(0, 8.0f);
  };
  put_btn("打开gam", [&] { gam_on_open(app); });
  put_btn("保存文件", [&] { gam_on_save(app); });
  put_btn("复制物品", [&] { gam_on_copy_item(app); });
  put_btn("粘贴物品", [&] { gam_on_paste_item(app); });
  put_btn("复制属性", [&] { gam_on_copy_attr(app); });
  put_btn("粘贴属性", [&] { gam_on_paste_attr(app); });
  ImGui::PopStyleVar();
  ImGui::NewLine();
  if (app.gam().gam_loaded)
    ImGui::TextDisabled("%s  |  物品 %d  容器 %d", app.gam().gam_path.c_str(),
                        static_cast<int>(app.gam().gam_file.items.size()),
                        static_cast<int>(app.gam().gam_file.containers.size()));
}

void render_gam_create_row(App& app) {
  float row_x = ImGui::GetContentRegionAvail().x;
  const float lbl_w = ImGui::CalcTextSize("创建新武器：").x;
  const char* cat_preview =
      app.gam().gam_cat_loaded
          ? app.gam().gam_catalog.cats[static_cast<size_t>(app.gam().gam_sel_cat)].c_str()
          : "(目录未加载)";
  const float cat_w = 96.0f;
  const float btn_w =
      ImGui::CalcTextSize("创建物品").x + ImGui::GetStyle().FramePadding.x * 2.0f + 10.0f;
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  float item_w = row_x - lbl_w - cat_w - btn_w - spacing * 4;
  if (item_w < 120.0f) item_w = 120.0f;
  ImGui::TextUnformatted("创建新武器：");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(cat_w);
  if (ImGui::BeginCombo("##cat", cat_preview)) {
    if (!app.gam().gam_cat_loaded) ImGui::TextDisabled("缺少 gam_catalog.bin");
    for (int i = 0; i < static_cast<int>(app.gam().gam_catalog.cats.size()); ++i)
      if (ImGui::Selectable(app.gam().gam_catalog.cats[static_cast<size_t>(i)].c_str(),
                            i == app.gam().gam_sel_cat))
        app.gam().gam_sel_cat = i;
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  const auto& ents = (app.gam().gam_cat_loaded &&
                      app.gam().gam_sel_cat < static_cast<int>(app.gam().gam_catalog.cats.size()))
                         ? app.gam().gam_catalog.items[static_cast<size_t>(app.gam().gam_sel_cat)]
                         : std::vector<GamCatalog::Entry>();
  static std::string preview_buf;
  if (!ents.empty() && app.gam().gam_sel_cat_item < static_cast<int>(ents.size())) {
    std::string dm =
        gam_item_disp_name(app, ents[static_cast<size_t>(app.gam().gam_sel_cat_item)].name);
    preview_buf = "Lv" +
                  std::to_string(ents[static_cast<size_t>(app.gam().gam_sel_cat_item)].level) +
                  " " + dm;
  } else {
    preview_buf = "选择物品";
  }
  ImGui::SetNextItemWidth(item_w);
  bool item_combo_open =
      ImGui::BeginCombo("##item", preview_buf.c_str(), ImGuiComboFlags_HeightLarge);
  static int tip_i = -1;
  static int tip_cat = -1;
  static std::string tip;
  if (item_combo_open) {
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(ents.size()));
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        std::string lab = "Lv" + std::to_string(ents[static_cast<size_t>(i)].level) + " " +
                          gam_item_disp_name(app, ents[static_cast<size_t>(i)].name);
        lab += "##ci" + std::to_string(i);
        bool sel = (i == app.gam().gam_sel_cat_item);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Selectable(lab.c_str(), sel)) app.gam().gam_sel_cat_item = i;
        if (ImGui::IsItemHovered()) {
          if (tip_i != i || tip_cat != app.gam().gam_sel_cat) {
            tip = gam_format_item_tip(app, ents[static_cast<size_t>(i)].rec);
            tip_i = i;
            tip_cat = app.gam().gam_sel_cat;
          }
          if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
        }
      }
    }
    ImGui::EndCombo();
  } else {
    if (ImGui::IsItemHovered() && !ents.empty() && app.gam().gam_sel_cat_item >= 0 &&
        app.gam().gam_sel_cat_item < static_cast<int>(ents.size())) {
      if (tip_i != app.gam().gam_sel_cat_item || tip_cat != app.gam().gam_sel_cat) {
        tip = gam_format_item_tip(app, ents[static_cast<size_t>(app.gam().gam_sel_cat_item)].rec);
        tip_i = app.gam().gam_sel_cat_item;
        tip_cat = app.gam().gam_sel_cat;
      }
      if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("创建物品")) gam_on_create(app);
  ImGui::TextDisabled("（先选中左侧地图，再把目录物品新建到该地图；选中具体物品后可悬停看属性）");
}

}  // namespace poq
