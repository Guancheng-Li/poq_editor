// GM Tab —— ImGui 布局(对齐 python gm_tab.py)。

#include <cstdio>
#include <string>
#include "imgui.h"
#include "UI/App.h"
#include "UI/AppCfgState.h"
#include "UI/GmTabLogic.h"

namespace poq {

void render_gm_tab(App& app) {
  app.gm_visible = true;
  // 版本切换 -> 重置激活态(对齐 python reload_mode)
  if (app.gm_mode_cache != app.cfg().cfg.current_mode) {
    app.gm_mode_cache = app.cfg().cfg.current_mode;
    app.gm_active = false;
    gm_refresh_ready(app);
  }

  // 第1部分:激活状态行(左) + 激活方式 + 激活/还原(右)
  std::string status_text = app.gm_active ? "GM权限已激活" : "GM权限未激活";
  const ImVec4 status_color =
      app.gm_active ? ImVec4(0.0f, 0.8f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float row_w = ImGui::GetContentRegionAvail().x;
  // 三个按钮(激活/还原、执行、读取配置)统一宽度,统一右对齐
  const float gm_btn_w =
      ImGui::CalcTextSize("读取配置").x + ImGui::GetStyle().FramePadding.x * 2.0f;
  const float method_w = 140.0f;
  const float method_label_w = ImGui::CalcTextSize("激活方式：").x;
  ImGui::TextColored(status_color, "%s", status_text.c_str());
  float right_w = method_label_w + method_w + gm_btn_w + spacing * 2.0f;
  if (ImGui::CalcTextSize(status_text.c_str()).x + right_w + spacing <= row_w)
    ImGui::SameLine(row_w - right_w);
  ImGui::TextUnformatted("激活方式：");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(method_w);
  const char* method_items[] = {"内存修改", "R9注入"};
  ImGui::Combo("##gm_method", &app.gm_method, method_items, 2);
  ImGui::SameLine();
  bool act_click = ImGui::Button(app.gm_active ? "还原" : "激活", ImVec2(gm_btn_w, 0));
  if (act_click) on_gm_toggle(app);
  ImGui::Separator();

  // 第3部分:命令输入 + 执行次数 + 执行(全部一行)
  ImGui::TextUnformatted("命令：");
  ImGui::SameLine();
  // 右侧固定部分:执行次数checkbox + 次数输入框 + 执行按钮(与激活/读取配置同宽),整体右对齐
  const float ck_total = ImGui::GetFrameHeight() + spacing + ImGui::CalcTextSize("执行次数").x;
  const float count_w = 80.0f;
  const float right_fixed = ck_total + count_w + gm_btn_w + spacing * 2.0f;
  float cmd_w = ImGui::GetContentRegionAvail().x - right_fixed - spacing;
  if (cmd_w < 150.0f) cmd_w = 150.0f;
  ImGui::SetNextItemWidth(cmd_w);
  if (!app.gm_active) ImGui::BeginDisabled();
  bool exec_pressed = ImGui::InputText("##gm_cmd", app.gm_cmd_buf, sizeof(app.gm_cmd_buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine(row_w - right_fixed);  // 右对齐:执行次数/次数框/执行按钮贴右缘
  bool ck = app.ck_count;
  if (ImGui::Checkbox("执行次数", &ck)) app.ck_count = ck;
  ImGui::SameLine();
  bool count_en = app.gm_active && app.ck_count;
  if (!count_en) ImGui::BeginDisabled();
  ImGui::SetNextItemWidth(80);
  if (ImGui::InputText("##gm_count", app.gm_count_buf, sizeof(app.gm_count_buf),
                       ImGuiInputTextFlags_EnterReturnsTrue))
    exec_pressed = true;
  if (!count_en) ImGui::EndDisabled();
  ImGui::SameLine();
  if (app.gm_exec_pending) ImGui::BeginDisabled();
  if (ImGui::Button(app.gm_exec_pending ? "执行中…" : "执行", ImVec2(gm_btn_w, 0)))
    exec_pressed = true;
  if (app.gm_exec_pending) ImGui::EndDisabled();
  if (!app.gm_active) ImGui::EndDisabled();
  if (exec_pressed && !app.gm_exec_pending) on_gm_exec(app);

  // 第3.5部分:常用命令(一行:"常用命令：" 标签 + 右侧"读取配置"按钮;
  // 下方展开全部选项,点选后填入命令输入框)
  ImGui::TextUnformatted("常用命令：");
  ImGui::SameLine(ImGui::GetContentRegionAvail().x - gm_btn_w);
  if (ImGui::Button("读取配置", ImVec2(gm_btn_w, 0))) {
    // 重新读取 gm_cmd.txt;有错误行 -> 弹窗提示并保留上一次的选项
    int err = load_gm_cmd_list(app, /*keep_previous_on_error=*/true);
    if (err > 0) {
      std::string msg =
          "常用命令文件 gm_cmd.txt 有 " + std::to_string(err) + " 行格式错误，已保留上一次的选项。";
      msg_box("读取配置", msg, MB_ICONWARNING);
      append_gm_log(app, "读取配置：" + msg);
    } else {
      app.set_status("已重新读取常用命令 (" + std::to_string(app.gm_cmd_list.size()) + " 条)");
      append_gm_log(
          app, "读取配置：已重新读取 " + std::to_string(app.gm_cmd_list.size()) + " 条常用命令");
    }
  }
  // 下方:展开全部选项(自动限高,超出滚动);点选 -> 命令输入框显示对应命令
  app.gm_cmdlist_focused = false;
  if (app.gm_cmd_list.empty()) {
    ImGui::TextDisabled("（暂无常用命令：可编辑同目录 gm_cmd.txt 后点“读取配置”加载）");
  } else {
    // 占满下方剩余空间;下方还要画 F12 调试日志时预留其高度
    float list_h = ImGui::GetContentRegionAvail().y;
    if (!app.hidden && !app.gm_log.empty())
      list_h -= (ImGui::GetTextLineHeightWithSpacing() + 130.0f + spacing * 3.0f + 2.0f);
    if (list_h < 30.0f) list_h = 30.0f;
    if (!app.gm_active) ImGui::BeginDisabled();
    ImGui::BeginChild("##gm_cmdlist", ImVec2(ImGui::GetContentRegionAvail().x, list_h),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    app.gm_cmdlist_focused = ImGui::IsWindowFocused();  // 列表持焦点时箭头留给列表滚动
    for (int i = 0; i < static_cast<int>(app.gm_cmd_list.size()); ++i) {
      const auto& kv = app.gm_cmd_list[static_cast<size_t>(i)];
      bool selected = (i == app.gm_cmdlist_sel);
      if (ImGui::Selectable(kv.second.c_str(), selected)) {
        app.gm_cmdlist_sel = i;
        std::snprintf(app.gm_cmd_buf, sizeof(app.gm_cmd_buf), "%s", kv.first.c_str());
      }
    }
    ImGui::EndChild();
    if (!app.gm_active) ImGui::EndDisabled();
  }

  // 上下箭头切换历史命令(仅当常用命令列表未持有焦点;单行输入框不消费上下箭头)。
  // 上:从最新一条往前翻,到第一条失效;下:往回翻,翻过最新一条后还原浏览前的输入原文,
  // 再按失效。
  if (!app.gm_cmdlist_focused && !app.gm_history.empty()) {
    const int n = static_cast<int>(app.gm_history.size());
    int idx = app.gm_hist_idx;
    std::string new_text;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
      if (idx == -1) {
        app.gm_hist_draft = app.gm_cmd_buf;  // 保存输入框原文
        idx = n - 1;                         // 从最新一条开始浏览
      } else if (idx > 0) {
        --idx;  // 到第一条(0)时不再上翻
      }
    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
      if (idx >= 0) {
        if (idx < n - 1) {
          ++idx;  // 往新的方向翻
        } else {
          idx = -1;  // 已在最新一条:下翻回到输入框原文,再按失效
        }
      }
    }
    if (idx != app.gm_hist_idx) {
      app.gm_hist_idx = idx;
      const std::string& text =
          (idx == -1) ? app.gm_hist_draft : app.gm_history[static_cast<size_t>(idx)];
      std::snprintf(app.gm_cmd_buf, sizeof(app.gm_cmd_buf), "%s", text.c_str());
    }
  }

  // F12 调试模式:显示工具日志(对齐 summon tab 模式)
  if (!app.hidden && !app.gm_log.empty()) {
    ImGui::Separator();
    ImGui::TextUnformatted("工具输出:");
    ImGui::SameLine();
    if (ImGui::Button("复制##gmlog")) ImGui::SetClipboardText(app.gm_log.c_str());
    ImGui::SameLine();
    if (ImGui::Button("清空##gmlog")) app.gm_log.clear();
    ImGui::BeginChild("gm_log", ImVec2(-FLT_MIN, 130), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(app.gm_log.c_str());
    ImGui::EndChild();
  }
}
}  // namespace poq
