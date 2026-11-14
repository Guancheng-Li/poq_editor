// 秦殇物品修改器 (PoqEditor) —— 主窗口框架与 Tab 栏。

#include "imgui.h"
#include "UI/App.h"

namespace poq {

void render_mem_tab(App& app);
void render_cfg_tab(App& app);
void render_top_bar(App& app);

// ============================================================================
// Dear ImGui 深色主题，配蓝色标题栏与强调色。
// ============================================================================

void apply_blue_theme() {
  // 默认 imgui 蓝色主题(深色 + 蓝色强调色)
  ImGui::StyleColorsDark();
  // 标题栏统一蓝色(经典 Windows 蓝):"关于"/消息等弹窗一致。
  // 主窗口走系统标题栏(NoTitleBar),不受影响。
  ImGuiStyle& style = ImGui::GetStyle();
  style.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.24f, 0.48f, 1.00f);
  style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.40f, 0.78f, 1.00f);
  style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.24f, 0.48f, 0.80f);
}
// ============================================================================
// 主窗口 / 主循环
// ============================================================================

void render_main_window(App& app) {
  app.ensure_process_list();
  // 精确贴住系统窗口客户区(左上 (0,0) 开始,尺寸=工作区);
  // 去掉 imgui 自绘标题栏,用系统标题栏(标题一致),不重复。
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("秦殇修改器", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

  render_top_bar(app);
  ImGui::Separator();

  // 选项卡区域(固定高度,把状态栏钉在底部)
  const float status_h = 26.0f;
  ImVec2 avail = ImGui::GetContentRegionAvail();
  float tab_h = avail.y - status_h - 8.0f;
  if (tab_h < 100.0f) tab_h = 100.0f;

  ImGui::BeginChild("tabarea", ImVec2(avail.x, tab_h));
  app.gm_visible = false;  // 每帧先复位,GM tab 渲染时置 true(周期刷新只在可见时跑)
  if (ImGui::BeginTabBar("maintabs")) {
    if (ImGui::BeginTabItem("人物修改")) {
      app.cur_tab = 0;
      render_char_tab(app);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("道具修改")) {
      app.cur_tab = 1;
      render_basic_fields(app);
      ImGui::Separator();
      float table_h = ImGui::GetContentRegionAvail().y;
      if (table_h < 60.0f) table_h = 60.0f;
      render_attr_table(app, table_h);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("召唤修改")) {
      app.cur_tab = 2;
      render_summon_tab(app);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("GM命令")) {
      app.cur_tab = 3;
      render_gm_tab(app);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("H2解密")) {
      app.cur_tab = 4;
      render_h2_tab(app);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("gam修改")) {
      app.cur_tab = 5;
      render_gam_tab(app);
      ImGui::EndTabItem();
    }
    if (!app.hidden && ImGui::BeginTabItem("内存查看")) {
      app.cur_tab = 6;
      render_mem_tab(app);
      ImGui::EndTabItem();
    }
    if (!app.hidden && ImGui::BeginTabItem("偏移设置")) {
      app.cur_tab = 7;
      render_cfg_tab(app);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::EndChild();

  render_help_popup(app);

  // 状态栏
  ImGui::Separator();
  ImGui::TextUnformatted(app.status.c_str());

  ImGui::End();
}

}  // namespace poq
