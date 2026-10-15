// 秦殇物品修改器 (PoqEditor) —— GM 命令 Tab(task8)
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
// GM 命令 Tab(task8)
// ============================================================================
// 版本选择复用顶部全局选择器;steam/v1.30 的区别集中在 Core/GmCmd.h 的
// GmVersion 常量(对齐 python gm_cmd.py 的"单模块双版本参数化"),本文件只做
// UI 编排。纯 C++ 实现,不调用 python。

// 解析 gm_cmd.txt 文本 -> (选项列表, 格式错误行数),对齐 python
// gm_tab._parse_cmd_list:每行 "{命令}#{显示名}";空行忽略;行首 # 注释;
// 无 # 则命令=显示名;多个 # 取第一个分割;命令为空(格式不符) -> 计为错误行。
std::pair<std::vector<std::pair<std::string, std::string>>, int> ParseGmCmdText(
    const std::string& raw) {
    std::vector<std::pair<std::string, std::string>> items;
    int errors = 0;
    std::string cur;
    auto add_line = [&]() {
        std::string s = Trim(cur);
        cur.clear();
        if (s.empty() || s[0] == '#') return;
        std::string cmd = s, disp = s;
        size_t pos = s.find('#');
        if (pos != std::string::npos) {
            cmd = Trim(s.substr(0, pos));
            disp = Trim(s.substr(pos + 1));
            if (disp.empty()) disp = cmd;
        }
        if (!cmd.empty())
            items.emplace_back(cmd, disp);
        else
            ++errors;  // 命令为空(格式不符) -> 错误行
    };
    for (size_t i = 0; i <= raw.size(); ++i) {
        char c = (i < raw.size()) ? raw[i] : '\n';
        if (c == '\r') continue;
        if (c != '\n') {
            cur.push_back(c);
            continue;
        }
        add_line();
    }
    if (!cur.empty()) add_line();
    return {std::move(items), errors};
}

// 读取 gm_cmd.txt(工具目录,UTF-8)填充"常用命令"下拉,返回格式错误行数。
// keep_previous_on_error=true 且存在错误行时保留原列表不变(调用方弹窗提示);
// 否则用解析结果替换。文件不存在/不可读 -> 空表、0 错误(不报错)。
int LoadGmCmdList(App& app, bool keep_previous_on_error) {
    FILE* f = _wfopen((AppDirW() + L"\\gm_cmd.txt").c_str(), L"rb");
    if (!f) return 0;
    std::string raw;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
    fclose(f);
    auto parsed = ParseGmCmdText(raw);
    if (parsed.second > 0 && keep_previous_on_error) return parsed.second;  // 保留旧列表
    app.gm_cmd_list = std::move(parsed.first);
    if (app.gm_cmdlist_sel >= static_cast<int>(app.gm_cmd_list.size())) app.gm_cmdlist_sel = 0;
    return parsed.second;
}

void AppendGmLog(App& app, const std::string& text) {
    if (text.empty()) return;
    if (!app.gm_log.empty()) app.gm_log += "\n";
    app.gm_log += text;
}

// 解析 GM 调用上下文(对齐 python gm_tab._call 的 pid/版本解析 + RunSummonTool 模式):
// pid 优先取已初始化(Modder 打开)的,否则取顶部下拉选中并临时 OpenProcess;
// 版本由当前模式决定(mode0=1.30 / mode1=steam),按该版本模块名找 rpg 基址,
// 找不到 -> 抛"版本不符"错误;exe = POQ.exe 基址。
void ResolveGmCtx(App& app, ProcessMemory& tmp_mem, ProcessMemory*& mem, unsigned int& pid,
                  uintptr_t& rpg, uintptr_t& exe, const gm_cmd::GmVersion*& ver) {
    mem = nullptr;
    if (app.modder.Mem().IsOpen()) {
        pid = app.modder.Pid();
        mem = &app.modder.Mem();
    } else {
        auto sel = TryGetSelectedPid(app);
        if (!sel) throw std::runtime_error("未选择进程：请先在顶部选择 POQ.EXE 并初始化。");
        pid = *sel;
        if (!tmp_mem.Open(pid))
            throw std::runtime_error("OpenProcess 失败（请以管理员身份运行）。");
        mem = &tmp_mem;
    }
    ver = (app.cfg.current_mode == "mode1") ? &gm_cmd::SteamVersion() : &gm_cmd::V130Version();
    auto base_opt = ProcessMemory::FindModuleBase(pid, ver->mod);
    if (!base_opt) {
        throw std::runtime_error(
            "未找到模块 " + ver->mod + "：当前运行的游戏版本与所选版本（" + app.cfg.Current().name +
            "）不符，或游戏未运行。请确认游戏正在运行，并在顶部选择正确的游戏版本后重新初始化。");
    }
    rpg = *base_opt;
    auto exe_opt = ProcessMemory::FindModuleBase(pid, "POQ.exe");
    exe = exe_opt.value_or(0);
}

// 静默刷新 GM 激活状态(对齐 python _refresh_status:读内存判断,不弹窗,失败=false)。
void GmRefreshReady(App& app) {
    bool ready = false;
    try {
        ProcessMemory tmp_mem;
        ProcessMemory* mem = nullptr;
        unsigned int pid = 0;
        uintptr_t rpg = 0, exe = 0;
        const gm_cmd::GmVersion* ver = nullptr;
        ResolveGmCtx(app, tmp_mem, mem, pid, rpg, exe, ver);
        gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
        ready = gm_cmd::IsGmReady(ctx);
    } catch (...) {
        ready = false;
    }
    app.gm_active = ready;
}

// GM 状态周期刷新(对齐 python QTimer 2s):仅 GM tab 可见时跑。
void TickGm(App& app, float dt) {
    if (!app.gm_visible) {
        app.gm_timer_accum = 0.0f;
        return;
    }
    app.gm_timer_accum += dt;
    if (app.gm_timer_accum < 2.0f) return;
    app.gm_timer_accum = 0.0f;
    GmRefreshReady(app);
}

void OnGmRestore(App& app);

void OnGmToggle(App& app) {
    if (app.gm_active) {
        OnGmRestore(app);
        return;
    }
    try {
        ProcessMemory tmp_mem;
        ProcessMemory* mem = nullptr;
        unsigned int pid = 0;
        uintptr_t rpg = 0, exe = 0;
        const gm_cmd::GmVersion* ver = nullptr;
        ResolveGmCtx(app, tmp_mem, mem, pid, rpg, exe, ver);
        gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
        gm_cmd::Method method = (app.gm_method == 1) ? gm_cmd::Method::R9 : gm_cmd::Method::Memory;
        // 激活/写 wizard.lst 的日志不进界面,进 F12 调试日志
        AppendGmLog(app, gm_cmd::Activate(ctx, method));
        GmRefreshReady(app);
        if (app.gm_active)
            app.SetStatus("GM 权限已激活。");
        else
            app.SetStatus("激活已执行，但未检测到 GM 权限（游戏内需读档进入后生效）。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("激活失败", msg, MB_ICONERROR);
        AppendGmLog(app, "激活失败：" + msg);
        app.SetStatus("激活失败：" + msg);
    }
}

void OnGmRestore(App& app) {
    try {
        ProcessMemory tmp_mem;
        ProcessMemory* mem = nullptr;
        unsigned int pid = 0;
        uintptr_t rpg = 0, exe = 0;
        const gm_cmd::GmVersion* ver = nullptr;
        ResolveGmCtx(app, tmp_mem, mem, pid, rpg, exe, ver);
        gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
        AppendGmLog(app, gm_cmd::Disable(ctx));
        GmRefreshReady(app);
        app.SetStatus("GM 权限已还原。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("还原失败", msg, MB_ICONERROR);
        AppendGmLog(app, "还原失败：" + msg);
        app.SetStatus("还原失败：" + msg);
    }
}

// 执行次数:未勾选=1;勾选则校验 1~20(对齐 python _parse_count)。非法返回 -1(已弹窗)。
int GmParseCount(App& app) {
    if (!app.ck_count) return 1;
    std::string txt = Trim(app.gm_count_buf);
    char* end = nullptr;
    long n = std::strtol(txt.c_str(), &end, 10);
    if (end == txt.c_str() || *end != '\0') {
        MsgBox("输入错误", "执行次数必须是整数。", MB_ICONWARNING);
        app.SetStatus("输入错误：执行次数必须是整数。");
        return -1;
    }
    if (n < 1 || n > 20) {
        MsgBox("输入错误", "执行次数必须在 1 ~ 20 之间。", MB_ICONWARNING);
        app.SetStatus("输入错误：执行次数必须在 1 ~ 20 之间。");
        return -1;
    }
    return static_cast<int>(n);
}

void OnGmExec(App& app) {
    std::string cmd = Trim(app.gm_cmd_buf);
    if (cmd.empty()) {
        MsgBox("命令为空", "请输入要执行的 GM 命令。", MB_ICONWARNING);
        app.SetStatus("命令为空");
        return;
    }
    int count = GmParseCount(app);
    if (count < 0) return;  // 已弹窗
    try {
        ProcessMemory tmp_mem;
        ProcessMemory* mem = nullptr;
        unsigned int pid = 0;
        uintptr_t rpg = 0, exe = 0;
        const gm_cmd::GmVersion* ver = nullptr;
        ResolveGmCtx(app, tmp_mem, mem, pid, rpg, exe, ver);
        gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
        for (int i = 0; i < count; ++i) {
            AppendGmLog(app, gm_cmd::Send(ctx, cmd));
            if (i + 1 < count) ::Sleep(100);  // 多次执行之间休息 100ms
        }
        // 记入本地命令历史(与上一条相同则不重复;最多缓存 30 条),浏览游标复位
        if (app.gm_history.empty() || app.gm_history.back() != cmd) {
            app.gm_history.push_back(cmd);
            if (app.gm_history.size() > 30) app.gm_history.erase(app.gm_history.begin());
        }
        app.gm_hist_idx = -1;
        // 执行命令的工具日志不进界面,进 F12 调试日志
        GmRefreshReady(app);
        app.SetStatus("已执行 " + std::to_string(count) + " 次命令：" + cmd);
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("执行失败", msg, MB_ICONERROR);
        AppendGmLog(app, "执行失败：" + msg);
        app.SetStatus("执行失败：" + msg);
    }
}

// GM命令 tab 主渲染(布局逐项对齐 python gm_tab.py _build_ui)。
void RenderGmTab(App& app) {
    app.gm_visible = true;
    // 版本切换 -> 重置激活态(对齐 python reload_mode)
    if (app.gm_mode_cache != app.cfg.current_mode) {
        app.gm_mode_cache = app.cfg.current_mode;
        app.gm_active = false;
        GmRefreshReady(app);
    }

    // 第1部分:激活状态行(左) + 激活方式 + 激活/还原(右)
    std::string status_text = app.gm_active ? "GM权限已激活" : "GM权限未激活";
    const ImVec4 status_color =
        app.gm_active ? ImVec4(0.0f, 0.8f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float row_w = ImGui::GetContentRegionAvail().x;
    // 三个按钮(激活/还原、执行、读取配置)统一宽度,统一右对齐
    const float gm_btn_w =
        ImGui::CalcTextSize("读取配置").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float method_w = 140.0f;
    const float method_label_w = ImGui::CalcTextSize("激活方式：").x;
    ImGui::TextColored(status_color, "%s", status_text.c_str());
    float right_w = method_label_w + method_w + gm_btn_w + spacing * 2.0f;
    if (ImGui::CalcTextSize(status_text.c_str()).x + right_w + spacing <= row_w)
        ImGui::SameLine(row_w - right_w);
    ImGui::TextUnformatted("激活方式：");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(method_w);
    const char* method_items[] = {"内存修改", "R9注入"};
    ImGui::Combo("##gm_method", &app.gm_method, method_items, 2);
    ImGui::SameLine();
    bool act_click = ImGui::Button(app.gm_active ? "还原" : "激活", ImVec2(gm_btn_w, 0));
    if (act_click) OnGmToggle(app);
    ImGui::Separator();

    // 第3部分:命令输入 + 执行次数 + 执行(全部一行)
    ImGui::TextUnformatted("命令：");
    ImGui::SameLine();
    // 右侧固定部分:执行次数checkbox + 次数输入框 + 执行按钮(与激活/读取配置同宽),整体右对齐
    const float ck_total = ImGui::GetFrameHeight() + spacing + ImGui::CalcTextSize("执行次数").x;
    const float count_w = 80.0f;
    const float right_fixed = ck_total + count_w + gm_btn_w + spacing * 2.0f;
    float cmd_w = ImGui::GetContentRegionAvail().x - right_fixed - spacing;
    if (cmd_w < 150.0f) cmd_w = 150.0f;
    ImGui::SetNextItemWidth(cmd_w);
    if (!app.gm_active) ImGui::BeginDisabled();
    bool exec_pressed = ImGui::InputText("##gm_cmd", app.gm_cmd_buf, sizeof(app.gm_cmd_buf),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(row_w - right_fixed);  // 右对齐:执行次数/次数框/执行按钮贴右缘
    bool ck = app.ck_count;
    if (ImGui::Checkbox("执行次数", &ck)) app.ck_count = ck;
    ImGui::SameLine();
    bool count_en = app.gm_active && app.ck_count;
    if (!count_en) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputText("##gm_count", app.gm_count_buf, sizeof(app.gm_count_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        exec_pressed = true;
    if (!count_en) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("执行", ImVec2(gm_btn_w, 0))) exec_pressed = true;
    if (!app.gm_active) ImGui::EndDisabled();
    if (exec_pressed) OnGmExec(app);

    // 第3.5部分:常用命令(一行:"常用命令：" 标签 + 右侧"读取配置"按钮;
    // 下方展开全部选项,点选后填入命令输入框)
    ImGui::TextUnformatted("常用命令：");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - gm_btn_w);
    if (ImGui::Button("读取配置", ImVec2(gm_btn_w, 0))) {
        // 重新读取 gm_cmd.txt;有错误行 -> 弹窗提示并保留上一次的选项
        int err = LoadGmCmdList(app, /*keep_previous_on_error=*/true);
        if (err > 0) {
            std::string msg = "常用命令文件 gm_cmd.txt 有 " + std::to_string(err) +
                              " 行格式错误，已保留上一次的选项。";
            MsgBox("读取配置", msg, MB_ICONWARNING);
            AppendGmLog(app, "读取配置：" + msg);
        } else {
            app.SetStatus("已重新读取常用命令 (" + std::to_string(app.gm_cmd_list.size()) + " 条)");
            AppendGmLog(app, "读取配置：已重新读取 " + std::to_string(app.gm_cmd_list.size()) +
                                 " 条常用命令");
        }
    }
    // 下方:展开全部选项(自动限高,超出滚动);点选 -> 命令输入框显示对应命令
    app.gm_cmdlist_focused = false;
    if (app.gm_cmd_list.empty()) {
        ImGui::TextDisabled("（暂无常用命令：可编辑同目录 gm_cmd.txt 后点“读取配置”加载）");
    } else {
        // 占满下方剩余空间;下方还要画 F12 调试日志时预留其高度
        float list_h = ImGui::GetContentRegionAvail().y;
        if (!app.hidden && !app.gm_log.empty())
            list_h -= (ImGui::GetTextLineHeightWithSpacing() + 130.0f + spacing * 3.0f + 2.0f);
        if (list_h < 30.0f) list_h = 30.0f;
        if (!app.gm_active) ImGui::BeginDisabled();
        ImGui::BeginChild("##gm_cmdlist", ImVec2(ImGui::GetContentRegionAvail().x, list_h),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        app.gm_cmdlist_focused = ImGui::IsWindowFocused();  // 列表持焦点时箭头留给列表滚动
        for (int i = 0; i < static_cast<int>(app.gm_cmd_list.size()); ++i) {
            const auto& kv = app.gm_cmd_list[static_cast<size_t>(i)];
            bool selected = (i == app.gm_cmdlist_sel);
            if (ImGui::Selectable(kv.second.c_str(), selected)) {
                app.gm_cmdlist_sel = i;
                std::snprintf(app.gm_cmd_buf, sizeof(app.gm_cmd_buf), "%s", kv.first.c_str());
            }
        }
        ImGui::EndChild();
        if (!app.gm_active) ImGui::EndDisabled();
    }

    // 上下箭头切换历史命令(仅当常用命令列表未持有焦点;单行输入框不消费上下箭头)。
    // 上:从最新一条往前翻,到第一条失效;下:往回翻,翻过最新一条后还原浏览前的输入原文,
    // 再按失效。
    if (!app.gm_cmdlist_focused && !app.gm_history.empty()) {
        const int n = static_cast<int>(app.gm_history.size());
        int idx = app.gm_hist_idx;
        std::string new_text;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            if (idx == -1) {
                app.gm_hist_draft = app.gm_cmd_buf;  // 保存输入框原文
                idx = n - 1;                         // 从最新一条开始浏览
            } else if (idx > 0) {
                --idx;  // 到第一条(0)时不再上翻
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            if (idx >= 0) {
                if (idx < n - 1) {
                    ++idx;  // 往新的方向翻
                } else {
                    idx = -1;  // 已在最新一条:下翻回到输入框原文,再按失效
                }
            }
        }
        if (idx != app.gm_hist_idx) {
            app.gm_hist_idx = idx;
            const std::string& text =
                (idx == -1) ? app.gm_hist_draft : app.gm_history[static_cast<size_t>(idx)];
            std::snprintf(app.gm_cmd_buf, sizeof(app.gm_cmd_buf), "%s", text.c_str());
        }
    }

    // F12 调试模式:显示工具日志(对齐 summon tab 模式)
    if (!app.hidden && !app.gm_log.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("工具输出:");
        ImGui::SameLine();
        if (ImGui::Button("复制##gmlog")) ImGui::SetClipboardText(app.gm_log.c_str());
        ImGui::SameLine();
        if (ImGui::Button("清空##gmlog")) app.gm_log.clear();
        ImGui::BeginChild("gm_log", ImVec2(-FLT_MIN, 130), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(app.gm_log.c_str());
        ImGui::EndChild();
    }
}

}  // namespace poq
