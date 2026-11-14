// 角色 Tab —— 全局属性 + 角色字段网格 UI。

#include <cstdio>
#include <string>
#include <vector>
#include "imgui.h"
#include "Core/Characters.h"
#include "UI/App.h"
#include "UI/AppCharState.h"

namespace poq {

void render_char_tab(App& app) {
  // ---- 全局属性(队伍共享,置顶)----
  ImGui::TextUnformatted("全局属性");
  ImGui::Separator();
  ImGui::TextUnformatted("金钱:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140);
  ImGui::InputText("##money", app.char_state().money_buf, sizeof(app.char_state().money_buf));
  ImGui::SameLine();
  if (ImGui::Button("修改金钱")) on_money_apply(app);
  ImGui::SameLine();
  bool inv = app.char_state().ck_invincible;
  if (ImGui::Checkbox("无敌模式", &inv)) {
    app.char_state().ck_invincible = inv;
    on_invincible_toggled(app, inv);
  }
  ImGui::SameLine();
  bool kill = app.char_state().ck_kill;
  if (ImGui::Checkbox("一击必杀", &kill)) {
    app.char_state().ck_kill = kill;
    on_kill_toggled(app, kill);
  }

  // ---- 角色选择 + 读取/保存 ----
  ImGui::Separator();
  ImGui::TextUnformatted("角色:");
  ImGui::SameLine();
  std::vector<std::string> char_labels;
  for (size_t i = 0; i < app.char_state().char_names.size(); ++i) {
    // 下拉格式 = "角色N-职业-名字"(前缀"角色N-" + 前面角色类型,后面角色名字)
    auto it = known_members().find(app.char_state().char_names[i]);
    std::string cls = (it != known_members().end()) ? it->second + "-" : "";
    char_labels.push_back("角色" + std::to_string(i + 1) + "-" + cls +
                          app.char_state().char_names[i]);
  }
  std::vector<const char*> char_items;
  for (const auto& l : char_labels) char_items.push_back(l.c_str());
  int csel = app.char_state().char_selected;
  ImGui::SetNextItemWidth(220);
  if (ImGui::Combo("##char", &csel, char_items.data(), static_cast<int>(char_items.size()))) {
    if (csel != app.char_state().char_selected && csel >= 0 &&
        static_cast<size_t>(csel) < app.char_state().char_names.size()) {
      app.char_state().char_selected = csel;
      rebuild_char_widgets(app);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("读取属性")) on_char_read(app);
  ImGui::SameLine();
  if (ImGui::Button("保存属性")) on_char_save(app);
  ImGui::SameLine();
  bool filter_on = app.char_state().char_filter;
  if (ImGui::Checkbox("角色属性校验过滤", &filter_on)) {
    if (!filter_on) app.char_state().char_filter_warn = true;
    app.char_state().char_filter = filter_on;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", app.char_state().char_status.c_str());
  if (!app.hidden && !app.char_state().char_slot_probe.empty()) {
    ImGui::TextDisabled("槽位探测:");
    for (size_t i = 0; i < app.char_state().char_slot_probe.size(); ++i) {
      ImGui::TextDisabled("  槽%zu=%s", i + 1, app.char_state().char_slot_probe[i].c_str());
    }
  }
  // 关闭"角色属性校验过滤"时的警告弹窗(居中、不可移动)
  if (app.char_state().char_filter_warn) {
    app.char_state().char_filter_warn = false;
    ImGui::OpenPopup("警告##charfilter");
  }
  ImGui::SetNextWindowSize(ImVec2(460, 170), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal(
          "警告##charfilter", nullptr,
          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
    // 每帧强制整窗居中(不依赖 Appearing 锚点,避免 AutoResize 导致偏位)
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetWindowPos(ImVec2(center.x - ImGui::GetWindowSize().x * 0.5f,
                               center.y - ImGui::GetWindowSize().y * 0.5f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 20);
    ImGui::TextWrapped("如果取消校验过滤，可能识别出其他角色，请谨慎修改！");
    ImGui::Spacing();
    const float btn_w = 120;
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - btn_w) * 0.5f);
    ImGui::SetCursorPosY(ImGui::GetWindowSize().y - 40);
    if (ImGui::Button("确认", ImVec2(btn_w, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  ImGui::Separator();

  // ---- 字段网格(task6 指定排列:3 列行 + 2 列行)----
  // 每格 = {字段名, 宽度权重}。权重用于让"经验"占两格宽(它是成对字段、渲染成两个输入框)。
  struct CharCol {
    const char* label;
    int w;
  };
  static const CharCol kLayout[][3] = {
      {{"姓名", 1}, {"别名", 1}, {"职业", 1}},
      {{"年龄", 1}, {"性别", 1}, {"等级", 1}},
      {{"移动速度", 1}, {"视野（迷雾去除）", 1}, {nullptr, 0}},  // 2026-10-17 新增: 独立行
      {{"经验", 2}, {"声望", 1}, {nullptr, 0}},
      {{"生命", 1}, {"精气", 1}, {nullptr, 0}},
      {{"力量", 1}, {"体质", 1}, {nullptr, 0}},
      {{"敏捷", 1}, {"悟性", 1}, {nullptr, 0}},
      {{"魅力", 1}, {"智慧", 1}, {nullptr, 0}},
      {{"普通攻击", 1}, {"普通防御", 1}, {nullptr, 0}},
      {{"金攻击", 1}, {"金防御", 1}, {nullptr, 0}},
      {{"木攻击", 1}, {"木防御", 1}, {nullptr, 0}},
      {{"水攻击", 1}, {"水防御", 1}, {nullptr, 0}},
      {{"火攻击", 1}, {"火防御", 1}, {nullptr, 0}},
      {{"土攻击", 1}, {"土防御", 1}, {nullptr, 0}},
      {{"命中率", 1}, {"闪避率", 1}, {nullptr, 0}},  // 命中率/闪避率 左右对调
      {{"剩余属性点", 1}, {"剩余技能点", 1}, {nullptr, 0}},
  };
  static std::vector<EnumComboOptions> enum_options = [] {
    const auto& fields = char_fields();
    std::vector<EnumComboOptions> options(fields.size());
    for (size_t i = 0; i < fields.size(); ++i)
      if (fields[i].kind == CharKind::Enum) options[i].build(fields[i].enum_map, true);
    return options;
  }();

  // 渲染一个字段(标签在上,输入框在下,填满列宽;pair 两个输入框 + "/")。
  auto render_field = [&](const char* label) {
    int idx = find_char_field(label);
    if (idx < 0) {
      ImGui::TextDisabled("%s(未定义)", label);
      return;
    }
    const auto& f = char_fields()[static_cast<size_t>(idx)];
    CharWidget& w = app.char_state().char_widgets[static_cast<size_t>(idx)];
    if (!f.writable) {
      ImGui::TextDisabled("%s(只读)", label);
      return;
    }
    if (!w.loaded) {
      ImGui::TextDisabled("%s(读取失败)", label);
      return;
    }
    bool disabled = app.char_state().char_addrs.empty() || !is_char_field_editable(app, f);
    ImGui::TextUnformatted(label);
    ImGui::BeginDisabled(disabled);
    std::string id = "##ch" + std::to_string(idx);
    if (f.kind == CharKind::StringPtr) {
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::InputText(id.c_str(), w.strbuf, sizeof(w.strbuf));
    } else if (f.kind == CharKind::Enum) {
      auto& options = enum_options[static_cast<size_t>(idx)];
      int cur = options.index_of(w.intval);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::Combo(id.c_str(), &cur, EnumComboOptions::label_at, &options,
                       static_cast<int>(options.labels.size()))) {
        if (cur >= 0) w.intval = options.values[static_cast<size_t>(cur)];
      }
    } else if (f.kind == CharKind::Pair) {
      float avail = ImGui::GetContentRegionAvail().x;
      float slash = ImGui::CalcTextSize("/").x + ImGui::GetStyle().ItemSpacing.x;
      float each = (avail - slash) * 0.5f;
      if (each < 30.0f) each = 30.0f;
      ImGui::SetNextItemWidth(each);
      ImGui::InputInt(("##chl" + std::to_string(idx)).c_str(), &w.pair_lo, 0, 0);
      ImGui::SameLine(0, 0);
      ImGui::TextUnformatted("/");
      ImGui::SameLine(0, 0);
      ImGui::SetNextItemWidth(each);
      ImGui::InputInt(("##chh" + std::to_string(idx)).c_str(), &w.pair_hi, 0, 0);
    } else {
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::InputInt(id.c_str(), &w.intval, 0, 0);
    }
    ImGui::EndDisabled();
    if (f.hint) ImGui::TextDisabled("%s", f.hint->c_str());
  };

  float grid_h = ImGui::GetContentRegionAvail().y;
  if (grid_h < 80.0f) grid_h = 80.0f;
  ImGui::BeginChild("charfields", ImVec2(-FLT_MIN, grid_h), ImGuiChildFlags_None,
                    ImGuiWindowFlags_None);
  for (size_t r = 0; r < sizeof(kLayout) / sizeof(kLayout[0]); ++r) {
    int ncols = kLayout[r][2].label ? 3 : 2;
    char tid[32];
    std::snprintf(tid, sizeof(tid), "charrow%zu", r);
    if (ImGui::BeginTable(tid, ncols)) {
      for (int c = 0; c < ncols; ++c)
        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch,
                                static_cast<float>(kLayout[r][c].w));
      ImGui::TableNextRow();
      for (int c = 0; c < ncols; ++c) {
        if (ImGui::TableSetColumnIndex(c)) render_field(kLayout[r][c].label);
      }
      ImGui::EndTable();
    }
  }
  // 2026-10-17 实测: 视野(+0x198)/移动速度(+0x190) 直写都安全, 但游戏只在换图/
  // 进图时读取 —— 提示用户生效时机, 免得以为没生效反复改。
  ImGui::TextDisabled("视野/移动速度改写换图/读档后生效。");
  ImGui::EndChild();
}
}  // namespace poq
