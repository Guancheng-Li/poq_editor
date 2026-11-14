// H2 Tab —— ImGui 与确认弹窗。

#include <string>
#include "imgui.h"
#include "UI/App.h"
#include "UI/H2TabLogic.h"

namespace poq {

void h2_render_backup_confirm(App& app) {
  const char* popup_id = "备份已存在##h2bak";
  if (!ImGui::IsPopupOpen(popup_id)) ImGui::OpenPopup(popup_id);
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal(
          popup_id, nullptr,
          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
    const float win_w = ImGui::GetWindowSize().x;
    ImGui::PushTextWrapPos(win_w - 40.0f);
    ImGui::TextUnformatted("已存在备份文件，请选择：");
    ImGui::TextUnformatted(wide_to_utf8(h2_backup_path(app.h2_path, app.h2_decrypt)).c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 14));
    const float btn_w = 150.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float total = btn_w * 3.0f + spacing * 2.0f;
    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - total) * 0.5f);
    if (ImGui::Button("覆盖备份转换", ImVec2(btn_w, 0))) {
      h2_do_convert(app, app.h2_path, app.h2_decrypt, /*make_backup=*/true);
      app.h2_wait_confirm = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("跳过备份转换", ImVec2(btn_w, 0))) {
      h2_do_convert(app, app.h2_path, app.h2_decrypt, /*make_backup=*/false);
      app.h2_wait_confirm = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("取消转换", ImVec2(btn_w, 0))) {
      app.h2_wait_confirm = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

// 目录转换确认弹窗(提示会转换整个目录含子文件夹的 txt 文件,请做好备份)。
void h2_render_dir_confirm(App& app) {
  const char* popup_id = "目录转换确认##h2dir";
  if (!ImGui::IsPopupOpen(popup_id)) ImGui::OpenPopup(popup_id);
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal(
          popup_id, nullptr,
          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
    const float win_w = ImGui::GetWindowSize().x;
    ImGui::PushTextWrapPos(win_w - 40.0f);
    ImGui::TextUnformatted("将会转换整个目录（包括子文件夹）中的 txt 文件，请做好备份！");
    ImGui::TextUnformatted(app.h2_dir_decrypt ? "已是明文（解密态）的文件将自动跳过。"
                                              : "已是加密态的文件将自动跳过。");
    ImGui::TextWrapped("目录：%s", wide_to_utf8(app.h2_dir).c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 14));
    const float btn_w = 140.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float total = btn_w * 2.0f + spacing;
    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - total) * 0.5f);
    if (ImGui::Button("开始转换", ImVec2(btn_w, 0))) {
      h2_run_batch(app, app.h2_dir, app.h2_dir_decrypt);
      app.h2_wait_dir_confirm = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(btn_w, 0))) {
      app.h2_wait_dir_confirm = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

// H2解密 tab 主渲染。
void render_h2_tab(App& app) {
  const float btn_w = ImGui::CalcTextSize("解密目录").x + ImGui::GetStyle().FramePadding.x * 2.0f;
  ImGui::TextDisabled("单个文件：");
  if (ImGui::Button("解密", ImVec2(btn_w, 0))) h2_on_convert(app, /*decrypt=*/true);
  ImGui::SameLine();
  if (ImGui::Button("加密", ImVec2(btn_w, 0))) h2_on_convert(app, /*decrypt=*/false);
  ImGui::TextDisabled("整个目录（含子文件夹）：");
  if (ImGui::Button("解密目录", ImVec2(btn_w, 0))) h2_on_batch(app, /*decrypt=*/true);
  ImGui::SameLine();
  if (ImGui::Button("加密目录", ImVec2(btn_w, 0))) h2_on_batch(app, /*decrypt=*/false);

  // 已有备份 -> 三选一弹窗
  if (app.h2_wait_confirm) h2_render_backup_confirm(app);
  // 目录确认 -> 二选弹窗
  if (app.h2_wait_dir_confirm) h2_render_dir_confirm(app);
}
}  // namespace poq
