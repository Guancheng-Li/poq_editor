// 秦殇物品修改器 (PoqEditor) —— 主窗口(顶部进程栏/主框架/内存查看/偏移设置)
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

void RenderMemTab(App& app) {
    ImGui::Text("地址(0x...)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("##addr", app.raw_addr, sizeof(app.raw_addr));
    ImGui::SameLine(0, 20);
    ImGui::Text("长度");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputText("##size", app.raw_size, sizeof(app.raw_size));
    ImGui::SameLine(0, 20);
    if (ImGui::Button("读取")) OnRawRead(app);
    ImGui::SameLine();
    if (ImGui::Button("写入")) OnRawWrite(app);

    ImGui::Separator();
    float avail_y = ImGui::GetContentRegionAvail().y;
    if (avail_y > 40) avail_y -= 26;  // 给说明留一行
    ImGui::InputTextMultiline("##hex", app.raw_hex, sizeof(app.raw_hex), ImVec2(-FLT_MIN, avail_y),
                              ImGuiInputTextFlags_None);
    ImGui::TextDisabled(
        "查看拾取物品指针附近内存。写入:每行 \"地址: 十六进制字节\" 或纯字节均可。");
}

void RenderCfgTab(App& app) {
    // 模式选择
    std::vector<std::string> mode_labels;
    for (const auto& kv : app.cfg.modes) mode_labels.push_back(kv.first + "：" + kv.second.name);
    std::vector<const char*> mode_items;
    for (const auto& s : mode_labels) mode_items.push_back(s.c_str());
    int sel = app.mode_selected;
    ImGui::Text("模式:");
    ImGui::SameLine();
    if (ImGui::Combo("##mode", &sel, mode_items.data(), static_cast<int>(mode_items.size()))) {
        app.mode_selected = sel;
    }

    auto row = [&](const char* label, char* buf, size_t buflen) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(0, 12);
        ImGui::SetNextItemWidth(300);
        ImGui::InputText(("##cfg" + std::string(label)).c_str(), buf, buflen);
    };

    ImGui::Separator();
    row("注入模块 (hook.module)", app.cfg_module, sizeof(app.cfg_module));
    row("注入偏移 (hook.offset, 0x...)", app.cfg_offset, sizeof(app.cfg_offset));
    row("原指令字节 (hook.aob)", app.cfg_aob, sizeof(app.cfg_aob));
    row("重量偏移 (hook.weight_offset, 0x...)", app.cfg_weight_offset,
        sizeof(app.cfg_weight_offset));
    row("重量上限 (hook.weight_max)", app.cfg_weight_max, sizeof(app.cfg_weight_max));
    row("物品基址调整 (item_base_adjust)", app.cfg_item_adjust, sizeof(app.cfg_item_adjust));
    row("属性头偏移 (attr_list.head_offset, 0x...)", app.cfg_head, sizeof(app.cfg_head));
    row("节点 next 偏移 (0x...)", app.cfg_next, sizeof(app.cfg_next));
    row("节点 ID 偏移 (0x...)", app.cfg_id, sizeof(app.cfg_id));
    row("节点 color 偏移 (0x...)", app.cfg_color, sizeof(app.cfg_color));
    row("节点 precent 偏移 (0x...)", app.cfg_precent, sizeof(app.cfg_precent));
    row("节点 value 偏移 (0x...)", app.cfg_value, sizeof(app.cfg_value));

    ImGui::Separator();
    if (ImGui::Button("保存配置并应用")) OnSaveCfg(app);
    ImGui::TextDisabled(
        "说明:以上为当前模式的 magic number,可在 poq 不同版本间切换。修改后点击\"保存配置并应用\""
        "立即生效,但仅本次会话有效(单文件版不写配置文件,重启回到内置默认)。"
        "内置默认在 src/Core/OffsetsConfig.cpp 的 DefaultMode() 中维护,"
        "需要固定偏移时改代码重新编译。");
}
void RenderTopBar(App& app) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float frame_pad_x = style.FramePadding.x * 2.0f;

    // 右侧按钮组总宽(刷新进程 + 初始化 + 帮助,用于让下拉框占满剩余宽度并右对齐)
    const char* kRefresh = "刷新进程";
    const char* kInit = "初始化";
    const char* kHelp = "帮助";
    const float refresh_w = ImGui::CalcTextSize(kRefresh).x + frame_pad_x;
    const float init_w = ImGui::CalcTextSize(kInit).x + frame_pad_x;
    const float help_w = ImGui::CalcTextSize(kHelp).x + frame_pad_x;
    const float buttons_w = refresh_w + init_w + help_w + spacing * 2.0f;

    // 游戏版本(mode)下拉项:显示 mode 名(v1.30 / Steam版)
    std::vector<std::string> ver_labels;
    for (const auto& kv : app.cfg.modes) ver_labels.push_back(kv.second.name);
    std::vector<const char*> ver_items;
    for (const auto& s : ver_labels) ver_items.push_back(s.c_str());

    // 进程下拉(POQ.EXE)
    std::vector<std::string> proc_labels;
    for (const auto& p : app.processes) proc_labels.push_back(p.Display());
    std::vector<const char*> proc_items;
    for (const auto& s : proc_labels) proc_items.push_back(s.c_str());

    // 版本标签固定 + 版本下拉固定宽;进程下拉占满剩余;按钮贴右缘
    const char* kVerLabel = "游戏版本:";
    const char* kProcLabel = "选择进程:";
    const float ver_label_w = ImGui::CalcTextSize(kVerLabel).x;
    const float proc_label_w = ImGui::CalcTextSize(kProcLabel).x;
    const float ver_w = 150.0f;
    const float row_w = ImGui::GetContentRegionAvail().x;
    float proc_w = row_w - ver_label_w - ver_w - proc_label_w - buttons_w - spacing * 4.0f;
    if (proc_w < 80.0f) proc_w = 80.0f;  // 窗口过窄时给进程下拉保底

    // 游戏版本切换 = 切换 offsets 模式:先还原旧模式注入,再切换并提示重新初始化
    ImGui::TextUnformatted(kVerLabel);
    ImGui::SameLine(0, spacing);
    ImGui::SetNextItemWidth(ver_w);
    int ver_sel = app.mode_selected;
    if (ImGui::Combo("##ver", &ver_sel, ver_items.data(), static_cast<int>(ver_items.size()))) {
        if (ver_sel != app.mode_selected && ver_sel >= 0 &&
            static_cast<size_t>(ver_sel) < app.mode_ids.size()) {
            const std::string& mid = app.mode_ids[static_cast<size_t>(ver_sel)];
            if (app.modder.Mem().IsOpen()) {
                auto cr = app.modder.Close();  // 还原旧模式注入,避免残留 hook
                if (!cr.hook_restored || !cr.cave_freed) {
                    // 计划 P0: hook 没还原成功 -> 游戏里仍指向 cave, 必须让用户知道
                    std::string msg = "旧版本的注入未能完全还原：\n";
                    msg += cr.error;
                    msg += "\n\n游戏内仍可能残留指向已分配内存的跳转。";
                    msg += "\n建议关闭并重启游戏后再切换版本。";
                    MsgBox("清理失败", msg, MB_ICONERROR);
                }
            }
            app.cfg.SetCurrent(mid);
            app.modder.Config() = app.cfg;
            app.ReloadCfgBuffers();
            app.RefreshProcesses();
            app.SetStatus("已切换游戏版本:" + app.cfg.Current().name + "。请重新初始化。");
        }
    }
    ImGui::SameLine(0, spacing);
    ImGui::TextUnformatted(kProcLabel);
    ImGui::SameLine(0, spacing);
    ImGui::SetNextItemWidth(proc_w);
    int sel = app.proc_selected;
    if (ImGui::Combo("##proc", &sel, proc_items.data(), static_cast<int>(proc_items.size()))) {
        app.proc_selected = sel;
    }
    ImGui::SameLine(0, spacing);
    if (ImGui::Button("刷新进程")) app.RefreshProcesses();
    ImGui::SameLine(0, spacing);
    if (ImGui::Button("初始化")) OnInit(app);
    ImGui::SameLine(0, spacing);
    if (ImGui::Button("帮助")) {
        // 弹窗打开瞬间跳到当前所在 tab 的说明(仅第一帧生效,之后可自由切换)
        app.help_tab = app.cur_tab;
        app.help_jump = true;
        ImGui::OpenPopup("帮助");
    }
}
// ============================================================================
// 灰白主题
// ============================================================================

void ApplyBlueTheme() {
    // 默认 imgui 蓝色主题(深色 + 蓝色强调色)
    ImGui::StyleColorsDark();
    // 标题栏统一蓝色(经典 Windows 蓝):"关于"/消息等弹窗一致。
    // 主窗口走系统标题栏(NoTitleBar),不受影响。
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.24f, 0.48f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.40f, 0.78f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.24f, 0.48f, 0.80f);
}
// ============================================================================
// 主窗口 / 主循环
// ============================================================================

void RenderMainWindow(App& app) {
    // 精确贴住系统窗口客户区(左上 (0,0) 开始,尺寸=工作区);
    // 去掉 imgui 自绘标题栏,用系统标题栏(标题一致),不重复。
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("秦殇修改器", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    RenderTopBar(app);
    ImGui::Separator();

    // 选项卡区域(固定高度,把状态栏钉在底部)
    const float status_h = 26.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float tab_h = avail.y - status_h - 8.0f;
    if (tab_h < 100.0f) tab_h = 100.0f;

    ImGui::BeginChild("tabarea", ImVec2(avail.x, tab_h));
    app.gm_visible = false;  // 每帧先复位,GM tab 渲染时置 true(周期刷新只在可见时跑)
    if (ImGui::BeginTabBar("maintabs")) {
        if (ImGui::BeginTabItem("人物修改")) {
            app.cur_tab = 0;
            RenderCharTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("道具修改")) {
            app.cur_tab = 1;
            RenderBasicFields(app);
            ImGui::Separator();
            float table_h = ImGui::GetContentRegionAvail().y;
            if (table_h < 60.0f) table_h = 60.0f;
            RenderAttrTable(app, table_h);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("召唤修改")) {
            app.cur_tab = 2;
            RenderSummonTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("GM命令")) {
            app.cur_tab = 3;
            RenderGmTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("H2解密")) {
            app.cur_tab = 4;
            RenderH2Tab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("gam修改")) {
            app.cur_tab = 5;
            RenderGamTab(app);
            ImGui::EndTabItem();
        }
        if (!app.hidden && ImGui::BeginTabItem("内存查看")) {
            app.cur_tab = 6;
            RenderMemTab(app);
            ImGui::EndTabItem();
        }
        if (!app.hidden && ImGui::BeginTabItem("偏移设置")) {
            app.cur_tab = 7;
            RenderCfgTab(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    RenderHelpPopup(app);

    // 状态栏
    ImGui::Separator();
    ImGui::TextUnformatted(app.status.c_str());

    ImGui::End();
}

}  // namespace poq
