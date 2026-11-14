// 召唤 Tab —— ImGui 布局。

#include <cctype>
#include <string>
#include <vector>
#include "imgui.h"
#include "UI/App.h"
#include "UI/AppCfgState.h"
#include "UI/SummonTabData.h"
#include "UI/SummonTabLogic.h"

namespace poq {

namespace {
/** ASCII 小写化(中文按字节原样保留), 用于模糊搜索的大小写不敏感匹配。 */
std::string lower_ascii(const char* s) {
  std::string o = (s == nullptr) ? std::string() : std::string(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}
}  // namespace

// 两版共有的"召唤归属"下拉(对齐 python _add_group_row)。
void summon_group_row(App& app) {
  const bool is_steam = (app.cfg().cfg.current_mode == "mode1");
  if (is_steam) {
    if (app.sm_group < 0) app.sm_group = 0;
    if (app.sm_group > 1) app.sm_group = 1;
  } else {
    if (app.sm_group < 0) app.sm_group = 0;
    if (app.sm_group > 1) app.sm_group = 1;  // 敌方已移除(实测召不出来)
  }
  ImGui::TextUnformatted("召唤归属：");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(is_steam ? 300.f : 380.f);
  if (is_steam) {
    const char* group_items[] = {"1：队友，可控制", "2：盟友，不可控制，被动反击"};
    ImGui::Combo("##sm_group", &app.sm_group, group_items, 2);
  } else {
    const char* group_items[] = {"0：队友，可控制", "1：盟友，不可控制，被动反击"};
    ImGui::Combo("##sm_group", &app.sm_group, group_items, 2);
  }
  if (!is_steam) {
    ImGui::SameLine();
    ImGui::TextUnformatted("单次数量：");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputInt("##sm_count", &app.sm_count, 1, 1);
    if (app.sm_count < 0) app.sm_count = 0;
    if (app.sm_count > 5) app.sm_count = 5;
    ImGui::SameLine();
    ImGui::TextDisabled("0=不干预(默认=技能原始数量5); 1=只出一只(改创建循环上界, 不动参数解析)");
  }
}

// 召唤功能区(Steam / v1.30 共用 UI; 底层分别走 SummonSteam / Summon130)。
void render_summon_options(App& app) {
  const bool is_steam = (app.cfg().cfg.current_mode == "mode1");
  summon_group_row(app);

  // Steam 版也支持"召唤其它(类型表)": sm_kind==3 不再被强制回落
  if (app.sm_kind < 0 || app.sm_kind > 3) app.sm_kind = 0;

  // 三选一单选(对齐 python 互斥); 避免「秦兵仍选中 + 下拉还显示善翼」却走了 soldier_type=0 随机秦兵
  if (ImGui::RadioButton("召唤野兽", app.sm_kind == 0)) app.sm_kind = 0;
  ImGui::SameLine();
  std::vector<std::string> mon_labels = make_mon_labels(kSummonMonNames, 10);
  std::vector<const char*> mon_items;
  for (const auto& s : mon_labels) mon_items.push_back(s.c_str());
  if (app.sm_kind != 0) ImGui::BeginDisabled();
  ImGui::SetNextItemWidth(220);
  int prev_mon = app.sm_mon;
  if (ImGui::Combo("##sm_mon", &app.sm_mon, mon_items.data(), static_cast<int>(mon_items.size())))
    app.sm_kind = 0;
  if (app.sm_kind != 0) ImGui::EndDisabled();

  if (!is_steam && app.sm_kind == 0 && (app.sm_mon == 8 || app.sm_mon == 9)) {
    if (prev_mon != app.sm_mon) {
      app.sm_beast_sub = 0;
      refresh_beast_wild_list(app);
    }
    bool wild = app.sm_beast_wild;
    if (ImGui::Checkbox("类型表模板(特定属性)", &wild)) {
      app.sm_beast_wild = wild;
      if (wild && app.sm_beast_wild_items.empty()) refresh_beast_wild_list(app);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("刷新列表")) refresh_beast_wild_list(app);
    if (app.sm_beast_wild) {
      if (app.sm_beast_wild_items.empty()) {
        ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.2f, 1.0f),
                           "未扫到条目：请进游戏并初始化/选进程后点「刷新列表」。");
      } else {
        std::vector<const char*> wild_items;
        for (const auto& s : app.sm_beast_wild_items) wild_items.push_back(s.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::Combo("##sm_beast_wild", &app.sm_beast_sub, wild_items.data(),
                     static_cast<int>(wild_items.size()));
      }
    }
  } else if (app.sm_beast_wild) {
    app.sm_beast_wild = false;
    app.sm_beast_wild_items.clear();
  }

  if (ImGui::RadioButton("召唤刺客", app.sm_kind == 1)) app.sm_kind = 1;
  ImGui::SameLine();
  std::vector<std::string> ass_labels;
  for (int i = 0; i < 2; ++i)
    ass_labels.push_back(std::to_string(i) + "：" + kSummonAssassinNames[i]);
  std::vector<const char*> ass_items;
  for (const auto& s : ass_labels) ass_items.push_back(s.c_str());
  if (app.sm_kind != 1) ImGui::BeginDisabled();
  ImGui::SetNextItemWidth(220);
  if (ImGui::Combo("##sm_assassin", &app.sm_assassin, ass_items.data(),
                   static_cast<int>(ass_items.size())))
    app.sm_kind = 1;
  if (app.sm_kind != 1) ImGui::EndDisabled();
  if (app.sm_kind == 1) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                       is_steam ? "攻击力较弱" : "1.30 刺客机制未充分验证");
  }

  if (ImGui::RadioButton("召唤秦兵", app.sm_kind == 2)) app.sm_kind = 2;
  ImGui::SameLine();
  std::vector<std::string> sol_labels;
  sol_labels.push_back("0：随机秦兵（默认）");
  for (size_t i = 0; i < summon_qin_cats().size(); ++i)
    sol_labels.push_back(std::to_string(i + 1) + "：" + summon_qin_cats()[i].name);
  std::vector<const char*> sol_items;
  for (const auto& s : sol_labels) sol_items.push_back(s.c_str());
  if (app.sm_kind != 2) ImGui::BeginDisabled();
  ImGui::SetNextItemWidth(220);
  int old_soldier = app.sm_soldier;
  if (ImGui::Combo("##sm_soldier", &app.sm_soldier, sol_items.data(),
                   static_cast<int>(sol_items.size()))) {
    app.sm_kind = 2;
    if (app.sm_soldier != old_soldier) {
      // 重填子类下拉(仅 1-6):过滤"旧重剑",子类 userData = 原列表索引
      app.sm_sub_items.clear();
      app.sm_sub = -1;
      if (app.sm_soldier >= 1 && app.sm_soldier <= 6) {
        const SummonCatUi& cat = summon_qin_cats()[static_cast<size_t>(app.sm_soldier - 1)];
        for (size_t i = 0; i < cat.subs.size(); ++i) {
          // Steam 无旧重剑子类; v1.30 类型表有, 仅 1.30 显示
          if (is_steam && app.sm_soldier == 2 && std::string(cat.subs[i]) == "旧重剑") continue;
          app.sm_sub_items.push_back(static_cast<int>(i));
        }
        if (!app.sm_sub_items.empty()) app.sm_sub = app.sm_sub_items[0];
      }
    }
  }
  if (app.sm_kind != 2) ImGui::EndDisabled();
  // 子类下拉(仅 soldier_type∈1-6 且非空时 enable,对齐 task7 #3.3.1)
  ImGui::SameLine();
  bool sub_en =
      (app.sm_kind == 2 && app.sm_soldier >= 1 && app.sm_soldier <= 6 && !app.sm_sub_items.empty());
  std::vector<const char*> sub_items;
  int sub_sel = -1;
  if (sub_en) {
    const SummonCatUi& cat = summon_qin_cats()[static_cast<size_t>(app.sm_soldier - 1)];
    for (size_t k = 0; k < app.sm_sub_items.size(); ++k) {
      sub_items.push_back(cat.subs[static_cast<size_t>(app.sm_sub_items[k])]);
      if (app.sm_sub_items[k] == app.sm_sub) sub_sel = static_cast<int>(k);
    }
    if (sub_sel < 0) sub_sel = 0;
  }
  if (!sub_en) ImGui::BeginDisabled();
  ImGui::SetNextItemWidth(160);
  if (ImGui::Combo("##sm_sub", &sub_sel, sub_items.data(), static_cast<int>(sub_items.size())) &&
      sub_sel >= 0 && sub_sel < static_cast<int>(app.sm_sub_items.size())) {
    app.sm_sub = app.sm_sub_items[static_cast<size_t>(sub_sel)];
  }
  if (!sub_en) ImGui::EndDisabled();

  // 召唤其它(NPC/类型表) —— v1.30 与 Steam 版都支持
  {
    if (ImGui::RadioButton("召唤其它(NPC/类型表)", app.sm_kind == 3)) {
      app.sm_kind = 3;
      if (app.sm_misc_items.empty()) refresh_misc_summon_list(app);
    }
    ImGui::SameLine();
    if (app.sm_kind != 3) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##sm_misc_search", "模糊搜索: 名称/别名/rec",
                             app.sm_misc_search, IM_ARRAYSIZE(app.sm_misc_search));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(420);
    // 按输入过滤选项(子串匹配, ASCII 不区分大小写); 过滤后自动把选择收敛到可见项,
    // 避免"下拉显示 A、实际召唤 B"。
    const std::string query = lower_ascii(app.sm_misc_search);
    std::vector<int> vis;
    std::vector<const char*> misc_items;
    for (size_t i = 0; i < app.sm_misc_items.size(); ++i) {
      if (!query.empty() && lower_ascii(app.sm_misc_items[i].c_str()).find(query) ==
                                std::string::npos) {
        continue;
      }
      vis.push_back(static_cast<int>(i));
      misc_items.push_back(app.sm_misc_items[i].c_str());
    }
    if (misc_items.empty()) {
      ImGui::TextUnformatted(query.empty() ? "（列表空）" : "（没有匹配的条目）");
    } else {
      int sel = 0;
      for (size_t k = 0; k < vis.size(); ++k) {
        if (vis[k] == app.sm_misc) {
          sel = static_cast<int>(k);
          break;
        }
      }
      if (ImGui::Combo("##sm_misc", &sel, misc_items.data(),
                       static_cast<int>(misc_items.size()))) {
        app.sm_kind = 3;
        app.sm_misc = vis[static_cast<size_t>(sel)];
      } else if (vis[static_cast<size_t>(sel)] != app.sm_misc) {
        app.sm_misc = vis[static_cast<size_t>(sel)];  // 当前项被过滤掉 -> 收敛到第一条可见项
      }
    }
    if (app.sm_kind != 3) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("刷新类型表列表")) refresh_misc_summon_list(app);
    if (app.sm_kind == 3 && app.sm_misc_items.empty()) {
      ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.2f, 1.0f),
                         "类型表为空：请进游戏、管理员初始化后点「刷新类型表列表」(含赵高等 NPC)。");
    } else if (app.sm_kind == 3 && !app.sm_misc_items.empty()) {
      ImGui::TextDisabled("共 %d 条, 当前筛选出 %d 条 (无过滤: 类型表全部记录, 含野兽/召唤兽/刺客/朱厌善翼)",
                          static_cast<int>(app.sm_misc_items.size()),
                          static_cast<int>(misc_items.size()));
      if (!app.sm_misc_search[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("· 输入关键字可模糊过滤");
      }
    }
  }

  if (!is_steam) {
    ImGui::TextColored(ImVec4(0.55f, 0.45f, 0.2f, 1.0f),
                       "v1.30: 敌方=Friend -1; 召唤偏弱为引擎/创建参限制, 勿改 0x20C18(会召不出)。");
  }
}

// 召唤修改 tab 主渲染。
void render_summon_tab(App& app) {
  // 版本切换 -> 重置激活态(对齐 python reload_mode)
  if (app.summon_mode_cache != app.cfg().cfg.current_mode) {
    app.summon_mode_cache = app.cfg().cfg.current_mode;
    app.summon_active = false;
    app.summon_log.clear();
    app.sm_kind = 0;
    app.sm_sub = -1;
    app.sm_sub_items.clear();
    app.sm_misc = 0;
    app.sm_misc_items.clear();
    app.sm_misc_recs.clear();
  }

  // 第一行:激活状态标签(左) + 激活/取消 + 刷新召唤(右)
  // 激活时:"激活状态：激活，当前召唤的怪物：<类别>，<阵营>";阵营=队友(group 0)/盟友(group 1)
  std::string status_text = app.summon_active ? "激活状态：激活，当前召唤的怪物：" +
                                                    summon_current_selection(app) + "，" +
                                                    (app.sm_group == 0   ? "队友"
                                                     : app.sm_group == 1 ? "盟友"
                                                                         : "敌方")
                                              : "激活状态：默认：召唤狼";
  const float row_w = ImGui::GetContentRegionAvail().x;
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float btn_w = 90.0f;
  const float refresh_w = 100.0f;
  const float label_w = ImGui::CalcTextSize(status_text.c_str()).x;
  ImGui::TextUnformatted(status_text.c_str());
  if (label_w + btn_w + refresh_w + spacing * 2.0f <= row_w)
    ImGui::SameLine(row_w - btn_w - refresh_w - spacing);

  // 激活/取消(不依赖初始化:未初始化也可点,点击时在 run_summon_tool 检查进程/版本)
  bool act_click = ImGui::Button(app.summon_active ? "取消" : "激活", ImVec2(btn_w, 0));
  ImGui::SameLine();
  if (!app.summon_active) ImGui::BeginDisabled();
  if (ImGui::Button("刷新召唤", ImVec2(refresh_w, 0))) on_summon_refresh(app);
  if (!app.summon_active) ImGui::EndDisabled();
  if (act_click) on_summon_activate(app);

  ImGui::Separator();

  render_summon_options(app);

  // F12 调试模式:显示工具输出日志(对齐 python set_log_visible, 只读可滚动,可复制/清空)
  if (!app.hidden && !app.summon_log.empty()) {
    ImGui::Separator();
    ImGui::TextUnformatted("工具输出:");
    ImGui::SameLine();
    if (ImGui::Button("复制")) ImGui::SetClipboardText(app.summon_log.c_str());
    ImGui::SameLine();
    if (ImGui::Button("清空")) app.summon_log.clear();
    ImGui::BeginChild("summon_log", ImVec2(-FLT_MIN, 130), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(app.summon_log.c_str());
    ImGui::EndChild();
  }
}
}  // namespace poq
