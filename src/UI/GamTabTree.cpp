// GAM tab —— 左侧地图/物品树 UI(计划 P3 自 TabGam.cpp 拆出)。

#include "UI/UiTabCommon.h"

namespace poq {
void render_gam_tree(App& app) {
  ImGui::BeginChild("gamtree", ImVec2(0.0f, ImGui::GetContentRegionAvail().y),
                    ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  static char filt[128] = {0};
  ImGui::TextUnformatted("地图(区域) -> 房间/物体 -> 物品：");
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputText("##gamfilt", filt, sizeof(filt));
  ImGui::Separator();
  ImGui::BeginChild("gamtree2");
  std::string lowf = to_lower(filt);

  auto seg = [](const std::string& path, int a, int b) {  // "/" 分割取 [a,b)
    std::vector<std::string> segs;
    size_t i = 0;
    while (true) {
      size_t j = path.find('/', i);
      if (j == std::string::npos) {
        segs.push_back(path.substr(i));
        break;
      }
      segs.push_back(path.substr(i, j - i));
      i = j + 1;
    }
    std::string out;
    for (int k = a; k < b && k < static_cast<int>(segs.size()); ++k) {
      if (!out.empty()) out += "/";
      out += segs[static_cast<size_t>(k)];
    }
    return out;
  };
  // 构建分级结构(P1: 缓存进 App, 物品数变化才重建 —— 世界档 558 容器每帧构建太浪费)
  auto& regions = app.gam().gam_regions;
  if (static_cast<int>(app.gam().gam_file.items.size()) != app.gam().gam_regions_items) {
    app.gam().gam_regions.clear();
    app.gam().gam_regions_items = static_cast<int>(app.gam().gam_file.items.size());
    app.gam().gam_filter_dirty = true;
    app.gam().gam_live_counts_dirty = true;
    std::map<std::string, int> ridx;
    std::string cur_region;
    for (int ci = 0; ci < static_cast<int>(app.gam().gam_file.containers.size()); ++ci) {
      const auto& c = app.gam().gam_file.containers[static_cast<size_t>(ci)];
      if (c.item_idx.empty()) continue;
      std::string path =
          app.gam().gam_scene_loaded ? app.gam().gam_scene_cat.match_path(c.alias) : "";
      std::string region_disp, room_label;
      bool is_scene = false;
      if (!path.empty()) {
        region_disp = seg(path, 0, 2);  // 秦殇/邯郸
        room_label = seg(path, 3, 4);   // 武器铺
        if (room_label.empty()) room_label = c.alias;
        cur_region = region_disp;
        is_scene = true;
      } else {
        room_label = c.alias;  // 物体(未匹配场景)
        region_disp = cur_region.empty() ? "未分类" : cur_region;
      }
      if (ridx.find(region_disp) == ridx.end()) {
        ridx[region_disp] = static_cast<int>(app.gam().gam_regions.size());
        app.gam().gam_regions.push_back(
            AppGamState::GamRegion{region_disp, to_lower(region_disp), {}, {}});
      }
      AppGamState::GamRoom r{room_label, ci, to_lower(room_label)};
      if (is_scene)
        app.gam().gam_regions[static_cast<size_t>(ridx[region_disp])].rooms.push_back(std::move(r));
      else
        app.gam().gam_regions[static_cast<size_t>(ridx[region_disp])].objs.push_back(std::move(r));
    }
  }
  if (app.gam().gam_live_counts_dirty ||
      app.gam().gam_live_counts.size() != app.gam().gam_file.containers.size()) {
    app.gam().gam_live_counts.assign(app.gam().gam_file.containers.size(), 0);
    for (size_t ci = 0; ci < app.gam().gam_file.containers.size(); ++ci)
      for (int idx : app.gam().gam_file.containers[ci].item_idx)
        if (idx >= 0 && static_cast<size_t>(idx) < app.gam().gam_file.items.size() &&
            !app.gam().gam_file.items[static_cast<size_t>(idx)].removed)
          ++app.gam().gam_live_counts[ci];
    app.gam().gam_live_counts_dirty = false;
  }

  // 渲染
  // 筛选结果只在关键词或树内容变化时重算。空筛选直接显示所有区域，
  // 避免世界档数千件物品在每帧重复做字符串匹配。
  if (lowf.empty()) {
    app.gam().gam_filter_cached.clear();
  } else if (app.gam().gam_filter_dirty || app.gam().gam_filter_cached != lowf ||
             app.gam().gam_filter_items.size() != app.gam().gam_file.items.size() ||
             app.gam().gam_filter_regions.size() != regions.size()) {
    app.gam().gam_filter_cached = lowf;
    app.gam().gam_filter_dirty = false;
    app.gam().gam_filter_items.assign(app.gam().gam_file.items.size(), 0);
    for (size_t i = 0; i < app.gam().gam_file.items.size(); ++i) {
      const auto& it = app.gam().gam_file.items[i];
      if (!it.removed && (it.alias_low.find(lowf) != std::string::npos ||
                          it.disp_low.find(lowf) != std::string::npos))
        app.gam().gam_filter_items[i] = 1;
    }
    std::vector<uint8_t> container_hits(app.gam().gam_file.containers.size(), 0);
    for (size_t ci = 0; ci < app.gam().gam_file.containers.size(); ++ci)
      for (int idx : app.gam().gam_file.containers[ci].item_idx)
        if (idx >= 0 && static_cast<size_t>(idx) < app.gam().gam_filter_items.size() &&
            app.gam().gam_filter_items[static_cast<size_t>(idx)]) {
          container_hits[ci] = 1;
          break;
        }
    app.gam().gam_filter_regions.assign(regions.size(), 0);
    for (size_t ri = 0; ri < regions.size(); ++ri) {
      const auto& region = regions[ri];
      bool match = region.disp_low.find(lowf) != std::string::npos;
      auto group_matches = [&](const AppGamState::GamRoom& room) {
        return room.label_low.find(lowf) != std::string::npos ||
               container_hits[static_cast<size_t>(room.cont)];
      };
      if (!match) match = std::any_of(region.rooms.begin(), region.rooms.end(), group_matches);
      if (!match) match = std::any_of(region.objs.begin(), region.objs.end(), group_matches);
      app.gam().gam_filter_regions[ri] = match ? 1 : 0;
    }
  }

  // treenode 联动: 选中物品所在房间折叠时, 物品行根本不渲染 -> 选中态不可见。
  // 选中项变化后, 自动展开它所在的房间一次(用户手动折叠不会被反复顶开)。
  if (app.gam().gam_sel_item != app.gam().gam_tree_seen_sel) {
    app.gam().gam_tree_seen_sel = app.gam().gam_sel_item;
    app.gam().gam_tree_open_cont = -1;
    if (app.gam().gam_sel_item >= 0 &&
        app.gam().gam_sel_item < static_cast<int>(app.gam().gam_file.items.size())) {
      for (const auto& c : app.gam().gam_file.containers) {
        for (int idx : c.item_idx) {
          if (idx == app.gam().gam_sel_item) {
            app.gam().gam_tree_open_cont =
                static_cast<int>(&c - app.gam().gam_file.containers.data());
            break;
          }
        }
        if (app.gam().gam_tree_open_cont >= 0) break;
      }
    }
  }
  auto item_matches = [&](int idx) {
    if (idx < 0 || idx >= static_cast<int>(app.gam().gam_file.items.size())) return false;
    const auto& it = app.gam().gam_file.items[static_cast<size_t>(idx)];
    if (it.removed) return false;
    if (lowf.empty()) return true;
    return app.gam().gam_filter_items[static_cast<size_t>(idx)] != 0;
  };
  auto render_items = [&](int ci) {
    const auto& c = app.gam().gam_file.containers[static_cast<size_t>(ci)];
    for (int idx : c.item_idx) {
      if (idx < 0 || idx >= static_cast<int>(app.gam().gam_file.items.size())) continue;
      auto& it = app.gam().gam_file.items[static_cast<size_t>(idx)];
      if (it.removed) continue;
      if (!item_matches(idx)) continue;
      bool ok = false;
      int32_t lvl = it.get_field("lvl", &ok);
      std::string disp = gam_item_disp_name(app, it.alias);
      if (disp.size() > 220) disp = disp.substr(0, 220);
      char buf[320];
      std::string tag;
      if (it.is_new) tag += " [新增]";
      if (gam_is_non_equipment(it)) tag += " [非装备记录]";
      std::snprintf(buf, sizeof(buf), "(%d) %s  Lv%d%s##i%s%zX", idx, disp.c_str(),
                    ok ? static_cast<int>(lvl) : 0, tag.c_str(), it.is_new ? "n" : "",
                    it.is_new ? static_cast<size_t>(idx) : it.pos);
      ImGui::PushID(idx);
      if (ImGui::SmallButton("删除")) {  // 删除该物品(内存标记,保存时生效)
        it.removed = true;
        app.gam().gam_filter_dirty = true;
        app.gam().gam_live_counts_dirty = true;
        if (app.gam().gam_sel_item == idx) {
          app.gam().gam_sel_item = -1;
          app.gam().gam_edit_loaded_item = -1;
        }
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("复制")) gam_copy_item_at(app, idx);  // 复制该物品记录
      ImGui::SameLine();
      bool sel = (app.gam().gam_sel_item == idx);
      if (ImGui::Selectable(buf, sel)) {
        app.gam().gam_sel_container = ci;
        app.gam().gam_sel_item = idx;
        app.gam().gam_edit_loaded_item = -1;
      }
      ImGui::PopID();
    }
  };
  auto render_group = [&](const AppGamState::GamRoom& room, const char* idpfx) {
    const auto& c = app.gam().gam_file.containers[static_cast<size_t>(room.cont)];
    bool cont_sel = (app.gam().gam_sel_container == room.cont && app.gam().gam_sel_item < 0);
    char lab[320];
    std::snprintf(lab, sizeof(lab), "%s (%d)##%s%X", room.label.c_str(),
                  app.gam().gam_live_counts[static_cast<size_t>(room.cont)], idpfx,
                  static_cast<unsigned>(c.start & 0xFFFFFFFF));
    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (cont_sel) f |= ImGuiTreeNodeFlags_Selected;
    if (room.cont == app.gam().gam_tree_open_cont) {
      ImGui::SetNextItemOpen(true);  // 选中联动: 本帧强制展开一次
      app.gam().gam_tree_open_cont = -1;
    }
    bool open = ImGui::TreeNodeEx(lab, f);
    if (ImGui::IsItemClicked()) {  // 点标签=选中该容器;点箭头=仅展开(OpenOnArrow)
      app.gam().gam_sel_container = room.cont;
      app.gam().gam_sel_item = -1;
    }
    ImGui::SameLine();
    ImGui::PushID(1000000 + room.cont);
    if (ImGui::SmallButton("删除")) {  // 删该容器下所有物品
      for (int idx : c.item_idx)
        if (idx >= 0 && static_cast<size_t>(idx) < app.gam().gam_file.items.size())
          app.gam().gam_file.items[static_cast<size_t>(idx)].removed = true;
      app.gam().gam_filter_dirty = true;
      app.gam().gam_live_counts_dirty = true;
      app.gam().gam_sel_container = room.cont;
      app.gam().gam_sel_item = -1;
      app.gam().gam_edit_loaded_item = -1;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("粘贴")) gam_paste_into(app, room.cont);  // 粘贴剪贴板物品到该容器
    ImGui::PopID();
    if (open) {
      render_items(room.cont);
      ImGui::TreePop();
    }
  };
  for (int ri = 0; ri < static_cast<int>(regions.size()); ++ri) {
    const AppGamState::GamRegion& R = regions[static_cast<size_t>(ri)];
    if (!lowf.empty() && !app.gam().gam_filter_regions[static_cast<size_t>(ri)]) continue;
    std::string rlabel = R.disp + "##r" + std::to_string(ri);
    if (!ImGui::TreeNodeEx(rlabel.c_str(),
                           ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen)) {
      continue;
    }
    for (const auto& room : R.rooms) render_group(room, "room");
    for (const auto& obj : R.objs) render_group(obj, "obj");
    ImGui::TreePop();
  }
  ImGui::EndChild();
  ImGui::EndChild();
}
}  // namespace poq
