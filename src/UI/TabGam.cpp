// 秦殇物品修改器 (PoqEditor) —— GAM 存档 Tab 入口与 prefs。
// 顶栏/创建 → GamTabToolbar.cpp；树/编辑 → GamTabTree.cpp / GamTabEdit.cpp；命令 →
// GamTabLogic.cpp。

#include <windows.h>
#include <cstdio>
#include <string>
#include "imgui.h"
#include "UI/App.h"
#include "UI/AppGamState.h"
#include "UI/GamTabUi.h"

namespace poq {

static std::wstring gam_prefs_path() {
  return app_dir_w() + L"\\poqeditor_gam.ini";
}

std::string gam_load_prefs() {
  FILE* fp = _wfopen(gam_prefs_path().c_str(), L"rb");
  if (!fp) return "";
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  std::string s(buf, n);
  size_t pos = s.find_first_of("\r\n");
  if (pos != std::string::npos) s = s.substr(0, pos);
  return s;
}

std::wstring timestamped_bak_path(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  std::wstring dir = (slash == std::wstring::npos) ? L"" : path.substr(0, slash + 1);
  std::wstring fn = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
  size_t dot = fn.find_last_of(L'.');
  std::wstring base = (dot == std::wstring::npos) ? fn : fn.substr(0, dot);
  std::wstring ext = (dot == std::wstring::npos) ? L"" : fn.substr(dot);
  SYSTEMTIME st;
  ::GetLocalTime(&st);
  wchar_t ts[32];
  ::swprintf(ts, 32, L"%04d%02d%02d%02d%02d%02d", static_cast<int>(st.wYear),
             static_cast<int>(st.wMonth), static_cast<int>(st.wDay), static_cast<int>(st.wHour),
             static_cast<int>(st.wMinute), static_cast<int>(st.wSecond));
  return dir + base + L"." + ts + L".bak" + ext;
}

void render_gam_tab(App& app) {
  app.ensure_gam_data_loaded();
  render_gam_command_bar(app);
  ImGui::Separator();
  render_gam_create_row(app);
  ImGui::Separator();

  if (!app.gam().gam_loaded) {
    ImGui::TextDisabled("尚未打开 gam。点\"打开gam\"选择存档(默认定位到游戏目录)。");
    return;
  }

  float avail = ImGui::GetContentRegionAvail().x;
  float w = avail * 0.42f;
  if (w < 260.0f) w = 260.0f;
  ImGui::BeginChild("gamleft", ImVec2(w, ImGui::GetContentRegionAvail().y),
                    ImGuiChildFlags_Borders);
  render_gam_tree(app);
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("gamright", ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y),
                    ImGuiChildFlags_Borders);
  render_gam_edit(app);
  ImGui::EndChild();
}

}  // namespace poq
