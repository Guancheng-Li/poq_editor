// 道具修改 Tab —— 读/写/模板/复制逻辑(计划 P3 #18 自 TabChar.cpp 拆出)。

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "Core/GameModder.h"
#include "Core/JsonTemplate.h"
#include "Core/OffsetsConfig.h"
#include "UI/App.h"
#include "UI/AppCfgState.h"
#include "UI/AppItemState.h"

namespace poq {

static void apply_copy_prev_item(App& app, bool withIdent);
static void apply_loaded_tpl(App& app, const TemplateLoadResult& res, bool withIdent);
void fill_basic(App& app, const BasicState& st) {
  for (size_t i = 0; i < app.cfg().basic_fields.size(); ++i) {
    const auto& f = app.cfg().basic_fields[i];
    AppItemState::BasicWidget& w = app.item().basic[i];
    if (f.type == "string_ptr") {
      auto it = st.strs.find(f.label);
      if (it != st.strs.end()) std::snprintf(w.strbuf, sizeof(w.strbuf), "%s", it->second.c_str());
    } else {
      auto it = st.ints.find(f.label);
      if (it != st.ints.end()) {
        if (w.is_enum)
          w.enum_val = it->second;
        else
          w.intval = it->second;
      }
    }
  }
}

// 把界面上的 道具名字/道具别名/类型 记进快照(供"复制上一件物品"按需复制)
void snapshot_names(App& app) {
  app.item().last_name.clear();
  app.item().last_alias.clear();
  app.item().last_type = -1;
  for (size_t i = 0; i < app.cfg().basic_fields.size() && i < app.item().basic.size(); ++i) {
    const std::string& lb = app.cfg().basic_fields[i].label;
    if (lb == "道具名字")
      app.item().last_name = app.item().basic[i].strbuf;
    else if (lb == "道具别名")
      app.item().last_alias = app.item().basic[i].strbuf;
    else if (lb == "类型")
      app.item().last_type =
          app.item().basic[i].is_enum ? app.item().basic[i].enum_val : app.item().basic[i].intval;
  }
}

// 字段值的可读显示: 枚举字段(如 类型/五行)显示成"值：名字", 与下拉框一致; 其它直接数字。
std::string field_value_display(const App& app, const char* label, int v) {
  for (const auto& f : app.cfg().cfg.current().basic_fields) {
    if (f.label != label) continue;
    auto it = f.enum_map.find(v);
    if (it != f.enum_map.end())
      return f.enum_display_value ? (std::to_string(v) + "：" + it->second) : it->second;
    break;
  }
  return std::to_string(v);
}

BasicState collect_basic_state(const App& app) {
  BasicState st;
  for (size_t i = 0; i < app.cfg().basic_fields.size(); ++i) {
    const auto& f = app.cfg().basic_fields[i];
    const AppItemState::BasicWidget& w = app.item().basic[i];
    if (f.type == "string_ptr")
      st.strs[f.label] = w.strbuf;
    else if (w.is_enum)
      st.ints[f.label] = w.enum_val;
    else
      st.ints[f.label] = w.intval;
  }
  return st;
}

// 检测游戏窗口内左键按下(上升沿),触发"点击捕获":
// 解锁->清槽->等游戏把被点中的物品读进槽->一到手立即冻结(防被后续悬停/场景顶掉)。
// 只在已注入且支持冻结时生效;点击发生在我们窗口上不算。
// 从物品基址读取基础字段+属性表格,更新快照与状态(「读取属性」的公共主体)。
void read_from_base(App& app, uintptr_t ib) {
  fill_basic(app, app.modder().read_basic_fields(ib));
  app.item().attrs = app.modder().read_attributes(ib);
  app.item().attr_sel.assign(app.item().attrs.size(), false);
  app.item().last_attrs = app.item().attrs;  // 快照:供"复制上一件物品"用
  snapshot_names(app);                       // 名字/别名也快照(复制时询问是否带过来)
  app.item().last_item_base = ib;
  if (!app.hidden) {
    app.set_status("读取完成。物品基址 = 0x" + hex8(ib) + ",属性 " +
                   std::to_string(app.item().attrs.size()) + " 条。");
  } else {
    app.set_status("读取完成,属性 " + std::to_string(app.item().attrs.size()) + " 条。");
  }
}

static void finish_item_read(App& app, uintptr_t ib) {
  try {
    read_from_base(app, ib);
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("读取失败", msg, MB_ICONERROR);
    app.set_status("读取失败:" + msg);
  }
}

void on_read(App& app) {
  if (!app.modder().mem().is_open()) {
    app.set_status("请先点击\"初始化\"。");
    return;
  }
  if (app.item().item_read_pending) return;  // 上一轮捕获还在等
  try {
    if (app.modder().bag_flag_active()) {
      // 背包拿起边缘门:闩锁 = 最近一次"从背包拿起(0->1)"的物品,读它即可。
      // 悬停/场景读取在 flag 平直期不会入闩锁,无需再等捕获。
      uintptr_t p = app.modder().get_item_pointer();
      if (p < 0x10000) {
        app.set_status("请先在游戏里从背包拿起一件物品(点一下让图标贴到光标),再点\"读取属性\"。");
        return;
      }
      finish_item_read(app, p + app.modder().config().current().item_base_adjust);
      return;
    }
    if (app.modder().capture_freeze_supported()) {
      // 解冻让新捕获可通过;绝不清槽——不匹配门控的读取不改 cur_item,
      // 失败也保持原地址(只会被 cave 的合法捕获改写)。
      // 等待交给 tick_item_read, 不 Sleep 卡 UI。
      app.modder().set_capture_frozen(false);
      app.item().item_read_pending = true;
      app.item().item_read_accum = 0.0f;
      app.set_status("正在捕获手持物品…(请保持武器握在手上)");
      tick_item_read(app, 0.0f);  // 槽里已有物品则本帧立刻读完
      return;
    }
    finish_item_read(app, app.modder().get_item_base());
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("读取失败", msg, MB_ICONERROR);
    app.set_status("读取失败:" + msg);
    app.item().item_read_pending = false;
  }
}

void tick_item_read(App& app, float dt) {
  if (!app.item().item_read_pending) return;
  if (!app.modder().mem().is_open()) {
    app.item().item_read_pending = false;
    app.item().item_read_accum = 0.0f;
    return;
  }
  try {
    uintptr_t p = app.modder().get_item_pointer();
    if (p >= 0x10000) {
      app.modder().set_capture_frozen(true);
      app.item().item_read_pending = false;
      app.item().item_read_accum = 0.0f;
      finish_item_read(app, p + app.modder().config().current().item_base_adjust);
      return;
    }
    app.item().item_read_accum += dt;
    if (app.item().item_read_accum >= 3.0f) {
      app.modder().set_capture_frozen(true);
      app.item().item_read_pending = false;
      app.item().item_read_accum = 0.0f;
      app.set_status("未读到手持物品,请手持武器后重试(cur_item 保持原地址不变)。");
    }
  } catch (const std::exception& ex) {
    app.modder().set_capture_frozen(true);
    app.item().item_read_pending = false;
    app.item().item_read_accum = 0.0f;
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("读取失败", msg, MB_ICONERROR);
    app.set_status("读取失败:" + msg);
  }
}

void on_save(App& app) {
  if (!app.modder().mem().is_open()) {
    app.set_status("请先点击\"初始化\"。");
    return;
  }
  auto props = app.item().attrs;
  try {
    uintptr_t ib = app.modder().get_item_base();
    BasicState bs = collect_basic_state(app);
    app.modder().write_basic_fields(ib, bs);
    auto written_skipped = app.modder().write_attributes(ib, props);
    int written = written_skipped.first;
    int skipped = written_skipped.second;
    // 保存成功后刷新快照(含用户编辑后的值),供"复制上一件物品"用。
    app.item().last_attrs = props;
    snapshot_names(app);
    app.item().last_item_base = ib;
    std::string msg = "保存完成:基础属性 " + std::to_string(bs.ints.size() + bs.strs.size()) +
                      " 项,属性 " + std::to_string(written) + " 条已写入。";
    if (skipped > 0) {
      msg += "(" + std::to_string(skipped) + " 条新增属性因节点池无空闲槽被跳过)";
      msg_box("部分属性未写入",
              "有 " + std::to_string(skipped) +
                  " 条新增属性因节点池无空闲槽被跳过。\n"
                  "若刚丢弃过武器或读档过,可点击\"重置节点池\"回收全部槽位后重试。\n"
                  "已写入的属性不受影响。",
              MB_ICONWARNING);
    }
    msg += "请在游戏中把物品扔到地上再捡起使修改生效。";
    app.set_status(msg);
    msg_box("修改成功", "请在游戏中把物品扔到地上在捡起来使修改生效", MB_ICONINFORMATION);
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("保存失败", msg, MB_ICONERROR);
    app.set_status("保存失败:" + msg);
  }
}

// 把缓存的上一件物品属性写到当前物品:
// 现有节点原位覆盖(前 min(m,k) 条),多出来的词条从节点池分配接到链尾,
// 少了则截断。名字/别名/类型按用户选择决定是否一起写。
void on_copy_prev_item(App& app) {
  if (!app.modder().mem().is_open()) {
    app.set_status("请先点击\"初始化\"。");
    return;
  }
  if (app.item().last_attrs.empty()) {
    app.set_status("还没有读取过任何物品的属性,无法复制。");
    return;
  }
  // 先问名字/别名/类型要不要一起复制, 再动手(取消 = 什么都不做)
  std::string q = "上一件物品:";
  if (!app.item().last_name.empty()) q += "\n  名字: " + app.item().last_name;
  if (!app.item().last_alias.empty()) q += "\n  别名: " + app.item().last_alias;
  if (app.item().last_type >= 0)
    q += "\n  类型: " + field_value_display(app, "类型", app.item().last_type);
  ask_include_ident(app, "复制上一件物品", q,
                    "这三项(名字/别名/类型)要不要一起复制过来？词条一定复制。",
                    "已取消复制上一件物品(什么都没改)。",
                    [&app](bool with_ident) { apply_copy_prev_item(app, with_ident); });
}

// 真正做"复制上一件物品": withIdent=true 连名字/别名/类型一起写。
static void apply_copy_prev_item(App& app, bool withIdent) {
  try {
    uintptr_t ib = app.modder().get_item_base();
    auto written_skipped = app.modder().write_attributes(ib, app.item().last_attrs);
    int written = written_skipped.first;
    int skipped = written_skipped.second;
    std::string msg =
        std::string(withIdent ? "已复制上一件物品的属性+名字/别名/类型,写入 "
                              : "已复制上一件物品的属性(名字/别名/类型保持原样),写入 ") +
        std::to_string(written) + " 条。";
    if (ib == app.item().last_item_base) {
      msg += "(注意:当前物品就是缓存的那件)";
    }
    if (skipped > 0) {
      msg += "(" + std::to_string(skipped) + " 条因节点池无空闲槽被跳过,可点\"重置节点池\"后重试)";
    }
    msg += "请在游戏中把物品扔到地上再捡起使修改生效。";
    // 表格同步为已写入的内容,与内存一致
    app.item().attrs = app.item().last_attrs;
    app.item().attr_sel.assign(app.item().attrs.size(), false);
    if (withIdent) {
      BasicState bs;
      if (!app.item().last_name.empty()) bs.strs["道具名字"] = app.item().last_name;
      if (!app.item().last_alias.empty()) bs.strs["道具别名"] = app.item().last_alias;
      if (app.item().last_type >= 0) bs.ints["类型"] = app.item().last_type;
      if (!bs.strs.empty() || !bs.ints.empty()) app.modder().write_basic_fields(ib, bs);
      for (size_t i = 0; i < app.cfg().basic_fields.size() && i < app.item().basic.size(); ++i) {
        const std::string& lb = app.cfg().basic_fields[i].label;
        if (!app.item().last_name.empty() && lb == "道具名字")
          std::snprintf(app.item().basic[i].strbuf, sizeof(app.item().basic[i].strbuf), "%s",
                        app.item().last_name.c_str());
        else if (!app.item().last_alias.empty() && lb == "道具别名")
          std::snprintf(app.item().basic[i].strbuf, sizeof(app.item().basic[i].strbuf), "%s",
                        app.item().last_alias.c_str());
        else if (app.item().last_type >= 0 && lb == "类型") {
          if (app.item().basic[i].is_enum)
            app.item().basic[i].enum_val = app.item().last_type;
          else
            app.item().basic[i].intval = app.item().last_type;
        }
      }
    }
    app.set_status(msg);
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    app.set_status("复制失败:" + msg);
  }
}

void on_load_tpl(App& app) {
  std::string path =
      open_file_dialog(app_dir_w(), L"JSON 文件 (*.json)\0*.json\0所有文件 (*)\0*.*\0\0");
  if (path.empty()) return;
  TemplateLoadResult res;
  try {
    res = JsonTemplate::load(path, app.cfg().basic_fields);
  } catch (const std::exception& ex) {
    msg_box("加载失败", "加载 JSON 失败,什么都不做:\n" + sanitize(ex.what(), !app.hidden),
            MB_ICONERROR);
    return;
  }
  // 基础属性:填入可写字段(只读字段不覆盖)
  for (size_t i = 0; i < app.cfg().basic_fields.size(); ++i) {
    const auto& f = app.cfg().basic_fields[i];
    auto it = res.basic_values.find(f.label);
    if (it == res.basic_values.end()) continue;
    AppItemState::BasicWidget& w = app.item().basic[i];
    if (f.type == "string_ptr") {
      std::snprintf(w.strbuf, sizeof(w.strbuf), "%d", it->second);
    } else if (w.is_enum) {
      w.enum_val = it->second;
    } else {
      w.intval = it->second;
    }
  }
  app.item().attrs = res.attrs;
  app.item().attr_sel.assign(app.item().attrs.size(), false);
  if (!res.errors.empty()) {
    std::string msg = "部分属性存在错误,跳过加载相关词条,请检查原始json\n\n";
    for (const auto& e : res.errors) msg += e + "\n";
    msg_box("部分属性存在错误", msg, MB_ICONWARNING);
  }
  // 名字/别名/类型: 模板里有就先问要不要一起导入, 再动手(取消 = 什么都不做)
  std::string nm, al, ty;
  {
    auto n1 = res.basic_strings.find("道具名字");
    if (n1 != res.basic_strings.end()) nm = n1->second;
    auto a1 = res.basic_strings.find("道具别名");
    if (a1 != res.basic_strings.end()) al = a1->second;
    auto t1 = res.basic_values.find("类型");
    if (t1 != res.basic_values.end()) ty = field_value_display(app, "类型", t1->second);
  }
  if (nm.empty() && al.empty() && ty.empty()) {
    apply_loaded_tpl(app, res, true);  // 没有这三项, 直接全量导入
    return;
  }
  std::string q = "模板里带有:";
  if (!nm.empty()) q += "\n  道具名字: " + nm;
  if (!al.empty()) q += "\n  道具别名: " + al;
  if (!ty.empty()) q += "\n  类型: " + ty;
  ask_include_ident(app, "导入模板", q, "这三项(名字/别名/类型)要不要一起导入？词条一定导入。",
                    "已取消导入(什么都没改)。",
                    [&app, res](bool with_ident) { apply_loaded_tpl(app, res, with_ident); });
}

// 真正应用模板: withIdent=false 时不导入 类型 与 名字/别名, 其余照常。
static void apply_loaded_tpl(App& app, const TemplateLoadResult& res, bool withIdent) {
  for (size_t i = 0; i < app.cfg().basic_fields.size(); ++i) {
    const auto& f = app.cfg().basic_fields[i];
    auto it = res.basic_values.find(f.label);
    if (it == res.basic_values.end()) continue;
    if (!withIdent && f.label == "类型") continue;  // 类型归入"名字/别名"一档
    AppItemState::BasicWidget& w = app.item().basic[i];
    if (f.type == "string_ptr") {
      std::snprintf(w.strbuf, sizeof(w.strbuf), "%d", it->second);
    } else if (w.is_enum) {
      w.enum_val = it->second;
    } else {
      w.intval = it->second;
    }
  }
  app.item().attrs = res.attrs;
  app.item().attr_sel.assign(app.item().attrs.size(), false);
  if (withIdent) {
    auto n1 = res.basic_strings.find("道具名字");
    auto a1 = res.basic_strings.find("道具别名");
    for (size_t i = 0; i < app.cfg().basic_fields.size() && i < app.item().basic.size(); ++i) {
      const std::string& lb = app.cfg().basic_fields[i].label;
      if (n1 != res.basic_strings.end() && lb == "道具名字")
        std::snprintf(app.item().basic[i].strbuf, sizeof(app.item().basic[i].strbuf), "%s",
                      n1->second.c_str());
      else if (a1 != res.basic_strings.end() && lb == "道具别名")
        std::snprintf(app.item().basic[i].strbuf, sizeof(app.item().basic[i].strbuf), "%s",
                      a1->second.c_str());
    }
  }
  if (!res.errors.empty()) {
    std::string msg = "部分属性存在错误,跳过加载相关词条,请检查原始json\n\n";
    for (const auto& e : res.errors) msg += e + "\n";
    msg_box("部分属性存在错误", msg, MB_ICONWARNING);
  }
  app.set_status(std::string("已从模板载入:") +
                 (withIdent ? "含名字/别名/类型," : "名字/别名/类型未导入,") + "基础属性 " +
                 std::to_string(res.basic_values.size()) + " 项,属性 " +
                 std::to_string(res.attrs.size()) + " 条。");
}

void on_save_as(App& app) {
  std::string path = save_file_dialog(app_dir_w(), L"JSON 文件 (*.json)\0*.json\0\0");
  if (path.empty()) return;
  try {
    BasicState st = collect_basic_state(app);
    JsonTemplate::save(path, st, app.cfg().basic_fields, app.item().attrs);
    app.set_status("已导出到 " + path + "。");
  } catch (const std::exception& ex) {
    std::string msg = sanitize(ex.what(), !app.hidden);
    msg_box("导出失败", msg, MB_ICONERROR);
    app.set_status("导出失败:" + msg);
  }
}
}  // namespace poq
