// 秦殇物品修改器 (PoqEditor) —— 召唤修改 Tab(task7)
// (#18 自 src/main.cpp 机械拆分, 代码逐字节搬移, 逻辑未改动。)。

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>  // SHBrowseForFolderW(目录选择)
#include <shobjidl.h>


#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <functional>
#include "imgui.h"

#include "UI/App.h"

#include "Core/Characters.h"
#include "Core/GamCatalog.h"
#include "Core/GamFile.h"
#include "Core/GameModder.h"
#include "Core/GmCmd.h"
#include "Core/ItemData.h"
#include "Core/ItemProperty.h"
#include "Core/Json.h"
#include "Core/JsonTemplate.h"
#include "Core/OffsetsConfig.h"
#include "Core/Summon130.h"
#include "Core/SummonSteam.h"
#include "Native/ProcessMemory.h"

namespace poq {

// ============================================================================
// 召唤修改 Tab(task7)
// ============================================================================
// 版本选择复用顶部全局选择器;steam/v1.30 两套召唤逻辑分别调用
// summon_steam / summon_130 独立模块(文件级隔离,不做配置耦合,对齐 python
// summon_tab.py + data/py/summon_tool_{steam,130}.py)。本文件只做 UI 编排。

// UI 数据镜像(对齐 python summon_tab.py: 下拉文案含序号,子类名与工具模块一致)。
const char* kSummonMonNames[10] = {"狼", "虎",   "熊",   "白狼", "木偶",
                                   "狗", "虎蛟", "狍鸮", "朱厌", "善翼"};
const char* kSummonAssassinNames[2] = {"弓箭刺客", "带刀刺客"};
struct SummonCatUi {
    const char* name;
    std::vector<const char*> subs;
};
const std::vector<SummonCatUi>& SummonQinCats() {
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
std::vector<std::string> MakeMonLabels(const char* const* names, int count) {
    std::vector<std::string> out;
    for (int i = 0; i < count; ++i) out.push_back(std::to_string(i) + "：" + names[i]);
    return out;
}

// 当前 UI 选择的召唤目标字符串(激活状态标签用,对齐 python _current_selection)。
std::string SummonCurrentSelection(const App& app) {
    if (app.cfg.current_mode == "mode1") {  // steam
        if (app.sm_kind == 0) return kSummonMonNames[app.sm_mon];
        if (app.sm_kind == 1) return kSummonAssassinNames[app.sm_assassin];
        if (app.sm_kind == 2) {
            if (app.sm_soldier == 0) return "随机秦兵";
            const SummonCatUi& cat = SummonQinCats()[static_cast<size_t>(app.sm_soldier - 1)];
            std::string sub = (app.sm_sub >= 0) ? cat.subs[static_cast<size_t>(app.sm_sub)] : "";
            return "秦兵-" + std::string(cat.name) + "-" + sub;
        }
        return "狼";  // 全不选:默认召唤狼
    }
    // v1.30
    if (app.sm130_kind == 0) return kSummonMonNames[app.sm130_mon];
    return "随机秦兵";
}

// 按当前 UI 构建工具命名参数(对齐 python _build_args)。
std::vector<std::string> BuildSummonArgs(const App& app) {
    std::vector<std::string> args;
    args.push_back("--group");
    args.push_back(std::to_string(app.sm_group));
    if (app.cfg.current_mode == "mode1") {  // steam
        if (app.sm_kind == 0) {
            args.push_back("--mon_type");
            args.push_back(std::to_string(app.sm_mon));
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
        } else {
            args.push_back("--mon_type");
            args.push_back("0");  // 默认狼
        }
    } else {  // v1.30
        if (app.sm130_kind == 0) {
            args.push_back("--mon_type");
            args.push_back(std::to_string(app.sm130_mon));
        } else if (app.sm130_kind == 1) {
            args.push_back("--soldier_type");
            args.push_back("0");  // 随机秦兵(task7 #4)
        } else {
            args.push_back("--mon_type");
            args.push_back("0");
        }
    }
    return args;
}

void AppendSummonLog(App& app, const std::string& text) {
    if (text.empty()) return;
    if (!app.summon_log.empty()) app.summon_log += "\n";
    app.summon_log += text;
}

// 调用对应版本工具模块(进程内,不调 python)。
// 进程 pid:已初始化(Modder 打开)优先用 Pid();未初始化用顶部下拉选中的进程(对齐 python
// summon_tab._resolve_pid)并临时 OpenProcess(对齐 python 每次自开 mem)。版本检查在下面:
// 所选版本的模块找不到 -> 弹窗提示版本不符。
std::string RunSummonTool(App& app, const std::vector<std::string>& args) {
    unsigned int pid = 0;
    ProcessMemory tmp_mem;  // 未初始化时的临时句柄(作用域结束自动关闭)
    ProcessMemory* mem = nullptr;
    if (app.modder.Mem().IsOpen()) {
        pid = app.modder.Pid();
        mem = &app.modder.Mem();
    } else {
        auto sel = TryGetSelectedPid(app);
        if (!sel) {
            throw std::runtime_error("未选择进程：请先在顶部选择 POQ.EXE 并初始化。");
        }
        pid = *sel;
        if (!tmp_mem.Open(pid)) {
            throw std::runtime_error("OpenProcess 失败（请以管理员身份运行）。");
        }
        mem = &tmp_mem;
    }
    const ModeCfg& m = app.cfg.Current();
    auto base_opt = ProcessMemory::FindModuleBase(pid, m.hook.module);
    if (!base_opt) {
        throw std::runtime_error(
            "未找到模块 " + m.hook.module + "：当前运行的游戏版本与所选版本（" + m.name +
            "）不符，或游戏未运行。请确认游戏正在运行，并在顶部选择正确的游戏版本后重新初始化。");
    }
    if (app.cfg.current_mode == "mode1") return summon_steam::CmdSummon(*mem, *base_opt, args);
    return summon_130::CmdSummon(*mem, *base_opt, args);
}

void OnSummonCancel(App& app);  // 前向声明(OnSummonActivate 定义在其之前)

void OnSummonActivate(App& app) {
    if (app.summon_active) {
        OnSummonCancel(app);
        return;
    }
    try {
        std::string text = RunSummonTool(app, BuildSummonArgs(app));
        AppendSummonLog(app, text);
        app.summon_active = true;
        app.SetStatus("召唤修改已激活：力士使用\"烽火狼烟\"即可召唤。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("激活失败", msg, MB_ICONERROR);
        AppendSummonLog(app, "激活失败：" + msg);
        app.SetStatus("激活失败：" + msg);
    }
}

void OnSummonCancel(App& app) {
    try {
        std::string text = RunSummonTool(app, {"--group", "0", "--mon_type", "0"});
        AppendSummonLog(app, text);
        app.summon_active = false;
        app.SetStatus("已取消：恢复到烽火狼烟默认值（召唤狼）。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("取消失败", msg, MB_ICONERROR);
        AppendSummonLog(app, "取消失败：" + msg);
        app.SetStatus("取消失败：" + msg);
    }
}

void OnSummonRefresh(App& app) {
    if (!app.summon_active) return;
    try {
        std::string text = RunSummonTool(app, BuildSummonArgs(app));
        AppendSummonLog(app, text);
        app.SetStatus("已重新设置当前界面选择的怪物类型。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("刷新失败", msg, MB_ICONERROR);
        AppendSummonLog(app, "刷新失败：" + msg);
        app.SetStatus("刷新失败：" + msg);
    }
}

// 两版共有的"召唤归属"下拉(对齐 python _add_group_row)。
void SummonGroupRow(App& app) {
    ImGui::TextUnformatted("召唤归属：");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300);
    const char* group_items[] = {"1：队友，可控制", "2：盟友，不可控制，被动反击"};
    ImGui::Combo("##sm_group", &app.sm_group, group_items, 2);
}

// Steam 功能区(task7 #3):mon_type / assassin_type / soldier_type(+子类) 三选一。
void RenderSummonSteamFunc(App& app) {
    SummonGroupRow(app);

    // 召唤野兽 (mon_type 0-9)
    bool v = (app.sm_kind == 0);
    if (ImGui::Checkbox("召唤野兽", &v)) app.sm_kind = v ? 0 : -1;
    ImGui::SameLine();
    std::vector<std::string> mon_labels = MakeMonLabels(kSummonMonNames, 10);
    std::vector<const char*> mon_items;
    for (const auto& s : mon_labels) mon_items.push_back(s.c_str());
    if (app.sm_kind != 0) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("##sm_mon", &app.sm_mon, mon_items.data(), static_cast<int>(mon_items.size()));
    if (app.sm_kind != 0) ImGui::EndDisabled();

    // 刺客 (assassin_type 0/1)
    v = (app.sm_kind == 1);
    if (ImGui::Checkbox("召唤刺客", &v)) app.sm_kind = v ? 1 : -1;
    ImGui::SameLine();
    std::vector<std::string> ass_labels;
    for (int i = 0; i < 2; ++i)
        ass_labels.push_back(std::to_string(i) + "：" + kSummonAssassinNames[i]);
    std::vector<const char*> ass_items;
    for (const auto& s : ass_labels) ass_items.push_back(s.c_str());
    if (app.sm_kind != 1) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("##sm_assassin", &app.sm_assassin, ass_items.data(),
                 static_cast<int>(ass_items.size()));
    if (app.sm_kind != 1) ImGui::EndDisabled();
    if (app.sm_kind == 1) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "攻击力较弱");
    }

    // 秦兵 (soldier_type 0-6) + 子类 (soldier_sub_type, 仅 1-6 时生效)
    v = (app.sm_kind == 2);
    if (ImGui::Checkbox("召唤秦兵", &v)) app.sm_kind = v ? 2 : -1;
    ImGui::SameLine();
    std::vector<std::string> sol_labels;
    sol_labels.push_back("0：随机秦兵（默认）");
    for (size_t i = 0; i < SummonQinCats().size(); ++i)
        sol_labels.push_back(std::to_string(i + 1) + "：" + SummonQinCats()[i].name);
    std::vector<const char*> sol_items;
    for (const auto& s : sol_labels) sol_items.push_back(s.c_str());
    if (app.sm_kind != 2) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(220);
    int old_soldier = app.sm_soldier;
    if (ImGui::Combo("##sm_soldier", &app.sm_soldier, sol_items.data(),
                     static_cast<int>(sol_items.size()))) {
        if (app.sm_soldier != old_soldier) {
            // 重填子类下拉(仅 1-6):过滤"旧重剑",子类 userData = 原列表索引
            app.sm_sub_items.clear();
            app.sm_sub = -1;
            if (app.sm_soldier >= 1 && app.sm_soldier <= 6) {
                const SummonCatUi& cat = SummonQinCats()[static_cast<size_t>(app.sm_soldier - 1)];
                for (size_t i = 0; i < cat.subs.size(); ++i) {
                    if (app.sm_soldier == 2 && std::string(cat.subs[i]) == "旧重剑") continue;
                    app.sm_sub_items.push_back(static_cast<int>(i));
                }
                if (!app.sm_sub_items.empty()) app.sm_sub = app.sm_sub_items[0];
            }
        }
    }
    if (app.sm_kind != 2) ImGui::EndDisabled();
    // 子类下拉(仅 soldier_type∈1-6 且非空时 enable,对齐 task7 #3.3.1)
    ImGui::SameLine();
    bool sub_en = (app.sm_kind == 2 && app.sm_soldier >= 1 && app.sm_soldier <= 6 &&
                   !app.sm_sub_items.empty());
    std::vector<const char*> sub_items;
    int sub_sel = -1;
    if (sub_en) {
        const SummonCatUi& cat = SummonQinCats()[static_cast<size_t>(app.sm_soldier - 1)];
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
}

// v1.30 功能区(task7 #4):mon_type 0-7 + 随机秦兵 checkbox(无子类下拉)。
void RenderSummon130Func(App& app) {
    SummonGroupRow(app);

    // 召唤野兽 (mon_type 0-7; 8朱厌/9善翼 不显示,映射保留在工具模块)
    bool v = (app.sm130_kind == 0);
    if (ImGui::Checkbox("召唤野兽", &v)) app.sm130_kind = v ? 0 : -1;
    ImGui::SameLine();
    std::vector<std::string> mon_labels = MakeMonLabels(kSummonMonNames, 8);
    std::vector<const char*> mon_items;
    for (const auto& s : mon_labels) mon_items.push_back(s.c_str());
    if (app.sm130_kind != 0) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("##sm130_mon", &app.sm130_mon, mon_items.data(),
                 static_cast<int>(mon_items.size()));
    if (app.sm130_kind != 0) ImGui::EndDisabled();

    // 随机秦兵 (仅 checkbox, 无下拉框; 对应 soldier_type=0, 对齐 task7 #4)
    v = (app.sm130_kind == 1);
    if (ImGui::Checkbox("随机秦兵", &v)) app.sm130_kind = v ? 1 : -1;
}

// 召唤修改 tab 主渲染。
void RenderSummonTab(App& app) {
    // 版本切换 -> 重置激活态(对齐 python reload_mode)
    if (app.summon_mode_cache != app.cfg.current_mode) {
        app.summon_mode_cache = app.cfg.current_mode;
        app.summon_active = false;
        app.summon_log.clear();
        app.sm_kind = 0;
        app.sm130_kind = 0;
        app.sm_sub = -1;
        app.sm_sub_items.clear();
    }

    // 第一行:激活状态标签(左) + 激活/取消 + 刷新召唤(右)
    // 激活时:"激活状态：激活，当前召唤的怪物：<类别>，<阵营>";阵营=队友(group 0)/盟友(group 1)
    std::string status_text = app.summon_active ? "激活状态：激活，当前召唤的怪物：" +
                                                      SummonCurrentSelection(app) + "，" +
                                                      (app.sm_group == 0 ? "队友" : "盟友")
                                                : "激活状态：默认：召唤狼";
    const float row_w = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float btn_w = 90.0f;
    const float refresh_w = 100.0f;
    const float label_w = ImGui::CalcTextSize(status_text.c_str()).x;
    ImGui::TextUnformatted(status_text.c_str());
    if (label_w + btn_w + refresh_w + spacing * 2.0f <= row_w)
        ImGui::SameLine(row_w - btn_w - refresh_w - spacing);

    // 激活/取消(不依赖初始化:未初始化也可点,点击时在 RunSummonTool 检查进程/版本)
    bool act_click = ImGui::Button(app.summon_active ? "取消" : "激活", ImVec2(btn_w, 0));
    ImGui::SameLine();
    if (!app.summon_active) ImGui::BeginDisabled();
    if (ImGui::Button("刷新召唤", ImVec2(refresh_w, 0))) OnSummonRefresh(app);
    if (!app.summon_active) ImGui::EndDisabled();
    if (act_click) OnSummonActivate(app);

    ImGui::Separator();

    // 第三行:功能区(steam / v1.30 各自独立,整组切换渲染)
    if (app.cfg.current_mode == "mode1")
        RenderSummonSteamFunc(app);
    else
        RenderSummon130Func(app);

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
