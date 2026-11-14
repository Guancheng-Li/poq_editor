// GAM 存档 tab —— 打开/保存/剪贴板/无中生有(计划 P3 自 TabGam.cpp 拆出)。

#include "UI/GamTabLogic.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "Core/GamFile.h"
#include "Core/GameModder.h"
#include "Core/ItemData.h"
#include "Core/JsonTemplate.h"
#include "Core/TextCodec.h"
#include "Native/ProcessMemory.h"
#include "UI/App.h"
#include "UI/AppGamState.h"

namespace poq {

static void apply_gam_paste_attr(App& app, int idx, bool withIdent);

static std::wstring gam_prefs_path() {
  return app_dir_w() + L"\\poqeditor_gam.ini";
}

// 道具修改属性名(UTF-8) -> 该属性在 G_Info 里的词条名(UTF-8)。差异:
// "普通攻击"↔"普通攻击力"; "X属性攻击"↔"X系攻击力"; "普通防御"↔"普通防御力";
// "X属性防御"↔"X系防御力"。
std::string attr_g_info_alias_u8(const std::string& n) {
  return poq::ItemData::attr_g_info_name(n);  // 唯一映射表在 ItemData(GAM 写盘也用它)
}

// "29：普通攻击"(attr_display 带 id 前缀+全角冒号) -> 纯名 "普通攻击"
std::string attr_bare_name_u8(const std::string& disp) {
  size_t p = disp.find("：");  // 全角冒号(UTF-8 3B)
  if (p == std::string::npos) return disp;
  return disp.substr(p + 3);
}

// GAM 字段名 -> "道具修改"字段标签(借其类型/枚举/范围做下拉与合法校验)
const char* gam_field_to_item_label(const std::string& name) {
  static const std::map<std::string, const char*> m = {
      {"lvl", "等级"},         {"dur", "耐久度"},       {"durmax", "耐久度上限"},
      {"price", "基准价格"},   {"wgt", "重量"},         {"type", "类型"},
      {"ident", "是否鉴定"},   {"sockets", "孔数"},     {"req_lvl", "等级需求"},
      {"req_str", "力量需求"}, {"req_agi", "敏捷需求"}, {"req_con", "体质需求"},
      {"wx", "五行"},          {"arrows", "弓箭数量"},
  };
  auto it = m.find(name);
  return it == m.end() ? nullptr : it->second;
}

// 物品显示名: 中文名(别名), 没有中文映射时仅别名。
std::string gam_item_disp_name(const App& app, const std::string& alias) {
  if (app.gam().gam_item_names_loaded) {
    std::string cn = app.gam().gam_item_names.cn(alias);
    if (!cn.empty()) return cn + "(" + alias + ")";
  }
  return alias;
}

// 生成物品属性提示(悬停显示): 解析记录 -> 固定字段 + 词条文本。
std::string gam_format_item_tip(const App& app, const std::vector<uint8_t>& rec) {
  poq::GamItem it;
  if (!poq::GamItem::parse_record(rec, it)) return "";
  std::string out = gam_item_disp_name(app, it.alias);
  bool ok = false;
  int32_t lvl = it.get_field("lvl", &ok);
  if (ok) out += "  Lv" + std::to_string(static_cast<int>(lvl));
  out += "\n";
  // 关键字段: 价格/重量/类型/鉴定/孔数/五行/需求/攻防
  auto addf = [&](const char* label, const char* name) {
    bool o = false;
    int32_t v = it.get_field(name, &o);
    if (o && v != -1) out += std::string(label) + std::to_string(static_cast<int>(v)) + " ";
  };
  addf("价格", "price");
  addf("重量", "wgt");
  addf("类型", "type");
  addf("鉴定", "ident");
  addf("孔数", "sockets");
  addf("五行", "wx");
  addf("需等级", "req_lvl");
  addf("需力量", "req_str");
  addf("需敏捷", "req_agi");
  addf("需体质", "req_con");
  addf("攻击", "f_118");
  addf("防御", "f_11c");
  out += "\n词条:\n" + poq::gbk_to_utf8(it.get_g_info());
  return out;
}

static void gam_save_prefs(const std::string& last_dir) {
  FILE* fp = _wfopen(gam_prefs_path().c_str(), L"wb");
  if (!fp) return;
  fwrite(last_dir.data(), 1, last_dir.size(), fp);
  fclose(fp);
}

static std::wstring gam_initial_dir(App& app) {
  // 游戏运行时优先定位到其存档目录 System\game
  std::optional<unsigned int> pid;
  if (app.modder().mem().is_open())
    pid = app.modder().pid();
  else if (auto s = try_get_selected_pid(app))
    pid = s;
  if (!pid) pid = find_poq_pid(app);
  if (pid) {
    auto path = poq::ProcessMemory::find_module_path(*pid, "POQ.exe");
    if (path) {
      std::string p = *path;
      size_t pos = p.find_last_of("/\\");
      if (pos != std::string::npos) {
        std::string exedir = p.substr(0, pos);
        std::wstring sdir = utf8_to_wide(exedir + "\\System\\game");
        if (::GetFileAttributesW(sdir.c_str()) != INVALID_FILE_ATTRIBUTES) return sdir;
        std::wstring dir = utf8_to_wide(exedir);
        if (!dir.empty() && ::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES)
          return dir;
      }
    }
  }
  // 游戏未运行:记忆上次打开目录
  if (!app.gam().gam_last_dir.empty()) {
    std::wstring w = utf8_to_wide(app.gam().gam_last_dir);
    if (::GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES) return w;
  }
  return app_dir_w();
}

void gam_on_open(App& app) {
  std::string sel =
      open_file_dialog(gam_initial_dir(app), L"GAM 存档 (*.gam)\0*.gam\0所有文件 (*)\0*.*\0\0");
  if (sel.empty()) return;
  poq::GamFile gf;
  std::string err;
  if (!poq::GamFile::load(sel, gf, err)) {
    msg_box("打开失败", err, MB_ICONERROR);
    return;
  }
  // 打开时自动备份原文件: <base>.<yyyymmddhhMMss>.bak.gam
  {
    std::wstring w = utf8_to_wide(sel);
    std::wstring bak = timestamped_bak_path(w);
    if (::CopyFileW(w.c_str(), bak.c_str(), FALSE)) track_backup(w, bak);
  }
  app.gam().gam_file = std::move(gf);
  app.gam().gam_regions_items = -1;
  app.gam().gam_filter_dirty = true;
  app.gam().gam_live_counts_dirty = true;
  app.gam().gam_path = sel;
  app.gam().gam_loaded = true;
  app.gam().gam_sel_container = -1;
  app.gam().gam_sel_item = -1;
  app.gam().gam_edit_loaded_item = -1;
  app.gam().gam_ginfo_pending = false;
  // 记忆上次打开目录(下次打开gam默认进该目录, 持久化到 exe 同级 ini)
  {
    size_t pos = sel.find_last_of("/\\");
    if (pos != std::string::npos) app.gam().gam_last_dir = sel.substr(0, pos);
  }
  gam_save_prefs(app.gam().gam_last_dir);
  app.set_status("GAM 已打开: " + app.gam().gam_path + "（容器 " +
                 std::to_string(app.gam().gam_file.containers.size()) + "，物品 " +
                 std::to_string(app.gam().gam_file.items.size()) + "）");
}

void gam_on_save(App& app) {
  if (!app.gam().gam_loaded) {
    msg_box("提示", "请先打开 gam 存档。", MB_ICONINFORMATION);
    return;
  }
  // 提交当前物品的词条编辑(装配回 G_Info)
  if (gam_sel_mode(app) == 1)
    gam_commit_g_info(app, app.gam().gam_file.items[static_cast<size_t>(app.gam().gam_sel_item)]);
  // 组装待插入记录(所有 is_new 物品 -> 按其当前内容重编)
  std::vector<std::pair<int, std::vector<uint8_t>>> added;
  for (size_t i = 0; i < app.gam().gam_file.items.size(); ++i) {
    const auto& it = app.gam().gam_file.items[i];
    if (!it.is_new) continue;
    int ci = -1;
    for (size_t c = 0; c < app.gam().gam_file.containers.size(); ++c)
      for (int idx : app.gam().gam_file.containers[c].item_idx)
        if (idx == static_cast<int>(i)) {
          ci = static_cast<int>(c);
          break;
        }
    if (ci >= 0) {
      std::vector<uint8_t> rec;
      if (poq::GamItem::build_record(it, rec)) added.push_back({ci, std::move(rec)});
    }
  }
  std::string err;
  if (!poq::GamFile::save(app.gam().gam_file, app.gam().gam_path, added, err)) {
    msg_box("保存失败", err, MB_ICONERROR);
    return;
  }
  app.gam().gam_filter_dirty = true;
  app.gam().gam_live_counts_dirty = true;
  app.set_status("GAM 已保存：" + app.gam().gam_path + "（新增 " + std::to_string(added.size()) +
                 " 件）");
  // 重载同步内存(位置/偏移已整体后移)
  poq::GamFile gf;
  std::string e;
  if (poq::GamFile::load(app.gam().gam_path, gf, e)) {
    int keep_c = app.gam().gam_sel_container, keep_i = app.gam().gam_sel_item;
    app.gam().gam_file = std::move(gf);
    app.gam().gam_regions_items = -1;
    app.gam().gam_filter_dirty = true;
    app.gam().gam_live_counts_dirty = true;
    app.gam().gam_sel_container = keep_c;
    app.gam().gam_sel_item = keep_i;
    app.gam().gam_edit_loaded_item = -1;
  }
}

// 非装备记录(技能等): 词条数组里含"装备属性表(0x00..0x62)之外"的 id(技能属性在 0x10x/0x11x)。
// 这类记录的词条名/id 不在下拉表里, 编辑会把它们从词条数组里丢掉 -> 禁用属性编辑。
bool gam_is_non_equipment(const poq::GamItem& it) {
  if (!it.has_affixes) return false;
  for (const auto& a : it.affixes)
    if (poq::ItemData::attr_names().find(static_cast<uint32_t>(a.id)) ==
        poq::ItemData::attr_names().end())
      return true;
  return false;
}

int gam_sel_mode(const App& app) {
  if (app.gam().gam_sel_container >= 0 && app.gam().gam_sel_item >= 0 &&
      app.gam().gam_sel_item < static_cast<int>(app.gam().gam_file.items.size()))
    return 1;  // 选中物品
  if (app.gam().gam_sel_container >= 0 &&
      app.gam().gam_sel_container < static_cast<int>(app.gam().gam_file.containers.size()))
    return 2;  // 仅选中地图
  return 0;
}

// 复制指定物品的整条记录到剪贴板。
void gam_copy_item_at(App& app, int idx) {
  if (idx < 0 || idx >= static_cast<int>(app.gam().gam_file.items.size())) return;
  const poq::GamItem& it = app.gam().gam_file.items[static_cast<size_t>(idx)];
  if (it.is_new) {
    poq::GamItem::build_record(it, app.gam().gam_clip_record);
  } else if (it.rec_end <= app.gam().gam_file.buf.size()) {
    app.gam().gam_clip_record.assign(
        app.gam().gam_file.buf.begin() + static_cast<ptrdiff_t>(it.pos),
        app.gam().gam_file.buf.begin() + static_cast<ptrdiff_t>(it.rec_end));
  }
  app.gam().gam_has_record = !app.gam().gam_clip_record.empty();
  app.set_status(app.gam().gam_has_record ? "已复制物品：" + it.alias : "复制物品失败");
}

void gam_on_copy_item(App& app) {
  if (gam_sel_mode(app) != 1) {
    msg_box("提示", "请先在左侧选中一件物品，再点\"复制物品\"。", MB_ICONINFORMATION);
    return;
  }
  gam_copy_item_at(app, app.gam().gam_sel_item);
}

// 把新建物品放到目标容器已有物品右侧(错开坐标, 避免叠位/不可见; 无已有物品则保留源坐标)
static void gam_place_new_item(App& app, int item_idx) {
  if (item_idx < 0 || item_idx >= static_cast<int>(app.gam().gam_file.items.size())) return;
  if (app.gam().gam_sel_container < 0 ||
      app.gam().gam_sel_container >= static_cast<int>(app.gam().gam_file.containers.size()))
    return;
  auto& cont = app.gam().gam_file.containers[static_cast<size_t>(app.gam().gam_sel_container)];
  long maxx = -0x7FFFFFFF;
  for (int idx : cont.item_idx) {
    if (idx < 0 || idx >= static_cast<int>(app.gam().gam_file.items.size())) continue;
    if (idx == item_idx) continue;
    bool ok = false;
    int32_t x = app.gam().gam_file.items[static_cast<size_t>(idx)].get_field("f_c8", &ok);
    if (ok && x >= 0 && x < 0x1000000) maxx = std::max<long>(maxx, static_cast<long>(x));
  }
  int newx = (maxx > -0x7FFFFFFF) ? static_cast<int>(maxx + 40) : 100;
  app.gam().gam_file.items[static_cast<size_t>(item_idx)].set_field("f_c8", newx);
}

// 把剪贴板记录作为新物品粘贴到容器 ci。
void gam_paste_into(App& app, int ci) {
  if (!app.gam().gam_has_record || app.gam().gam_clip_record.empty()) {
    msg_box("提示", "剪贴板中没有物品记录，请先\"复制物品\"或从目录\"创建物品\"。",
            MB_ICONINFORMATION);
    return;
  }
  if (ci < 0 || ci >= static_cast<int>(app.gam().gam_file.containers.size())) return;
  poq::GamItem it;
  if (!poq::GamItem::parse_record(app.gam().gam_clip_record, it)) {
    msg_box("粘贴失败", "剪贴板记录不是合法物品帧。", MB_ICONERROR);
    return;
  }
  it.is_new = true;
  auto& items = app.gam().gam_file.items;
  int idx = static_cast<int>(items.size());
  items.push_back(std::move(it));
  app.gam().gam_file.containers[static_cast<size_t>(ci)].item_idx.push_back(idx);
  app.gam().gam_sel_container = ci;
  app.gam().gam_sel_item = idx;
  app.gam().gam_edit_loaded_item = -1;
  gam_place_new_item(app, idx);  // 错开坐标避免叠位/不可见
  app.set_status("已粘贴物品：" + items[static_cast<size_t>(idx)].alias);
}

void gam_on_paste_item(App& app) {
  if (gam_sel_mode(app) < 2) {
    msg_box("提示", "请先在左侧选中一个地图(容器)。", MB_ICONINFORMATION);
    return;
  }
  gam_paste_into(app, app.gam().gam_sel_container);
}

void gam_on_copy_attr(App& app) {
  if (gam_sel_mode(app) != 1) {
    msg_box("提示", "请先选中一件物品(在右侧编辑界面)。", MB_ICONINFORMATION);
    return;
  }
  const poq::GamItem& it = app.gam().gam_file.items[static_cast<size_t>(app.gam().gam_sel_item)];
  if (gam_is_non_equipment(it)) {
    msg_box("提示", "这是非装备记录（技能等），不支持复制属性。", MB_ICONINFORMATION);
    return;
  }
  app.gam().gam_clip_fields.clear();
  for (const auto& f : poq::GamFile::field_list()) {
    bool ok = false;
    int32_t v = it.get_field(f.name, &ok);
    if (ok) app.gam().gam_clip_fields[f.name] = v;
  }
  app.gam().gam_clip_ginfo = it.get_g_info();
  app.gam().gam_clip_name = poq::gbk_to_utf8(it.name);  // 名字(UTF-8, 供询问框显示)
  app.gam().gam_clip_alias = it.alias;
  app.gam().gam_has_attrs = true;
  app.set_status("已复制属性：" + it.alias);
}

void gam_on_paste_attr(App& app) {
  if (gam_sel_mode(app) != 1) {
    msg_box("提示", "请先选中一件物品(在右侧编辑界面)。", MB_ICONINFORMATION);
    return;
  }
  if (!app.gam().gam_has_attrs) {
    msg_box("提示", "剪贴板中没有属性，请先\"复制属性\"。", MB_ICONINFORMATION);
    return;
  }
  poq::GamItem& it = app.gam().gam_file.items[static_cast<size_t>(app.gam().gam_sel_item)];
  if (gam_is_non_equipment(it)) {
    msg_box("提示", "这是非装备记录（技能等），属性编辑已禁用，不能粘贴属性。", MB_ICONINFORMATION);
    return;
  }
  // 先问"名字/别名/类型"要不要一起粘贴, 再动手(取消 = 什么都不做)
  int idx = app.gam().gam_sel_item;
  std::string q = "来源物品:";
  if (!app.gam().gam_clip_name.empty()) q += "\n  名字: " + app.gam().gam_clip_name;
  if (!app.gam().gam_clip_alias.empty()) q += "\n  别名: " + app.gam().gam_clip_alias;
  {
    auto ft = app.gam().gam_clip_fields.find("type");
    int tv = ft != app.gam().gam_clip_fields.end() ? static_cast<int>(ft->second) : -1;
    q += "\n  类型: " + field_value_display(app, "类型", tv);  // 枚举显示"值：名字"
  }
  ask_include_ident(app, "粘贴属性", q,
                    "这三项(名字/别名/类型)要不要一起粘贴？其余属性按所选方式粘贴。",
                    "已取消粘贴属性(什么都没改)。",
                    [&app, idx](bool with_ident) { apply_gam_paste_attr(app, idx, with_ident); });
}

// 真正做粘贴: withIdent=true 连名字/别名/类型一起; false 只粘贴其余属性。
static void apply_gam_paste_attr(App& app, int idx, bool withIdent) {
  if (idx < 0 || idx >= static_cast<int>(app.gam().gam_file.items.size())) return;
  poq::GamItem& it = app.gam().gam_file.items[static_cast<size_t>(idx)];
  bool ginfo_ok = true;
  for (const auto& kv : app.gam().gam_clip_fields) {
    if (!withIdent && kv.first == "type") continue;  // 类型归入"名字/别名"一档
    it.set_field(kv.first.c_str(), kv.second);
  }
  if (!app.gam().gam_clip_ginfo.empty()) ginfo_ok = it.set_g_info(app.gam().gam_clip_ginfo);
  if (withIdent) {
    if (!app.gam().gam_clip_name.empty()) it.set_name(poq::utf8_to_gbk(app.gam().gam_clip_name));
    if (!app.gam().gam_clip_alias.empty()) it.set_alias(app.gam().gam_clip_alias);
    app.gam().gam_filter_dirty = true;
  }
  app.gam().gam_edit_loaded_item = -1;  // 刷新界面
  std::string msg = std::string(withIdent ? "已粘贴属性+名字/别名/类型到 "
                                          : "已粘贴属性(名字/别名/类型保持原样)到 ") +
                    it.alias;
  if (!ginfo_ok) msg += "（词条文本无法原位覆盖，已保留原词条）";
  msg += "。记得点\"保存文件\"。";
  app.set_status(msg);
}

void gam_on_create(App& app) {
  if (!app.gam().gam_cat_loaded || app.gam().gam_sel_cat < 0 ||
      app.gam().gam_sel_cat >= static_cast<int>(app.gam().gam_catalog.cats.size())) {
    msg_box("提示", "物品目录未加载（缺少 gam_catalog.bin，请放 exe 目录或 data 子目录）。",
            MB_ICONINFORMATION);
    return;
  }
  const auto& ents = app.gam().gam_catalog.items[static_cast<size_t>(app.gam().gam_sel_cat)];
  if (ents.empty() || app.gam().gam_sel_cat_item < 0 ||
      app.gam().gam_sel_cat_item >= static_cast<int>(ents.size()))
    return;
  if (gam_sel_mode(app) < 2) {
    msg_box("提示", "请先在左侧选中一个地图(容器)。", MB_ICONINFORMATION);
    return;
  }
  const auto& e = ents[static_cast<size_t>(app.gam().gam_sel_cat_item)];
  poq::GamItem it;
  if (!poq::GamItem::parse_record(e.rec, it)) {
    msg_box("创建失败", "目录记录不是合法物品帧。", MB_ICONERROR);
    return;
  }
  it.is_new = true;
  auto& items = app.gam().gam_file.items;
  int idx = static_cast<int>(items.size());
  items.push_back(std::move(it));
  app.gam()
      .gam_file.containers[static_cast<size_t>(app.gam().gam_sel_container)]
      .item_idx.push_back(idx);
  app.gam().gam_sel_item = idx;
  app.gam().gam_edit_loaded_item = -1;
  gam_place_new_item(app, idx);  // 错开坐标避免叠位/不可见
  app.set_status("已创建物品：" + gam_item_disp_name(app, e.name));
}

void gam_commit_g_info(App& app, GamItem& it) {
  std::string txt;
  if (!GamItem::build_g_info_text(app.gam().gam_gheader, app.gam().gam_grows, txt)) return;
  if (txt == app.gam().gam_gtext_loaded) return;
  if (it.set_g_info(txt)) app.gam().gam_gtext_loaded = txt;
}

}  // namespace poq
