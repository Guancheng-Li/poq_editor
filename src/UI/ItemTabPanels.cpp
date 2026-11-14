// 道具 Tab —— 基础字段网格 + 词条表格 UI (MainWindow 道具页调用)。

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "imgui.h"
#include "Core/GameModder.h"
#include "Core/ItemData.h"
#include "Core/ItemProperty.h"
#include "UI/App.h"
#include "UI/AppCfgState.h"
#include "UI/AppItemState.h"

namespace poq {

int index_of(const std::vector<uint32_t>& ids, uint32_t v) {
  for (size_t i = 0; i < ids.size(); ++i)
    if (ids[i] == v) return static_cast<int>(i);
  return -1;
}

void render_basic_fields(App& app) {
  auto type_val = app.current_type_value();
  const float avail = ImGui::GetContentRegionAvail().x;
  // 网格列数:正常宽度下武器名字/别名 2 列一行、其余 3 列一行;窗口窄时递减。
  const int cols = avail >= 900.0f ? 3 : (avail >= 600.0f ? 2 : 1);
  // 前 2 个字段(武器名字/别名)是否单独用 2 列各占半行(窗口太窄时并入后面表格)。
  const size_t head_n = (cols >= 2 && app.cfg().basic_fields.size() >= 2) ? 2 : 0;

  // 渲染单个字段(labels 在上、输入框在下、hint 灰字在输入框下方,输入框填满列宽)。
  auto render_field = [&](size_t i) {
    const auto& f = app.cfg().basic_fields[i];
    AppItemState::BasicWidget& w = app.item().basic[i];

    // 条件/强制字段:渲染时即时计算(逻辑对齐 cpp UpdateConditionalFields)
    bool forced = f.force_when && f.is_forced(type_val);
    bool cond_inactive =
        f.conditional && f.conditional->field == "类型" &&
        !(type_val && std::find(f.conditional->values.begin(), f.conditional->values.end(),
                                *type_val) != f.conditional->values.end());
    bool disabled = f.readonly || forced || cond_inactive;

    std::string label = f.label;
    if (f.readonly) label += "（只读）";

    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::BeginDisabled(disabled);

    if (f.type == "string_ptr") {
      ImGui::InputText(("##b" + std::to_string(i)).c_str(), w.strbuf, sizeof(w.strbuf),
                       ImGuiInputTextFlags_None);
    } else if (w.is_enum) {
      auto& options = app.item().basic_enum_options[i];
      int display = w.enum_val;
      if (forced) display = f.force_when->value;
      int cur = options.index_of(display);
      if (ImGui::Combo(("##b" + std::to_string(i)).c_str(), &cur, EnumComboOptions::label_at,
                       &options, static_cast<int>(options.labels.size()))) {
        if (cur >= 0) w.enum_val = options.values[static_cast<size_t>(cur)];
      }
    } else {
      int edit = w.intval;
      if (forced)
        edit = f.force_when->value;
      else if (cond_inactive)
        edit = -1;
      if (ImGui::InputInt(("##b" + std::to_string(i)).c_str(), &edit)) w.intval = edit;
    }
    ImGui::EndDisabled();
    if (f.hint) {
      ImGui::TextDisabled("%s", f.hint->c_str());
    }
  };

  // 武器名字/武器别名:2 列,各占半行。
  if (head_n > 0) {
    if (ImGui::BeginTable("basichead", 2)) {
      render_field(0);
      render_field(1);
      ImGui::EndTable();
    }
  }
  // 其余属性:3 列一行(窄窗口退化为 2/1 列)。
  if (app.cfg().basic_fields.size() > head_n) {
    if (ImGui::BeginTable("basicrest", cols)) {
      for (size_t i = head_n; i < app.cfg().basic_fields.size(); ++i) render_field(i);
      ImGui::EndTable();
    }
  }
}

void render_attr_table(App& app, float height) {
  // 按钮行
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
  if (app.item().item_read_pending) ImGui::BeginDisabled();
  if (ImGui::Button(app.item().item_read_pending ? "捕获中…" : "读取属性")) on_read(app);
  if (app.item().item_read_pending) ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("从模板读取")) on_load_tpl(app);
  ImGui::SameLine();
  if (ImGui::Button("保存属性")) on_save(app);
  ImGui::SameLine();
  if (ImGui::Button("另存为")) on_save_as(app);
  ImGui::SameLine();
  if (ImGui::Button("添加")) {
    app.item().attrs.push_back(ItemProperty());
    app.item().attr_sel.push_back(false);
  }
  ImGui::SameLine();
  if (ImGui::Button("删除")) {
    for (int r = static_cast<int>(app.item().attrs.size()) - 1; r >= 0; --r) {
      if (r < static_cast<int>(app.item().attr_sel.size()) &&
          app.item().attr_sel[static_cast<size_t>(r)]) {
        app.item().attrs.erase(app.item().attrs.begin() + r);
        app.item().attr_sel.erase(app.item().attr_sel.begin() + r);
      }
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("清空")) {
    app.item().attrs.clear();
    app.item().attr_sel.clear();
  }
  ImGui::SameLine();
  if (ImGui::Button("重置节点池")) {
    if (!app.modder().mem().is_open()) {
      app.set_status("请先点击\"初始化\"。");
    } else if (app.modder().reset_pool()) {
      app.set_status("节点池已清零,全部槽位恢复可用。");
    } else {
      app.set_status("重置节点池失败(池不存在或清零失败)。");
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "把新增词条用的节点池整体清零回收。\n"
        "在游戏里丢弃过带新词条的武器、或读档之后,\n"
        "旧链表已作废但池槽不清零会一直占着,点此全部回收。\n"
        "注意:若还有存活武器的词条挂在池内,清零会废掉那些词条。");
  }
  ImGui::SameLine();
  if (ImGui::Button("复制上一件物品")) on_copy_prev_item(app);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "把上次读取/保存的那件物品的词条,复制到当前物品上。\n"
        "不复制基础属性(名字、耐久等各物品独立)。\n"
        "用法:读取并改好一把武器后,捡起新武器,直接点此按钮。");
  }
  ImGui::PopStyleVar();
  ImGui::SameLine();
  ImGui::Text("属性数：%d", static_cast<int>(app.item().attrs.size()));

  ImGui::Separator();

  static const std::vector<std::string>& name_items = ItemData::attr_display_list_ref();
  static std::vector<const char*> name_items_c;
  if (name_items_c.empty())
    for (const auto& s : name_items) name_items_c.push_back(s.c_str());
  std::vector<const char*> percent_items_c;
  for (const auto& kv : ItemData::percent_names()) percent_items_c.push_back(kv.second.c_str());
  // 属性颜色下拉:颜色字段 + node_flag@0x10(隐藏/激活) 合成 6 项。
  // 颜色:0白 1蓝 2浅蓝 3金 4不显示;浅蓝按 active 拆「激活/未激活」两项。
  static const char* kColorStateItems[] = {
      "白色", "蓝色(K)", "浅蓝色激活(C)", "浅蓝色未激活(H)", "金色(Z)", "不显示",
  };
  auto color_to_idx = [](uint32_t c, uint32_t act) -> int {
    switch (c) {
      case 0:
        return 0;
      case 1:
        return 1;
      case 2:
        return act ? 2 : 3;
      case 3:
        return 4;
      default:
        return 5;  // 4 及未知 -> 不显示
    }
  };
  auto color_from_idx = [](int idx, uint32_t& c, uint32_t& act) {
    switch (idx) {
      case 0:
        c = 0;
        act = 1;
        break;
      case 1:
        c = 1;
        act = 1;
        break;
      case 2:
        c = 2;
        act = 1;
        break;  // 浅蓝激活
      case 3:
        c = 2;
        act = 0;
        break;  // 浅蓝未激活(隐藏)
      case 4:
        c = 3;
        act = 1;
        break;
      default:
        c = 4;
        act = 1;
        break;
    }
  };

  ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_NoHostExtendY;
  if (ImGui::BeginTable("attrs", 5, flags, ImVec2(0.0f, height))) {
    ImGui::TableSetupColumn("选择", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("属性名", ImGuiTableColumnFlags_WidthFixed, 220.0f);
    ImGui::TableSetupColumn("属性颜色", ImGuiTableColumnFlags_WidthFixed, 178.0f);
    ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("属性值", ImGuiTableColumnFlags_WidthStretch, 0.0f);
    ImGui::TableHeadersRow();

    for (int r = 0; r < static_cast<int>(app.item().attrs.size()); ++r) {
      ItemProperty& p = app.item().attrs[static_cast<size_t>(r)];
      ImGui::PushID(r);
      ImGui::TableNextRow();
      if (ImGui::TableSetColumnIndex(0)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool sel = app.item().attr_sel[static_cast<size_t>(r)];
        if (ImGui::Checkbox("##sel", &sel)) app.item().attr_sel[static_cast<size_t>(r)] = sel;
      }
      if (ImGui::TableSetColumnIndex(1)) {
        int cur = index_of(app.item().attr_ids, p.attr_id);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##name", &cur, name_items_c.data(),
                         static_cast<int>(name_items_c.size()))) {
          if (cur >= 0) p.attr_id = app.item().attr_ids[static_cast<size_t>(cur)];
        }
      }
      if (ImGui::TableSetColumnIndex(2)) {
        int cur = color_to_idx(p.color, p.active);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##color", &cur, kColorStateItems, 6)) {
          color_from_idx(cur, p.color, p.active);
        }
      }
      if (ImGui::TableSetColumnIndex(3)) {
        int cur = index_of(app.item().percent_ids, p.precent);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##prec", &cur, percent_items_c.data(),
                         static_cast<int>(percent_items_c.size()))) {
          if (cur >= 0) p.precent = app.item().percent_ids[static_cast<size_t>(cur)];
        }
      }
      if (ImGui::TableSetColumnIndex(4)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputFloat("##val", &p.value, 0.0f, 0.0f, "%.6g");
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
}
}  // namespace poq
