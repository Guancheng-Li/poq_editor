// 召唤 Tab —— 下拉文案与 build_summon_args(Steam/1.30 共用)。

#include "UI/SummonTabData.h"
#include <stdexcept>
#include <string>
#include <vector>
#include "UI/App.h"
#include "UI/AppCfgState.h"
#include "UI/SummonTabLogic.h"

namespace poq {

const char* kSummonMonNames[10] = {"狼", "虎",   "熊",   "白狼", "木偶",
                                   "狗", "虎蛟", "狍鸮", "朱厌", "善翼"};
const char* kSummonAssassinNames[2] = {"弓箭刺客", "带刀刺客"};

const std::vector<SummonCatUi>& summon_qin_cats() {
  static const std::vector<SummonCatUi> cats = {
      {"普通兵", {"刀盾", "长戈", "弓箭"}},
      // 队长大类的 '旧重剑' 不支持:数据保留、下拉不显示(对齐 task7 #3.3.1)
      {"队长", {"长斧", "旧重剑", "重剑"}},
      {"大将", {"A", "B", "C", "D", "E"}},
      {"官员", {"官员"}},
      {"法师", {"A", "B", "C", "D", "E"}},
      {"大法师", {"A", "B", "C", "D", "E"}},
  };
  return cats;
}

// 带序号的下拉项("0：狼"...),供 Combo 使用。
std::vector<std::string> make_mon_labels(const char* const* names, int count) {
  std::vector<std::string> out;
  for (int i = 0; i < count; ++i) out.push_back(std::to_string(i) + "：" + names[i]);
  return out;
}

// 当前 UI 选择的召唤目标字符串(激活状态标签用,对齐 python _current_selection)。
std::string summon_current_selection(const App& app) {
  if (app.sm_kind == 0) {
    if (app.sm_beast_wild && (app.sm_mon == 8 || app.sm_mon == 9) &&
        !app.sm_beast_wild_items.empty()) {
      size_t ix = static_cast<size_t>(app.sm_beast_sub);
      if (ix >= app.sm_beast_wild_items.size()) ix = 0;
      return std::string(kSummonMonNames[app.sm_mon]) + "·" + app.sm_beast_wild_items[ix];
    }
    return kSummonMonNames[app.sm_mon];
  }
  if (app.sm_kind == 1) return kSummonAssassinNames[app.sm_assassin];
  if (app.sm_kind == 2) {
    if (app.sm_soldier == 0) return "随机秦兵";
    const SummonCatUi& cat = summon_qin_cats()[static_cast<size_t>(app.sm_soldier - 1)];
    std::string sub = (app.sm_sub >= 0) ? cat.subs[static_cast<size_t>(app.sm_sub)] : "";
    return "秦兵-" + std::string(cat.name) + "-" + sub;
  }
  if (app.sm_kind == 3 && !app.sm_misc_items.empty()) {
    size_t ix = static_cast<size_t>(app.sm_misc);
    if (ix >= app.sm_misc_items.size()) ix = 0;
    return "其它-" + app.sm_misc_items[ix];
  }
  return "狼";  // 全不选:默认召唤狼
}

// 按当前 UI 构建工具命名参数(对齐 python _build_args)。
std::vector<std::string> build_summon_args(const App& app) {
  std::vector<std::string> args;
  args.push_back("--group");
  args.push_back(std::to_string(app.sm_group));
  if (app.sm_kind == 0) {
    args.push_back("--mon_type");
    args.push_back(std::to_string(app.sm_mon));
    if (app.sm_beast_wild && (app.sm_mon == 8 || app.sm_mon == 9)) {
      args.push_back("--beast_wild");
      args.push_back("1");
      args.push_back("--beast_sub_type");
      args.push_back(std::to_string(app.sm_beast_sub));
    }
  } else if (app.sm_kind == 1) {
    args.push_back("--assassin_type");
    args.push_back(std::to_string(app.sm_assassin));
  } else if (app.sm_kind == 2) {
    args.push_back("--soldier_type");
    args.push_back(std::to_string(app.sm_soldier));
    if (app.sm_soldier >= 1 && app.sm_soldier <= 6 && app.sm_sub >= 0) {
      args.push_back("--soldier_sub_type");
      args.push_back(std::to_string(app.sm_sub));
    }
  } else if (app.sm_kind == 3) {
    if (app.sm_misc_recs.empty()) {
      throw std::runtime_error("其它怪物列表为空：请进游戏、选 1.30 进程后点「刷新列表」。");
    }
    int ix = app.sm_misc;
    if (ix < 0 || ix >= static_cast<int>(app.sm_misc_recs.size())) ix = 0;
    args.push_back("--type_table_rec");
    args.push_back(std::to_string(app.sm_misc_recs[static_cast<size_t>(ix)]));
  } else {
    args.push_back("--mon_type");
    args.push_back("0");  // 默认狼
  }
  // v1.30: 限制"一次召唤造几只"(烽火狼烟原始数量是 5, 会让同种怪物一下冒出好几只)
  if (app.cfg().cfg.current_mode != "mode1" && app.sm_count > 0) {
    args.push_back("--count");
    args.push_back(std::to_string(app.sm_count));
  }
  return args;
}
}  // namespace poq
