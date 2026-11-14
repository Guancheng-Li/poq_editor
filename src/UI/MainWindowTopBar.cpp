// 主窗口 —— 顶部进程栏与版本切换。

#include <string>
#include <vector>
#include "imgui.h"
#include "Core/GameModder.h"
#include "UI/App.h"
#include "UI/AppCfgState.h"
#include "UI/AppProcessState.h"

namespace poq {

void render_top_bar(App& app) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float spacing = style.ItemSpacing.x;
  const float frame_pad_x = style.FramePadding.x * 2.0f;

  // 右侧按钮组总宽(刷新进程 + 初始化 + 帮助,用于让下拉框占满剩余宽度并右对齐)
  const char* kRefresh = "刷新进程";
  const char* kInit = "初始化";
  const char* kHelp = "帮助";
  const float refresh_w = ImGui::CalcTextSize(kRefresh).x + frame_pad_x;
  const float init_w = ImGui::CalcTextSize(kInit).x + frame_pad_x;
  const float help_w = ImGui::CalcTextSize(kHelp).x + frame_pad_x;
  const float buttons_w = refresh_w + init_w + help_w + spacing * 2.0f;

  // 游戏版本(mode)下拉项:显示 mode 名(v1.30 / Steam版)
  std::vector<std::string> ver_labels;
  for (const auto& kv : app.cfg().cfg.modes) ver_labels.push_back(kv.second.name);
  std::vector<const char*> ver_items;
  for (const auto& s : ver_labels) ver_items.push_back(s.c_str());

  // 进程下拉(POQ.EXE)
  std::vector<std::string> proc_labels;
  for (const auto& p : app.proc().processes) proc_labels.push_back(p.display());
  std::vector<const char*> proc_items;
  for (const auto& s : proc_labels) proc_items.push_back(s.c_str());

  // 版本标签固定 + 版本下拉固定宽;进程下拉占满剩余;按钮贴右缘
  const char* kVerLabel = "游戏版本:";
  const char* kProcLabel = "选择进程:";
  const float ver_label_w = ImGui::CalcTextSize(kVerLabel).x;
  const float proc_label_w = ImGui::CalcTextSize(kProcLabel).x;
  const float ver_w = 150.0f;
  const float row_w = ImGui::GetContentRegionAvail().x;
  float proc_w = row_w - ver_label_w - ver_w - proc_label_w - buttons_w - spacing * 4.0f;
  if (proc_w < 80.0f) proc_w = 80.0f;  // 窗口过窄时给进程下拉保底

  // 游戏版本切换 = 切换 offsets 模式:先还原旧模式注入,再切换并提示重新初始化
  ImGui::TextUnformatted(kVerLabel);
  ImGui::SameLine(0, spacing);
  ImGui::SetNextItemWidth(ver_w);
  int ver_sel = app.cfg().mode_selected;
  if (ImGui::Combo("##ver", &ver_sel, ver_items.data(), static_cast<int>(ver_items.size()))) {
    if (ver_sel != app.cfg().mode_selected && ver_sel >= 0 &&
        static_cast<size_t>(ver_sel) < app.cfg().mode_ids.size()) {
      const std::string& mid = app.cfg().mode_ids[static_cast<size_t>(ver_sel)];
      if (app.modder().mem().is_open()) {
        auto cr = app.modder().close();  // 还原旧模式注入,避免残留 hook
        if (!cr.hook_restored || !cr.cave_freed) {
          // 计划 P0: hook 没还原成功 -> 游戏里仍指向 cave, 必须让用户知道
          std::string msg = "旧版本的注入未能完全还原：\n";
          msg += cr.error;
          msg += "\n\n游戏内仍可能残留指向已分配内存的跳转。";
          msg += "\n建议关闭并重启游戏后再切换版本。";
          msg_box("清理失败", msg, MB_ICONERROR);
        }
      }
      app.cfg().cfg.set_current(mid);
      app.modder().config() = app.cfg().cfg;
      app.reload_cfg_buffers();
      app.refresh_processes();
      app.set_status("已切换游戏版本:" + app.cfg().cfg.current().name + "。请重新初始化。");
    }
  }
  ImGui::SameLine(0, spacing);
  ImGui::TextUnformatted(kProcLabel);
  ImGui::SameLine(0, spacing);
  ImGui::SetNextItemWidth(proc_w);
  int sel = app.proc().proc_selected;
  if (ImGui::Combo("##proc", &sel, proc_items.data(), static_cast<int>(proc_items.size()))) {
    app.proc().proc_selected = sel;
  }
  ImGui::SameLine(0, spacing);
  if (ImGui::Button("刷新进程")) app.refresh_processes();
  ImGui::SameLine(0, spacing);
  if (ImGui::Button("初始化")) on_init(app);
  ImGui::SameLine(0, spacing);
  if (ImGui::Button("帮助")) {
    // 弹窗打开瞬间跳到当前所在 tab 的说明(仅第一帧生效,之后可自由切换)
    app.help_tab = app.cur_tab;
    app.help_jump = true;
    ImGui::OpenPopup("帮助");
  }
}
}  // namespace poq
