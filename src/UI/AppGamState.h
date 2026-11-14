#pragma once
// GAM 存档 tab 专用状态(从 App 拆出): 含 GamFile / 目录 / 树缓存等。
// 仅 GamTab*、App.cpp 等需要访问 GAM 数据的翻译单元 include 本头;
// App.h 只持 unique_ptr<AppGamState>, 改 GamFile.h 不会牵连 char/GM 等 UI 重编。

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "Core/GamCatalog.h"
#include "Core/GamFile.h"

namespace poq {

struct AppGamState {
  poq::GamFile gam_file;
  std::string gam_path;
  bool gam_loaded = false;
  int gam_sel_container = -1;
  int gam_sel_item = -1;
  int gam_tree_open_cont = -1;
  int gam_tree_seen_sel = -2;
  int gam_edit_loaded_item = -1;
  std::vector<uint8_t> gam_clip_record;
  bool gam_has_record = false;
  std::map<std::string, int64_t> gam_clip_fields;
  std::string gam_clip_ginfo;
  std::string gam_clip_name, gam_clip_alias;
  bool gam_has_attrs = false;
  poq::GamCatalog gam_catalog;
  bool gam_cat_loaded = false;
  int gam_sel_cat = 0;
  int gam_sel_cat_item = 0;
  std::vector<std::pair<int, std::vector<uint8_t>>> gam_added;
  std::string gam_last_dir;
  poq::SceneCatalog gam_scene_cat;
  bool gam_scene_loaded = false;

  struct GamRoom {
    std::string label;
    int cont = 0;
    std::string label_low;
  };
  struct GamRegion {
    std::string disp;
    std::string disp_low;
    std::vector<GamRoom> rooms;
    std::vector<GamRoom> objs;
  };
  std::vector<GamRegion> gam_regions;
  int gam_regions_items = -1;
  std::vector<int> gam_live_counts;
  bool gam_live_counts_dirty = true;
  std::string gam_filter_cached;
  std::vector<uint8_t> gam_filter_items;
  std::vector<uint8_t> gam_filter_regions;
  bool gam_filter_dirty = true;
  poq::ItemNames gam_item_names;
  bool gam_item_names_loaded = false;
  bool gam_data_load_tried = false;

  char gam_ginfo_buf[32768] = {0};
  bool gam_ginfo_pending = false;
  std::vector<poq::GamItem::GInfoRow> gam_grows;
  std::vector<uint8_t> gam_grow_sel;
  std::string gam_gheader;
  std::string gam_gtext_loaded;
  char gam_alias_buf[64] = {0};
  char gam_name_buf[128] = {0};
};

}  // namespace poq
