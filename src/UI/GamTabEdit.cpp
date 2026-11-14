// GAM tab —— 右侧物品/词条编辑 UI。

#include "UI/UiTabCommon.h"

namespace poq {
static void gam_load_g_info_rows(App& app, poq::GamItem& it) {
  app.gam().gam_grows.clear();
  it.parse_g_info_rows(app.gam().gam_grows);
  app.gam().gam_grow_sel.assign(app.gam().gam_grows.size(), 0);  // 勾选状态随行数重置
  app.gam().gam_gheader = it.get_g_info_header();
  // 记下"装载时的装配结果"。游戏写的文本有些行尾没空格, 而 build_g_info_text 一律补一个,
  // 所以直接比对原文会误判成"改过"; 用装载时装配的结果做基准, 才能区分出真正的用户编辑。
  app.gam().gam_gtext_loaded.clear();
  poq::GamItem::build_g_info_text(app.gam().gam_gheader, app.gam().gam_grows,
                                  app.gam().gam_gtext_loaded);
}

void render_gam_edit(App& app) {
  if (gam_sel_mode(app) != 1) {
    ImGui::TextDisabled("左侧选中地图后，点选一件物品进入编辑。");
    return;
  }
  poq::GamItem& it = app.gam().gam_file.items[static_cast<size_t>(app.gam().gam_sel_item)];
  // 非装备记录(技能等): 词条 id 不在装备属性表里, 编辑会破坏它 -> 直接不给改
  if (gam_is_non_equipment(it)) {
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                       "这是非装备记录（技能等），属性编辑已禁用。");
    ImGui::TextWrapped(
        "它的词条用的是另一套属性 id（0x10x/0x11x，如 驯兽最大数量 / "
        "召唤伙伴个数），不在装备属性表内。"
        "编辑器无法安全改写它的词条数组，所以这里只读；需要删除的话在左侧树上用\"删除\"。");
    if (app.gam().gam_loaded) {
      ImGui::Separator();
      ImGui::Text("记录: %s  长度 %d  词条节点 %d", it.alias.c_str(),
                  static_cast<int>(it.content.size()), static_cast<int>(it.affixes.size()));
      for (const auto& a : it.affixes)
        ImGui::BulletText("id 0x%03X  值 %g  颜色 %u  激活 %u", static_cast<unsigned>(a.id),
                          a.value, a.color, a.active);
    }
    return;
  }
  // 选中物品变化: 先提交上一件的词条编辑, 再载入新物品
  if (app.gam().gam_edit_loaded_item != app.gam().gam_sel_item) {
    if (app.gam().gam_edit_loaded_item >= 0 &&
        app.gam().gam_edit_loaded_item < static_cast<int>(app.gam().gam_file.items.size()))
      gam_commit_g_info(
          app, app.gam().gam_file.items[static_cast<size_t>(app.gam().gam_edit_loaded_item)]);
    gam_load_g_info_rows(app, it);
    std::snprintf(app.gam().gam_alias_buf, sizeof(app.gam().gam_alias_buf), "%s", it.alias.c_str());
    std::snprintf(app.gam().gam_name_buf, sizeof(app.gam().gam_name_buf), "%s",
                  poq::gbk_to_utf8(it.name).c_str());
    app.gam().gam_edit_loaded_item = app.gam().gam_sel_item;
  }
  // 名称(标签放输入框左边)
  ImGui::Text("记录长度 %u 字节%s", it.len, it.is_new ? "   （新增，保存后写入所选地图）" : "");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("道具名字");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(240.0f);
  if (ImGui::InputText("##gamname", app.gam().gam_name_buf, sizeof(app.gam().gam_name_buf))) {
    if (!it.set_name(poq::utf8_to_gbk(app.gam().gam_name_buf)))  // 非法则还原
      std::snprintf(app.gam().gam_name_buf, sizeof(app.gam().gam_name_buf), "%s",
                    poq::gbk_to_utf8(it.name).c_str());
    else
      app.gam().gam_filter_dirty = true;
  }
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("道具别名");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(220.0f);
  if (ImGui::InputText("##gamalias", app.gam().gam_alias_buf, sizeof(app.gam().gam_alias_buf),
                       ImGuiInputTextFlags_CharsNoBlank)) {
    if (!it.set_alias(app.gam().gam_alias_buf))  // 非法则还原
      std::snprintf(app.gam().gam_alias_buf, sizeof(app.gam().gam_alias_buf), "%s",
                    it.alias.c_str());
    else
      app.gam().gam_filter_dirty = true;
  }
  ImGui::Separator();
  // 基础字段网格(3 列)
  const float avail = ImGui::GetContentRegionAvail().x;
  const int cols = avail >= 800.0f ? 4 : (avail >= 500.0f ? 3 : 2);
  if (ImGui::BeginTable("gambasic", cols, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    const auto& bfs = app.cfg().cfg.current().basic_fields;
    auto findCfg = [&](const char* label) -> const poq::BasicFieldCfg* {
      for (const auto& bf : bfs)
        if (bf.label == label) return &bf;
      return nullptr;
    };
    for (const auto& f : poq::GamFile::field_list()) {
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(f.label);
      bool ok = false;
      int32_t v = it.get_field(f.name, &ok);
      int ed = ok ? static_cast<int>(v) : -1;
      const char* lbl = gam_field_to_item_label(f.name);
      const poq::BasicFieldCfg* bf = lbl ? findCfg(lbl) : nullptr;
      std::string id = "##gam" + std::string(f.name);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (bf && !bf->enum_map.empty()) {
        const size_t bf_idx = static_cast<size_t>(bf - bfs.data());
        auto& options = app.cfg().gam_enum_options[bf_idx];
        int cur = options.index_of(ed);
        if (ImGui::Combo(id.c_str(), &cur, EnumComboOptions::label_at, &options,
                         static_cast<int>(options.labels.size()))) {
          if (cur >= 0) it.set_field(f.name, options.values[static_cast<size_t>(cur)]);
        }
      } else if (ImGui::InputInt(id.c_str(), &ed)) {
        if (bf) ed = bf->clamp(ed);  // 合法校验与"道具修改"一致
        it.set_field(f.name, ed);
      }
    }
    ImGui::EndTable();
  }
  ImGui::Separator();
  // 词条: 一行一个属性(隐藏模板头), 保存时装配回 G_Info
  ImGui::TextUnformatted("词条（一行一个属性，保存时装配）：");
  const char* color_items[] = {"白色", "蓝色(K)", "亮蓝色(C,激活隐藏)", "金色(Z)",
                               "暗蓝色(H,未激活隐藏)"};
  const char color_letters[] = {0, 'K', 'C', 'Z', 'H'};
  if (ImGui::BeginTable("grows", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    // 列顺序与"道具修改"一致: 选择 / 属性名 / 属性颜色 / 类型 / 属性值
    ImGui::TableSetupColumn("选择", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("属性名", ImGuiTableColumnFlags_WidthFixed, 160.0f);
    ImGui::TableSetupColumn("属性颜色", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("属性值", ImGuiTableColumnFlags_WidthStretch, 0.0f);
    ImGui::TableHeadersRow();
    // 属性名下拉(显示同"道具修改"如 "29：普通攻击"); 用纯名别名映射匹配 G_Info 名
    const auto& s_disp = ItemData::attr_display_list_ref();
    static const std::vector<std::string> s_bare = [&] {
      std::vector<std::string> bare;
      bare.reserve(s_disp.size());
      for (const auto& d : s_disp) bare.push_back(attr_bare_name_u8(d));
      return bare;
    }();
    // 与原来的从头线性查找相同:重名时保留第一个下标。
    static const std::unordered_map<std::string, int> s_ginfo_index = [&] {
      std::unordered_map<std::string, int> index;
      index.reserve(s_bare.size());
      for (size_t k = 0; k < s_bare.size(); ++k)
        index.emplace(attr_g_info_alias_u8(s_bare[k]), static_cast<int>(k));
      return index;
    }();
    static const std::vector<const char*> s_disp_c = [&] {
      std::vector<const char*> ptrs;
      ptrs.reserve(s_disp.size());
      for (const auto& d : s_disp) ptrs.push_back(d.c_str());
      return ptrs;
    }();
    // 存档中的 GAM 专有名字仍在下拉框原名置顶, 保持旧交互。
    static const std::vector<std::string>& pct_list = ItemData::percent_display_list_ref();
    static std::vector<const char*> pct_c;
    if (pct_c.empty())
      for (auto& t : pct_list) pct_c.push_back(t.c_str());
    for (int i = 0; i < static_cast<int>(app.gam().gam_grows.size()); ++i) {
      auto& r = app.gam().gam_grows[static_cast<size_t>(i)];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      if (ImGui::TableSetColumnIndex(0)) {
        bool bsel = (i < static_cast<int>(app.gam().gam_grow_sel.size())) &&
                    app.gam().gam_grow_sel[static_cast<size_t>(i)];
        if (ImGui::Checkbox("##sel", &bsel) && i < static_cast<int>(app.gam().gam_grow_sel.size()))
          app.gam().gam_grow_sel[static_cast<size_t>(i)] = bsel ? 1 : 0;
      }
      if (ImGui::TableSetColumnIndex(1)) {
        std::string name_u = poq::gbk_to_utf8(r.name);
        int cur = -1;
        auto found = s_ginfo_index.find(name_u);
        if (found != s_ginfo_index.end()) cur = found->second;
        std::vector<const char*> nc;
        std::string custom;
        int base = 0;
        if (cur < 0) {  // 未匹配: 原名置顶, 不显示为空
          custom = name_u.empty() ? "（空）" : name_u;
          nc.reserve(s_disp_c.size() + 1);
          nc.push_back(custom.c_str());
          nc.insert(nc.end(), s_disp_c.begin(), s_disp_c.end());
          base = 1;
          cur = 0;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto& choices = base ? nc : s_disp_c;
        if (ImGui::Combo("##nm", &cur, choices.data(), static_cast<int>(choices.size()))) {
          int si = cur - base;
          if (si >= 0 && si < static_cast<int>(s_bare.size()))
            // 写 G_Info 名字(如 普通攻击力), 不是道具修改的名字(普通攻击)
            r.name = poq::utf8_to_gbk(attr_g_info_alias_u8(s_bare[static_cast<size_t>(si)]));
        }
      }
      if (ImGui::TableSetColumnIndex(2)) {
        int cc = 0;
        for (int x = 0; x < 5; ++x)
          if (color_letters[x] == r.color) {
            cc = x;
            break;
          }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##col", &cc, color_items, 5)) r.color = color_letters[cc];
      }
      if (ImGui::TableSetColumnIndex(3)) {
        int pc = r.is_percent ? 1 : 0;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##pct", &pc, pct_c.data(), static_cast<int>(pct_c.size())))
          r.is_percent = (pc == 1);
      }
      if (ImGui::TableSetColumnIndex(4)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        int v = static_cast<int>(r.vmin);
        if (ImGui::InputInt("##v", &v)) r.vmin = v;
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (ImGui::Button("添加属性")) {
    poq::GamItem::GInfoRow r{};
    r.color = 0;
    const auto& lst = ItemData::attr_display_list_ref();  // 默认取第一个属性名
    r.name = lst.empty() ? std::string()
                         : poq::utf8_to_gbk(attr_g_info_alias_u8(attr_bare_name_u8(lst[0])));
    r.vmin = 0;
    r.has_max = false;
    r.is_percent = false;
    app.gam().gam_grows.push_back(r);
    app.gam().gam_grow_sel.push_back(0);
  }
  ImGui::SameLine();
  if (ImGui::Button("删除选中")) {  // 删除勾选的行
    for (int i = static_cast<int>(app.gam().gam_grows.size()) - 1; i >= 0; --i) {
      if (i >= static_cast<int>(app.gam().gam_grow_sel.size()) ||
          !app.gam().gam_grow_sel[static_cast<size_t>(i)])
        continue;
      app.gam().gam_grows.erase(app.gam().gam_grows.begin() + i);
      app.gam().gam_grow_sel.erase(app.gam().gam_grow_sel.begin() + i);
    }
  }
  ImGui::NewLine();  // 提示另起一行
  ImGui::TextDisabled(
      "改完后点\"保存文件\"即装配写回；模板头已隐藏、保存时自动补回。数值/"
      "上限可任意改（变长会自动重排）。");
}

}  // namespace poq
