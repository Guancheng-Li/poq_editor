// 秦殇物品修改器 (PoqEditor) —— imgui 单文件版。
//
// 对应 poqeditor_cpp(C++/Qt)的 100% 功能复刻,UI 改用 Dear ImGui(Win32 + DirectX11,
// WARP 软件渲染回退),默认蓝色主题 + 中文字体(运行时加载系统微软雅黑),零 DLL 依赖单文件 exe。
//
// 结构:
//   WinMain(入口,无控制台窗口) -> D3D11 初始化 -> imgui 初始化 -> 主循环
//   主循环每帧渲染:进程栏 / TabBar(物品修改、内存查看、偏移设置,F12 切换隐藏) / 状态栏
//   App 结构持有全部状态;GameModder/ProcessMemory 等核心逻辑与 cpp 版完全一致。

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>  // SetCurrentProcessExplicitAppUserModelID
#include <shlobj.h>    // SHBrowseForFolderW(目录选择)
#include <commdlg.h>

#include <d3d11.h>
#include <tchar.h>

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

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "Core/GameModder.h"
#include "Core/Characters.h"
#include "Core/ItemData.h"
#include "Core/ItemProperty.h"
#include "Core/Json.h"
#include "Core/JsonTemplate.h"
#include "Core/OffsetsConfig.h"
#include "Core/SummonSteam.h"
#include "Core/Summon130.h"
#include "Core/GmCmd.h"
#include "Native/ProcessMemory.h"

// imgui_impl_win32.h 用 #if 0 注释掉了这个声明(避免拖入 windows.h 依赖),
// 这里在全局作用域补上前置声明,供 poq::WndProc 调用。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace poq {

// ============================================================================
// 工具函数
// ============================================================================

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

std::wstring AppDirW() {
    wchar_t self[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring p(self);
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

// imgui 消息弹窗队列(替代系统 MessageBox:入队后主循环逐帧弹 imgui modal,
// 样式与启动免责声明弹窗一致)。单文件单实例,全局队列保持 MsgBox 调用点签名不变。
struct MessageBoxMsg {
    std::string title;
    std::string text;
    int type = MB_ICONINFORMATION;
};
std::vector<MessageBoxMsg> g_msgbox_queue;

void MsgBox(const std::string& title, const std::string& text, UINT type) {
    g_msgbox_queue.push_back(MessageBoxMsg{title, text, static_cast<int>(type)});
}

std::string OpenFileDialog(const std::wstring& dir, const wchar_t* filter) {
    wchar_t buf[1024]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetOpenFileNameW(&ofn)) return WideToUtf8(buf);
    return "";
}

std::string SaveFileDialog(const std::wstring& dir, const wchar_t* filter) {
    wchar_t buf[1024]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetSaveFileNameW(&ofn)) return WideToUtf8(buf);
    return "";
}

std::string Hex8(uintptr_t a) {
    char b[32];
    std::snprintf(b, sizeof(b), "%08llX", static_cast<unsigned long long>(a));
    return b;
}

std::string HexStr(uint32_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%X", static_cast<unsigned>(v));
    return b;
}

bool IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// 非调试模式下去掉消息里的内存地址(0x...),只保留动作结果文本。
std::string Sanitize(const std::string& msg, bool debug) {
    if (debug) return msg;
    std::string out;
    size_t i = 0;
    while (i < msg.size()) {
        // 去掉 "@ 0x12345678:"(含前导 @ 与随后的冒号)
        if (msg[i] == '@') {
            size_t j = i + 1;
            while (j < msg.size() && msg[j] == ' ') ++j;
            if (j + 1 < msg.size() && msg[j] == '0' && (msg[j + 1] == 'x' || msg[j + 1] == 'X')) {
                size_t k = j + 2;
                while (k < msg.size() && IsHex(msg[k])) ++k;
                if (k > j + 2) {
                    if (k < msg.size() && msg[k] == ':') ++k;
                    i = k;
                    continue;
                }
            }
        }
        // 去掉独立的 "0x1234"(>=4 位十六进制)
        if (msg[i] == '0' && i + 1 < msg.size() && (msg[i + 1] == 'x' || msg[i + 1] == 'X')) {
            size_t k = i + 2;
            while (k < msg.size() && IsHex(msg[k])) ++k;
            if (k - (i + 2) >= 4) {
                i = k;
                continue;
            }
        }
        out.push_back(msg[i]);
        ++i;
    }
    // 清理残留的多余空格
    std::string r;
    bool prev_space = false;
    for (char c : out) {
        if (c == ' ') {
            if (!prev_space) r += c;
            prev_space = true;
        } else {
            r += c;
            prev_space = false;
        }
    }
    return Trim(r);
}

// 浮点值显示:整数去 .0,否则原样。
std::string FormatValue(float v) {
    if (v == static_cast<float>(static_cast<int>(v))) return std::to_string(static_cast<int>(v));
    char b[64];
    std::snprintf(b, sizeof(b), "%g", static_cast<double>(v));
    return b;
}

std::string FormatHexDump(uintptr_t base_addr, const std::vector<unsigned char>& data) {
    std::string out;
    char line[64];
    for (size_t i = 0; i < data.size(); i += 16) {
        std::snprintf(line, sizeof(line), "%08llX: ", static_cast<unsigned long long>(base_addr + i));
        out += line;
        size_t n = std::min<size_t>(16, data.size() - i);
        for (size_t j = 0; j < n; ++j) {
            std::snprintf(line, sizeof(line), "%02X ", data[i + j]);
            out += line;
        }
        out += "\n";
    }
    return out;
}

std::vector<unsigned char> ParseHexDump(const std::string& text) {
    std::vector<unsigned char> bytes;
    std::string line;
    for (size_t i = 0; i <= text.size(); ++i) {
        char c = (i < text.size()) ? text[i] : '\n';
        if (c == '\r') continue;
        if (c != '\n') {
            line.push_back(c);
            continue;
        }
        // 处理一行
        std::string t = Trim(line);
        line.clear();
        if (t.empty()) continue;
        size_t colon = t.find(':');
        if (colon != std::string::npos) t = Trim(t.substr(colon + 1));
        // 按空白切 token
        size_t a = 0;
        while (a < t.size()) {
            while (a < t.size() && (t[a] == ' ' || t[a] == '\t')) ++a;
            size_t b = a;
            while (b < t.size() && t[b] != ' ' && t[b] != '\t') ++b;
            if (b > a) {
                std::string tok = t.substr(a, b - a);
                if (tok.size() == 2) {
                    char* end = nullptr;
                    unsigned long v = std::strtoul(tok.c_str(), &end, 16);
                    if (end == tok.c_str() + 2) bytes.push_back(static_cast<unsigned char>(v));
                }
            }
            a = b;
        }
    }
    return bytes;
}

// ============================================================================
// App 状态(对应 cpp MainWindow 的成员)
// ============================================================================

// 角色字段控件状态(当前选中角色一份;切换角色时从内存重填)。
struct CharWidget {
    int intval = 0;           // dword/word/enum
    int pair_lo = 0;          // pair 下限
    int pair_hi = 0;          // pair 上限
    char strbuf[128] = {0};   // string_ptr(UTF-8,足够 50 个汉字)
};

struct App {
    OffsetsConfig cfg;
    GameModder modder;
    std::vector<ProcessInfo> processes;
    std::vector<BasicFieldCfg> basic_fields;

    // ---- 基础属性控件状态(与 basic_fields 平行) ----
    struct BasicWidget {
        bool is_enum = false;
        int intval = 0;          // 数值字段
        int enum_val = 0;        // enum 字段当前值
        char strbuf[256] = {0};  // string_ptr(UTF-8,足够 50 个汉字=150 字节)
    };
    std::vector<BasicWidget> basic;

    // ---- 属性表格 ----
    std::vector<ItemProperty> attrs;
    std::vector<bool> attr_sel;

    // ---- 进程栏 ----
    int proc_selected = -1;

    // ---- 内存查看 ----
    char raw_addr[64] = {0};
    char raw_size[16] = {0};
    char raw_hex[131072] = {0};

    // ---- 偏移设置 ----
    std::vector<std::string> mode_ids;
    int mode_selected = 0;
    char cfg_module[128] = {0};
    char cfg_offset[32] = {0};
    char cfg_aob[128] = {0};
    char cfg_weight_offset[32] = {0};
    char cfg_weight_max[32] = {0};
    char cfg_item_adjust[32] = {0};
    char cfg_head[32] = {0};
    char cfg_next[32] = {0};
    char cfg_id[32] = {0};
    char cfg_color[32] = {0};
    char cfg_precent[32] = {0};
    char cfg_value[32] = {0};
    char cfg_node_size[32] = {0};
    char cfg_max_nodes[32] = {0};

    // ---- 属性修改(角色)----
    std::map<std::string, uintptr_t> char_addrs;   // 姓名 -> 结构体地址
    std::vector<std::string> char_names;           // 下拉顺序(角色x-名字)
    int char_selected = 0;
    std::vector<CharWidget> char_widgets;          // 平行 CharFields,当前选中角色
    char money_buf[32] = {0};                      // 全局金钱输入
    bool ck_invincible = false;                    // 无敌模式
    bool ck_kill = false;                          // 一击必杀
    // 锁原值缓存:姓名 -> {label: 原值}(勾选时缓存,取消时恢复)
    std::map<std::string, std::map<std::string, CharValue>> def_cache;  // 防御/闪避
    std::map<std::string, std::map<std::string, CharValue>> hit_cache;  // 攻击/命中率
    float lock_accum = 0.0f;                       // 锁定时器累加(500ms 写回一次)
    std::string char_status;                       // 角色 tab 状态文本

    // ---- 召唤修改(task7)----
    bool summon_active = false;       // 激活态(激活后按钮变"取消",刷新可点)
    std::string summon_log;           // 召唤工具输出(F12 调试模式显示)
    std::string summon_mode_cache;    // 上次渲染的版本,检测切换并重置激活态
    int sm_group = 0;                 // 召唤归属: 0=队友可控(默认) 1=盟友不可控
    int sm_kind = 0;                  // steam 三选一: 0=野兽 1=刺客 2=秦兵 (-1=全不选)
    int sm_mon = 0;                   // steam 野兽(0-9)
    int sm_assassin = 0;              // steam 刺客(0=弓箭 1=带刀)
    int sm_soldier = 0;               // steam 秦兵(0=随机 1-6=六大类)
    std::vector<int> sm_sub_items;    // 当前秦兵大类的子类(原索引列表,过滤旧重剑)
    int sm_sub = -1;                  // 选中子类(原索引)
    int sm130_kind = 0;               // 1.30 二选一: 0=野兽 1=随机秦兵 (-1=全不选)
    int sm130_mon = 0;                // 1.30 野兽(0-7,朱厌/善翼不显示)

    // ---- GM命令(task8)----
    bool gm_active = false;           // 当前 GM 是否已激活(is_gm_ready 决定)
    std::string gm_log;               // 工具日志(F12 调试模式显示)
    std::string gm_mode_cache;        // 上次渲染的版本,检测切换并重置激活态
    int gm_method = 0;                // 激活方式: 0=内存修改 1=R9注入(默认内存修改)
    char gm_cmd_buf[256] = {0};       // 命令输入框(UTF-8)
    bool ck_count = false;            // 执行次数
    char gm_count_buf[8] = {0};       // 执行次数输入(默认 "1")
    float gm_timer_accum = 0.0f;      // 2s 周期刷新 GM 状态(仅 GM tab 可见时跑)
    bool gm_visible = false;          // GM tab 当前是否显示
    std::vector<std::pair<std::string, std::string>> gm_cmd_list;  // 常用命令 (命令, 显示名)
    int gm_cmdlist_sel = 0;           // 常用命令下拉当前选中项

    // ---- H2解密(task10)----
    std::wstring h2_path;             // 待转换文件(已有备份时等待用户选择)
    bool h2_decrypt = true;           // 待转换方向: true=解密 false=加密
    bool h2_wait_confirm = false;     // 已有备份,等待覆盖/跳过/取消选择
    std::wstring h2_dir;              // 待批量转换目录(等待确认)
    bool h2_dir_decrypt = true;       // 目录转换方向: true=解密 false=加密
    bool h2_wait_dir_confirm = false; // 目录转换确认弹窗待定

    // ---- 状态 / 静态选项 ----
    bool hidden = true;  // F12 前隐藏调试 tab
    bool show_welcome = true;  // 启动免责声明弹窗(每次打开应用显示一次)
    std::string status;
    std::vector<uint32_t> attr_ids, color_ids, percent_ids;

    App() : cfg(OffsetsConfig::Default()), modder(cfg) {
        basic_fields = cfg.Current().basic_fields;
        RebuildBasic();
        char_widgets.assign(CharFields().size(), CharWidget());  // 属性修改网格(未读取时全空)
        for (const auto& kv : ItemData::AttrNames()) attr_ids.push_back(kv.first);
        for (const auto& kv : ItemData::ColorNames()) color_ids.push_back(kv.first);
        for (const auto& kv : ItemData::PercentNames()) percent_ids.push_back(kv.first);
        RefreshProcesses();
        ReloadCfgBuffers();
        std::snprintf(gm_count_buf, sizeof(gm_count_buf), "1");  // 执行次数默认 1
        status = "就绪。请以管理员身份运行,初始化后在游戏中拾起物品。";
    }

    // 重建基础属性控件(配置变化时)
    void RebuildBasic() {
        basic.clear();
        for (const auto& f : basic_fields) {
            BasicWidget w;
            w.is_enum = !f.enum_map.empty();
            basic.push_back(w);
        }
    }

    // 刷新进程列表,只列 POQ.EXE(自动选中第一个,避免全系统进程干扰选择)
    void RefreshProcesses() {
        processes.clear();
        for (const auto& p : ProcessMemory::EnumerateProcesses())
            if (ToLower(p.name) == "poq.exe") processes.push_back(p);
        proc_selected = processes.empty() ? -1 : 0;
        std::string msg = "已列出 " + std::to_string(processes.size()) + " 个 POQ.EXE 进程。";
        if (processes.empty())
            msg += "未找到 POQ.EXE,请先启动游戏后点\"刷新进程\"。";
        status = msg;
    }

    void ReloadCfgBuffers() {
        mode_ids.clear();
        for (const auto& kv : cfg.modes) mode_ids.push_back(kv.first);
        // 定位当前模式
        mode_selected = 0;
        for (size_t i = 0; i < mode_ids.size(); ++i)
            if (mode_ids[i] == cfg.current_mode) {
                mode_selected = static_cast<int>(i);
                break;
            }
        const ModeCfg& m = cfg.Current();
        const HookCfg& hk = m.hook;
        const AttrListCfg& al = m.attr_list;
        std::snprintf(cfg_module, sizeof(cfg_module), "%s", hk.module.c_str());
        std::snprintf(cfg_offset, sizeof(cfg_offset), "%s", HexStr(hk.offset).c_str());
        std::snprintf(cfg_aob, sizeof(cfg_aob), "%s", hk.aob.c_str());
        std::snprintf(cfg_weight_offset, sizeof(cfg_weight_offset), "%s", HexStr(hk.weight_offset).c_str());
        std::snprintf(cfg_weight_max, sizeof(cfg_weight_max), "%u", hk.weight_max);
        std::snprintf(cfg_item_adjust, sizeof(cfg_item_adjust), "%d", m.item_base_adjust);
        std::snprintf(cfg_head, sizeof(cfg_head), "%s", HexStr(al.head_offset).c_str());
        std::snprintf(cfg_next, sizeof(cfg_next), "%s", HexStr(al.next_offset).c_str());
        std::snprintf(cfg_id, sizeof(cfg_id), "%s", HexStr(al.id_offset).c_str());
        std::snprintf(cfg_color, sizeof(cfg_color), "%s", HexStr(al.color_offset).c_str());
        std::snprintf(cfg_precent, sizeof(cfg_precent), "%s", HexStr(al.precent_offset).c_str());
        std::snprintf(cfg_value, sizeof(cfg_value), "%s", HexStr(al.value_offset).c_str());
        std::snprintf(cfg_node_size, sizeof(cfg_node_size), "%s", HexStr(al.node_size).c_str());
        std::snprintf(cfg_max_nodes, sizeof(cfg_max_nodes), "%u", al.max_nodes);
    }

    // 当前"类型"字段值(enum 取 enum_val,否则 intval)
    std::optional<int> CurrentTypeValue() const {
        for (size_t i = 0; i < basic_fields.size(); ++i) {
            if (basic_fields[i].label == "类型") {
                return basic[i].is_enum ? basic[i].enum_val : basic[i].intval;
            }
        }
        return std::nullopt;
    }

    void SetStatus(const std::string& msg) { status = Sanitize(msg, !hidden); }
};

// ============================================================================
// 进程 / 业务逻辑(对应 cpp MainWindow 的 slot)
// ============================================================================

// 前向声明:OnInit 调用 OnCharRead(定义在后面的属性修改逻辑区)。
void OnCharRead(App& app);

std::optional<unsigned int> TryGetSelectedPid(const App& app) {
    if (app.proc_selected >= 0 && static_cast<size_t>(app.proc_selected) < app.processes.size())
        return app.processes[static_cast<size_t>(app.proc_selected)].pid;
    return std::nullopt;
}

std::optional<unsigned int> FindPoqPid(const App& app) {
    for (const auto& p : app.processes)
        if (ToLower(p.name) == "poq.exe") return p.pid;
    return std::nullopt;
}

void OnInit(App& app) {
    auto pid = TryGetSelectedPid(app);
    if (!pid) {
        app.SetStatus("请先选择一个进程。");
        return;
    }
    bool restarted = false;
    uintptr_t ptr = 0;
    try {
        try {
            ptr = app.modder.Initialize(*pid);
        } catch (const std::exception& ex) {
            // 游戏可能已重启,选中 PID 失效:刷新进程列表后按名重试一次
            if (std::string(ex.what()).find("OpenProcess") == std::string::npos) throw;
            app.RefreshProcesses();
            auto new_pid = FindPoqPid(app);
            if (!new_pid || *new_pid == *pid) throw;
            pid = new_pid;
            for (size_t i = 0; i < app.processes.size(); ++i)
                if (app.processes[i].pid == *new_pid) {
                    app.proc_selected = static_cast<int>(i);
                    break;
                }
            ptr = app.modder.Initialize(*pid);
            restarted = true;
        }
        uintptr_t slot = app.modder.CurItemSlotAddr();
        std::snprintf(app.raw_addr, sizeof(app.raw_addr), "0x%s", Hex8(slot).c_str());
        const bool dbg = !app.hidden;
        std::string prefix = restarted ? "检测到游戏重启,已自动重新选择进程。" : "";
        if (dbg) {
            std::string base_info = "hook模块=" + app.cfg.Current().hook.module + " hook@0x" +
                                    Hex8(app.modder.HookAddr()) + " cur_item槽=0x" + Hex8(slot);
            if (ptr == 0 || ptr == 0xFFFFFFFF) {
                app.SetStatus(prefix +
                              "初始化成功(已注入)。当前无手持物品,请在游戏中拾起物品后点\"读取属性\"。" +
                              base_info);
            } else {
                app.SetStatus(prefix + "初始化成功(已注入)。手持物品指针 = 0x" + Hex8(ptr) +
                              base_info);
            }
        } else {
            if (ptr == 0 || ptr == 0xFFFFFFFF) {
                app.SetStatus(prefix +
                              "初始化成功(已注入)。当前无手持物品,请在游戏中拾起物品后点\"读取属性\"。");
            } else {
                app.SetStatus(prefix + "初始化成功(已注入)。");
            }
        }
        // 初始化后自动加载角色属性(对齐 Python _on_init)
        try {
            OnCharRead(app);
        } catch (...) {
        }
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("初始化失败", msg, MB_ICONERROR);
        app.SetStatus("初始化失败:" + msg);
    }
}

void FillBasic(App& app, const BasicState& st) {
    for (size_t i = 0; i < app.basic_fields.size(); ++i) {
        const auto& f = app.basic_fields[i];
        App::BasicWidget& w = app.basic[i];
        if (f.type == "string_ptr") {
            auto it = st.strs.find(f.label);
            if (it != st.strs.end())
                std::snprintf(w.strbuf, sizeof(w.strbuf), "%s", it->second.c_str());
        } else {
            auto it = st.ints.find(f.label);
            if (it != st.ints.end()) {
                if (w.is_enum) w.enum_val = it->second;
                else w.intval = it->second;
            }
        }
    }
}

BasicState CollectBasicState(const App& app) {
    BasicState st;
    for (size_t i = 0; i < app.basic_fields.size(); ++i) {
        const auto& f = app.basic_fields[i];
        const App::BasicWidget& w = app.basic[i];
        if (f.type == "string_ptr") st.strs[f.label] = w.strbuf;
        else if (w.is_enum) st.ints[f.label] = w.enum_val;
        else st.ints[f.label] = w.intval;
    }
    return st;
}

void OnRead(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先点击\"初始化\"。");
        return;
    }
    try {
        uintptr_t ib = app.modder.GetItemBase();
        FillBasic(app, app.modder.ReadBasicFields(ib));
        app.attrs = app.modder.ReadAttributes(ib);
        app.attr_sel.assign(app.attrs.size(), false);
        if (!app.hidden) {
            app.SetStatus("读取完成。物品基址 = 0x" + Hex8(ib) + ",属性 " +
                          std::to_string(app.attrs.size()) + " 条。");
        } else {
            app.SetStatus("读取完成,属性 " + std::to_string(app.attrs.size()) + " 条。");
        }
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("读取失败", msg, MB_ICONERROR);
        app.SetStatus("读取失败:" + msg);
    }
}

void OnSave(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先点击\"初始化\"。");
        return;
    }
    auto props = app.attrs;
    try {
        uintptr_t ib = app.modder.GetItemBase();
        BasicState bs = CollectBasicState(app);
        app.modder.WriteBasicFields(ib, bs);
        auto written_skipped = app.modder.WriteAttributes(ib, props);
        int written = written_skipped.first;
        int skipped = written_skipped.second;
        std::string msg = "保存完成:基础属性 " + std::to_string(bs.ints.size() + bs.strs.size()) +
                          " 项,属性 " + std::to_string(written) + " 条已写入。";
        if (skipped > 0) {
            msg += "(" + std::to_string(skipped) + " 条新增属性因静态节点池无空闲槽被跳过)";
            MsgBox("部分属性未写入",
                   "有 " + std::to_string(skipped) +
                       " 条新增属性因静态节点池无空闲槽被跳过。\n"
                       "可在偏移配置(F12)中调大 attr_list.static_size,或在游戏里换个物品试试。\n"
                       "已写入的属性不受影响。",
                   MB_ICONWARNING);
        }
        msg += "请在游戏中把物品扔到地上再捡起使修改生效。";
        app.SetStatus(msg);
        MsgBox("修改成功",
               "请在游戏中把物品扔到地上在捡起来使修改生效",
               MB_ICONINFORMATION);
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("保存失败", msg, MB_ICONERROR);
        app.SetStatus("保存失败:" + msg);
    }
}

void OnLoadTpl(App& app) {
    std::string path = OpenFileDialog(AppDirW(), L"JSON 文件 (*.json)\0*.json\0所有文件 (*)\0*.*\0\0");
    if (path.empty()) return;
    TemplateLoadResult res;
    try {
        res = JsonTemplate::Load(path, app.basic_fields);
    } catch (const std::exception& ex) {
        MsgBox("加载失败", "加载 JSON 失败,什么都不做:\n" + Sanitize(ex.what(), !app.hidden),
               MB_ICONERROR);
        return;
    }
    // 基础属性:填入可写字段(只读字段不覆盖)
    for (size_t i = 0; i < app.basic_fields.size(); ++i) {
        const auto& f = app.basic_fields[i];
        auto it = res.basic_values.find(f.label);
        if (it == res.basic_values.end()) continue;
        App::BasicWidget& w = app.basic[i];
        if (f.type == "string_ptr") {
            std::snprintf(w.strbuf, sizeof(w.strbuf), "%d", it->second);
        } else if (w.is_enum) {
            w.enum_val = it->second;
        } else {
            w.intval = it->second;
        }
    }
    app.attrs = res.attrs;
    app.attr_sel.assign(app.attrs.size(), false);
    if (!res.errors.empty()) {
        std::string msg = "部分属性存在错误,跳过加载相关词条,请检查原始json\n\n";
        for (const auto& e : res.errors) msg += e + "\n";
        MsgBox("部分属性存在错误", msg, MB_ICONWARNING);
    }
    app.SetStatus("已从模板载入:基础属性 " + std::to_string(res.basic_values.size()) +
                  " 项,属性 " + std::to_string(res.attrs.size()) + " 条。");
}

void OnSaveAs(App& app) {
    std::string path = SaveFileDialog(AppDirW(), L"JSON 文件 (*.json)\0*.json\0\0");
    if (path.empty()) return;
    try {
        BasicState st = CollectBasicState(app);
        JsonTemplate::Save(path, st, app.basic_fields, app.attrs);
        app.SetStatus("已导出到 " + path + "。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("导出失败", msg, MB_ICONERROR);
        app.SetStatus("导出失败:" + msg);
    }
}

void OnRawRead(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先\"初始化\"。");
        return;
    }
    try {
        uintptr_t addr = ProcessMemory::ParseAddress(Trim(app.raw_addr));
        int size = 64;
        char* end = nullptr;
        long parsed = std::strtol(app.raw_size, &end, 10);
        if (end != app.raw_size && *end == '\0' && parsed > 0) size = static_cast<int>(parsed);
        if (size > 4000) size = 4000;  // 防 buffer 溢出
        auto data = app.modder.ReadMemoryDump(addr, size);
        std::string dump = FormatHexDump(addr, data);
        std::snprintf(app.raw_hex, sizeof(app.raw_hex), "%s", dump.c_str());
        app.SetStatus("已读取 0x" + Hex8(addr) + " 共 " + std::to_string(data.size()) + " 字节。");
    } catch (const std::exception& ex) {
        app.SetStatus("读取失败:" + Sanitize(ex.what(), !app.hidden));
    }
}

void OnRawWrite(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先\"初始化\"。");
        return;
    }
    try {
        uintptr_t addr = ProcessMemory::ParseAddress(Trim(app.raw_addr));
        auto data = ParseHexDump(app.raw_hex);
        if (data.empty()) {
            app.SetStatus("没有可写入的十六进制字节。");
            return;
        }
        app.modder.Mem().Write(addr, data);
        app.SetStatus("已写入 0x" + Hex8(addr) + " 共 " + std::to_string(data.size()) + " 字节。");
    } catch (const std::exception& ex) {
        app.SetStatus("写入失败:" + Sanitize(ex.what(), !app.hidden));
    }
}

void OnSaveCfg(App& app) {
    try {
        if (app.mode_selected < 0 || static_cast<size_t>(app.mode_selected) >= app.mode_ids.size())
            throw std::runtime_error("无效模式");
        const std::string& mid = app.mode_ids[static_cast<size_t>(app.mode_selected)];
        auto mit = app.cfg.modes.find(mid);
        if (mit == app.cfg.modes.end()) throw std::runtime_error("无效模式");
        ModeCfg& m = mit->second;
        auto parse_hex = [](const char* s) -> uint32_t {
            return static_cast<uint32_t>(OffsetsConfig::ParseLong(Trim(s)));
        };
        auto parse_dec = [](const char* key, const char* s) -> long long {
            std::string t = Trim(s);
            if (t.empty()) throw std::runtime_error(std::string(key) + " 不是整数");
            char* end = nullptr;
            long long v = std::strtoll(t.c_str(), &end, 10);
            if (end == t.c_str() || *end != '\0') throw std::runtime_error(std::string(key) + " 不是整数");
            return v;
        };

        m.hook.module = app.cfg_module;
        m.hook.offset = parse_hex(app.cfg_offset);
        m.hook.aob = app.cfg_aob;
        m.hook.weight_offset = parse_hex(app.cfg_weight_offset);
        m.hook.weight_max = static_cast<uint32_t>(parse_dec("hook.weight_max", app.cfg_weight_max));
        m.item_base_adjust = static_cast<int>(parse_dec("item_base_adjust", app.cfg_item_adjust));
        AttrListCfg& al = m.attr_list;
        al.head_offset = parse_hex(app.cfg_head);
        al.next_offset = parse_hex(app.cfg_next);
        al.id_offset = parse_hex(app.cfg_id);
        al.color_offset = parse_hex(app.cfg_color);
        al.precent_offset = parse_hex(app.cfg_precent);
        al.value_offset = parse_hex(app.cfg_value);
        al.node_size = parse_hex(app.cfg_node_size);
        al.max_nodes = static_cast<uint32_t>(parse_dec("attr_list.max_nodes", app.cfg_max_nodes));
        app.cfg.SetCurrent(mid);
        // 单文件版:偏移只保存在内存中,不写配置文件(重启回到内置默认)。
        app.modder.Config() = app.cfg;
        app.basic_fields = app.cfg.Current().basic_fields;
        app.RebuildBasic();
        app.ReloadCfgBuffers();
        app.SetStatus("配置已应用(仅本次会话,重启后回到内置默认)。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("保存配置失败", msg, MB_ICONERROR);
        app.SetStatus("保存配置失败:" + msg);
    }
}

// ============================================================================
// 属性修改(角色)逻辑(对应 Python main_window.py 的 _on_char_* / _lock_*)
// ============================================================================

// 无敌模式涉及的防御字段(普通防御 + 五行防御)。
const std::set<std::string>& DefLabels() {
    static const std::set<std::string> s = {"普通防御", "金防御", "木防御", "水防御", "火防御", "土防御"};
    return s;
}
// 一击必杀涉及的攻击字段(普通攻击 + 五行攻击,均为 pair)。
const std::set<std::string>& AtkLabels() {
    static const std::set<std::string> s = {"普通攻击", "金攻击", "木攻击", "水攻击", "火攻击", "土攻击"};
    return s;
}

// 字段在当前锁状态下是否应禁用编辑(无敌:生命/精气/防御/闪避率;一击必杀:攻击/命中率)。
bool IsCharFieldLocked(const App& app, const std::string& label) {
    if (app.ck_invincible) {
        if (label == "生命" || label == "精气" || label == "闪避率" || DefLabels().count(label)) return true;
    }
    if (app.ck_kill) {
        if (label == "命中率" || AtkLabels().count(label)) return true;
    }
    return false;
}

// 按 label 查 CharFields 索引(找不到返回 -1)。
int FindCharField(const std::string& label) {
    const auto& fs = CharFields();
    for (size_t i = 0; i < fs.size(); ++i)
        if (fs[i].label == label) return static_cast<int>(i);
    return -1;
}

// 为当前选中角色重建字段控件(从内存读取并填充)。
void RebuildCharWidgets(App& app) {
    if (app.char_names.empty() || app.char_selected < 0 ||
        static_cast<size_t>(app.char_selected) >= app.char_names.size()) {
        app.char_widgets.clear();
        return;
    }
    const std::string& name = app.char_names[static_cast<size_t>(app.char_selected)];
    auto it = app.char_addrs.find(name);
    if (it == app.char_addrs.end()) {
        app.char_widgets.clear();
        return;
    }
    uintptr_t base = it->second;
    app.char_widgets.assign(CharFields().size(), CharWidget());
    auto values = ReadAllChar(app.modder.Mem(), base);
    for (size_t i = 0; i < CharFields().size(); ++i) {
        const auto& f = CharFields()[i];
        CharWidget& w = app.char_widgets[i];
        auto vit = values.find(f.label);
        if (vit == values.end()) continue;  // 读取失败:保留默认 0
        const CharValue& v = vit->second;
        if (f.kind == CharKind::StringPtr) {
            std::snprintf(w.strbuf, sizeof(w.strbuf), "%s", v.str.c_str());
        } else if (f.kind == CharKind::Pair) {
            w.pair_lo = v.lo;
            w.pair_hi = v.hi;
        } else {
            w.intval = v.lo;
        }
    }
}

// 读取全部队员:用固定偏移定位(按当前模式的引擎模块选偏移表),填充下拉框 + 全局属性。
void OnCharRead(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.char_status = "请先点击\"初始化\"。";
        app.SetStatus("请先点击\"初始化\"。");
        return;
    }
    try {
        const std::string& module = app.cfg.Current().hook.module;  // steam/v1.30
        auto located = LocateByOffsets(app.modder.Mem(), app.modder.Pid(), module);
        if (located.empty()) {
            app.char_status = "未定位到队员(偏移可能失效)。";
            app.SetStatus("未定位到任何队员。");
            return;
        }
        app.char_addrs.clear();
        app.char_names.clear();
        for (auto& kv : located) {
            app.char_addrs[kv.first] = kv.second;  // map 供按名查找
            app.char_names.push_back(kv.first);     // 槽位顺序保留下拉框
        }
        app.char_selected = 0;
        // 全局属性:从第 1 人读
        const std::string& first = app.char_names.front();
        auto g = ReadGlobalChar(app.modder.Mem(), app.char_addrs[first]);
        auto mit = g.find("金钱");
        if (mit != g.end()) std::snprintf(app.money_buf, sizeof(app.money_buf), "%d", mit->second.lo);
        RebuildCharWidgets(app);
        app.char_status = "已定位 " + std::to_string(located.size()) + " 人。";
        app.SetStatus("读取完成:定位 " + std::to_string(located.size()) + " 人。");
    } catch (const std::exception& ex) {
        app.char_status = "读取失败:" + Sanitize(ex.what(), !app.hidden);
        app.SetStatus("读取失败:" + Sanitize(ex.what(), !app.hidden));
    }
}

// 保存当前选中角色属性:收集 -> 校验(有非法则整体拒绝) -> 写入。
void OnCharSave(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先点击\"初始化\"。");
        return;
    }
    if (app.char_addrs.empty()) {
        app.SetStatus("请先\"读取属性\"定位队员。");
        return;
    }
    if (app.char_selected < 0 || static_cast<size_t>(app.char_selected) >= app.char_names.size()) {
        app.SetStatus("请先选择角色。");
        return;
    }
    const std::string& name = app.char_names[static_cast<size_t>(app.char_selected)];
    uintptr_t base = app.char_addrs[name];
    // 1) 收集全部字段值
    std::map<std::string, CharValue> values;
    for (size_t i = 0; i < CharFields().size(); ++i) {
        const auto& f = CharFields()[i];
        const CharWidget& w = app.char_widgets[i];
        CharValue v;
        if (f.kind == CharKind::StringPtr) {
            v.str = w.strbuf;
        } else if (f.kind == CharKind::Pair) {
            v.is_pair = true;
            v.lo = w.pair_lo;
            v.hi = w.pair_hi;
        } else {
            v.lo = w.intval;
        }
        values[f.label] = v;
    }
    // 2) 校验全部字段(有非法则弹窗并拒绝,整次不写)
    std::vector<std::string> errors;
    for (const auto& f : CharFields()) {
        auto err = ValidateValue(f, values[f.label]);
        if (err) errors.push_back(*err);
    }
    if (!errors.empty()) {
        std::string msg = "以下字段不合法,未保存:\n\n";
        for (const auto& e : errors) msg += e + "\n";
        MsgBox("校验失败", msg, MB_ICONERROR);
        app.SetStatus("校验失败,未保存:" + name + "。");
        return;
    }
    // 3) 校验通过,写入
    int saved = 0;
    for (const auto& f : CharFields()) {
        if (WriteCharField(app.modder.Mem(), base, f, values[f.label])) ++saved;
    }
    app.char_status = "保存完成:" + name + " " + std::to_string(saved) + " 个字段已写入。请在游戏中验证。";
    app.SetStatus("角色属性保存完成:" + name + "。");
    MsgBox("保存完成",
           "请存档后重新读档查看变化，部分属性词条可能因道具属性加成与设定值不同",
           MB_ICONINFORMATION);
}

// 修改金钱按钮:校验后仅写第 1 人 +0x2C。
void OnMoneyApply(App& app) {
    if (!app.modder.Mem().IsOpen() || app.char_addrs.empty()) {
        app.SetStatus("请先初始化并读取属性。");
        return;
    }
    const auto& gf = GlobalFields().front();  // 金钱
    std::string t = Trim(app.money_buf);
    char* end = nullptr;
    long long val = std::strtoll(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != '\0') {
        MsgBox("输入错误", "金钱必须是整数(0 ~ 99999999)。", MB_ICONWARNING);
        return;
    }
    CharValue v;
    v.lo = static_cast<int>(val);
    auto err = ValidateValue(gf, v);
    if (err) {
        MsgBox("输入错误", *err, MB_ICONWARNING);
        return;
    }
    const std::string& first = app.char_names.front();
    WriteCharField(app.modder.Mem(), app.char_addrs[first], gf, v);
    MsgBox("金钱已修改", "修改后丢弃金钱、购买商品、捡起金钱，数额才会更新", MB_ICONINFORMATION);
}

// ---- 无敌 / 一击必杀锁 ----
// 立即把已启用锁的字段写为 9999(对所有队员,不等定时器)。
void LockWriteOnce(App& app) {
    if (!app.modder.Mem().IsOpen() || app.char_addrs.empty()) return;
    if (!app.ck_invincible && !app.ck_kill) return;
    for (const auto& kv : app.char_addrs) {
        uintptr_t base = kv.second;
        try {
            if (app.ck_invincible) {
                app.modder.Mem().WriteUInt32(base + 0x30, 9999);  // 当前生命
                app.modder.Mem().WriteUInt32(base + 0x38, 9999);  // 当前精气
                for (const auto& f : CharFields()) {
                    if (DefLabels().count(f.label) || f.label == "闪避率") {
                        CharValue v;
                        v.lo = 9999;
                        WriteCharField(app.modder.Mem(), base, f, v);
                    }
                }
            }
            if (app.ck_kill) {
                for (const auto& f : CharFields()) {
                    if (AtkLabels().count(f.label)) {
                        CharValue v;
                        v.is_pair = true;
                        v.lo = 9999;
                        v.hi = 9999;
                        WriteCharField(app.modder.Mem(), base, f, v);
                    } else if (f.label == "命中率") {
                        CharValue v;
                        v.lo = 9999;
                        WriteCharField(app.modder.Mem(), base, f, v);
                    }
                }
            }
        } catch (...) {
        }
    }
}

// 无敌模式勾选/取消。
void OnInvincibleToggled(App& app, bool checked) {
    if (checked) {
        // 缓存防御/闪避原值(所有队员)
        app.def_cache.clear();
        for (const auto& kv : app.char_addrs) {
            for (const auto& f : CharFields()) {
                if (DefLabels().count(f.label) || f.label == "闪避率") {
                    auto v = ReadCharField(app.modder.Mem(), kv.second, f);
                    if (v) app.def_cache[kv.first][f.label] = *v;
                }
            }
        }
        LockWriteOnce(app);  // 立即写 9999
    } else {
        // 取消:生命=最大生命、精气=最大精气、防御+闪避=缓存值
        for (const auto& kv : app.char_addrs) {
            uintptr_t base = kv.second;
            try {
                unsigned int mx_hp = app.modder.Mem().ReadUInt32(base + 0x34);
                app.modder.Mem().WriteUInt32(base + 0x30, mx_hp);
                unsigned int mx_mp = app.modder.Mem().ReadUInt32(base + 0x3C);
                app.modder.Mem().WriteUInt32(base + 0x38, mx_mp);
                for (const auto& f : CharFields()) {
                    if (DefLabels().count(f.label) || f.label == "闪避率") {
                        auto cit = app.def_cache.find(kv.first);
                        if (cit != app.def_cache.end()) {
                            auto fit = cit->second.find(f.label);
                            if (fit != cit->second.end())
                                WriteCharField(app.modder.Mem(), base, f, fit->second);
                        }
                    }
                }
            } catch (...) {
            }
        }
        app.def_cache.clear();
    }
}

// 一击必杀勾选/取消。
void OnKillToggled(App& app, bool checked) {
    if (checked) {
        // 缓存攻击/命中率原值(所有队员)
        app.hit_cache.clear();
        for (const auto& kv : app.char_addrs) {
            for (const auto& f : CharFields()) {
                if (AtkLabels().count(f.label) || f.label == "命中率") {
                    auto v = ReadCharField(app.modder.Mem(), kv.second, f);
                    if (v) app.hit_cache[kv.first][f.label] = *v;
                }
            }
        }
        LockWriteOnce(app);  // 立即写 9999
    } else {
        // 取消:攻击/命中率恢复原值
        for (const auto& kv : app.char_addrs) {
            uintptr_t base = kv.second;
            try {
                for (const auto& f : CharFields()) {
                    if (AtkLabels().count(f.label) || f.label == "命中率") {
                        auto cit = app.hit_cache.find(kv.first);
                        if (cit != app.hit_cache.end()) {
                            auto fit = cit->second.find(f.label);
                            if (fit != cit->second.end())
                                WriteCharField(app.modder.Mem(), base, f, fit->second);
                        }
                    }
                }
            } catch (...) {
            }
        }
        app.hit_cache.clear();
    }
}

// 锁定时器:每 500ms 把已启用锁的字段写回 9999(对所有队员)。
void TickLock(App& app, float dt) {
    if (!app.ck_invincible && !app.ck_kill) {
        app.lock_accum = 0.0f;
        return;
    }
    app.lock_accum += dt;
    if (app.lock_accum < 0.5f) return;
    app.lock_accum = 0.0f;
    LockWriteOnce(app);
}

// ============================================================================
// UI 渲染
// ============================================================================

int IndexOf(const std::vector<uint32_t>& ids, uint32_t v) {
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == v) return static_cast<int>(i);
    return -1;
}

void RenderBasicFields(App& app) {
    auto type_val = app.CurrentTypeValue();
    // 使用步骤提示:识别的物品是"上一次操作(选中后放下)的物品",不是光标物品
    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.15f, 1.0f),
                       "使用步骤：第1步：选中箱子中的物品然后放下；第2步：读取属性并修改；第3步：丢弃修改物品再捡起，在箱子里查看修改后的属性；");
    ImGui::Separator();
    const float avail = ImGui::GetContentRegionAvail().x;
    // 网格列数:正常宽度下武器名字/别名 2 列一行、其余 3 列一行;窗口窄时递减。
    const int cols = avail >= 900.0f ? 3 : (avail >= 600.0f ? 2 : 1);
    // 前 2 个字段(武器名字/别名)是否单独用 2 列各占半行(窗口太窄时并入后面表格)。
    const size_t head_n = (cols >= 2 && app.basic_fields.size() >= 2) ? 2 : 0;

    // 渲染单个字段(labels 在上、输入框在下、hint 灰字在输入框下方,输入框填满列宽)。
    auto render_field = [&](size_t i) {
        const auto& f = app.basic_fields[i];
        App::BasicWidget& w = app.basic[i];

        // 条件/强制字段:渲染时即时计算(逻辑对齐 cpp UpdateConditionalFields)
        bool forced = f.force_when && f.IsForced(type_val);
        bool cond_inactive = f.conditional && f.conditional->field == "类型" &&
                             !(type_val && std::find(f.conditional->values.begin(),
                                                     f.conditional->values.end(),
                                                     *type_val) !=
                                                 f.conditional->values.end());
        bool disabled = f.readonly || forced || cond_inactive;

        std::string label = f.label;
        if (f.readonly) label += "（只读）";

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::BeginDisabled(disabled);

        if (f.type == "string_ptr") {
            ImGui::InputText(("##b" + std::to_string(i)).c_str(), w.strbuf,
                             sizeof(w.strbuf), ImGuiInputTextFlags_None);
        } else if (w.is_enum) {
            // 下拉选项:值->名字(enum_display_value 显示 "值：名字")
            std::vector<std::pair<int, std::string>> options;
            for (const auto& kv : f.enum_map) {
                std::string item = f.enum_display_value
                                       ? std::to_string(kv.first) + "：" + kv.second
                                       : kv.second;
                options.emplace_back(kv.first, std::move(item));
            }
            std::vector<const char*> items;
            for (const auto& o : options) items.push_back(o.second.c_str());
            int display = w.enum_val;
            if (forced) display = f.force_when->value;
            int cur = -1;
            for (int j = 0; j < static_cast<int>(options.size()); ++j)
                if (options[static_cast<size_t>(j)].first == display) {
                    cur = j;
                    break;
                }
            if (ImGui::Combo(("##b" + std::to_string(i)).c_str(), &cur, items.data(),
                             static_cast<int>(items.size()))) {
                if (cur >= 0) w.enum_val = options[static_cast<size_t>(cur)].first;
            }
        } else {
            int edit = w.intval;
            if (forced) edit = f.force_when->value;
            else if (cond_inactive) edit = -1;
            if (ImGui::InputInt(("##b" + std::to_string(i)).c_str(), &edit)) w.intval = edit;
        }
        ImGui::EndDisabled();
        if (f.hint) {
            ImGui::TextDisabled("%s", f.hint->c_str());
        }
    };

    // 武器名字/武器别名:2 列,各占半行。
    if (head_n > 0) {
        if (ImGui::BeginTable("basichead", 2)) {
            render_field(0);
            render_field(1);
            ImGui::EndTable();
        }
    }
    // 其余属性:3 列一行(窄窗口退化为 2/1 列)。
    if (app.basic_fields.size() > head_n) {
        if (ImGui::BeginTable("basicrest", cols)) {
            for (size_t i = head_n; i < app.basic_fields.size(); ++i) render_field(i);
            ImGui::EndTable();
        }
    }
}

void RenderAttrTable(App& app, float height) {
    // 按钮行
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    if (ImGui::Button("读取属性")) OnRead(app);
    ImGui::SameLine();
    if (ImGui::Button("从模板读取")) OnLoadTpl(app);
    ImGui::SameLine();
    if (ImGui::Button("保存属性")) OnSave(app);
    ImGui::SameLine();
    if (ImGui::Button("另存为")) OnSaveAs(app);
    ImGui::SameLine();
    if (ImGui::Button("添加")) {
        app.attrs.push_back(ItemProperty());
        app.attr_sel.push_back(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("删除")) {
        for (int r = static_cast<int>(app.attrs.size()) - 1; r >= 0; --r) {
            if (r < static_cast<int>(app.attr_sel.size()) && app.attr_sel[static_cast<size_t>(r)]) {
                app.attrs.erase(app.attrs.begin() + r);
                app.attr_sel.erase(app.attr_sel.begin() + r);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("清空")) {
        app.attrs.clear();
        app.attr_sel.clear();
    }
    ImGui::PopStyleVar();
    ImGui::SameLine();
    ImGui::Text("属性数：%d", static_cast<int>(app.attrs.size()));

    ImGui::Separator();

    std::vector<std::string> name_items = ItemData::AttrDisplayList();
    std::vector<const char*> name_items_c;
    for (const auto& s : name_items) name_items_c.push_back(s.c_str());
    std::vector<const char*> color_items_c;
    for (const auto& kv : ItemData::ColorNames()) color_items_c.push_back(kv.second.c_str());
    std::vector<const char*> percent_items_c;
    for (const auto& kv : ItemData::PercentNames()) percent_items_c.push_back(kv.second.c_str());

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_NoHostExtendY;
    if (ImGui::BeginTable("attrs", 5, flags, ImVec2(0.0f, height))) {
        ImGui::TableSetupColumn("选择", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("属性名", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("属性颜色", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("属性值", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableHeadersRow();

        for (int r = 0; r < static_cast<int>(app.attrs.size()); ++r) {
            ItemProperty& p = app.attrs[static_cast<size_t>(r)];
            ImGui::PushID(r);
            ImGui::TableNextRow();
            if (ImGui::TableSetColumnIndex(0)) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                bool sel = app.attr_sel[static_cast<size_t>(r)];
                if (ImGui::Checkbox("##sel", &sel))
                    app.attr_sel[static_cast<size_t>(r)] = sel;
            }
            if (ImGui::TableSetColumnIndex(1)) {
                int cur = IndexOf(app.attr_ids, p.attr_id);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##name", &cur, name_items_c.data(),
                                 static_cast<int>(name_items_c.size()))) {
                    if (cur >= 0) p.attr_id = app.attr_ids[static_cast<size_t>(cur)];
                }
            }
            if (ImGui::TableSetColumnIndex(2)) {
                int cur = IndexOf(app.color_ids, p.color);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##color", &cur, color_items_c.data(),
                                 static_cast<int>(color_items_c.size()))) {
                    if (cur >= 0) p.color = app.color_ids[static_cast<size_t>(cur)];
                }
            }
            if (ImGui::TableSetColumnIndex(3)) {
                int cur = IndexOf(app.percent_ids, p.precent);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##prec", &cur, percent_items_c.data(),
                                 static_cast<int>(percent_items_c.size()))) {
                    if (cur >= 0) p.precent = app.percent_ids[static_cast<size_t>(cur)];
                }
            }
            if (ImGui::TableSetColumnIndex(4)) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputFloat("##val", &p.value, 0.0f, 0.0f, "%.6g");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

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
    ImGui::InputTextMultiline("##hex", app.raw_hex, sizeof(app.raw_hex),
                              ImVec2(-FLT_MIN, avail_y), ImGuiInputTextFlags_None);
    ImGui::TextDisabled("查看拾取物品指针附近内存。写入:每行 \"地址: 十六进制字节\" 或纯字节均可。");
}

void RenderCfgTab(App& app) {
    // 模式选择
    std::vector<std::string> mode_labels;
    for (const auto& kv : app.cfg.modes)
        mode_labels.push_back(kv.first + "：" + kv.second.name);
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
    row("重量偏移 (hook.weight_offset, 0x...)", app.cfg_weight_offset, sizeof(app.cfg_weight_offset));
    row("重量上限 (hook.weight_max)", app.cfg_weight_max, sizeof(app.cfg_weight_max));
    row("物品基址调整 (item_base_adjust)", app.cfg_item_adjust, sizeof(app.cfg_item_adjust));
    row("属性头偏移 (attr_list.head_offset, 0x...)", app.cfg_head, sizeof(app.cfg_head));
    row("节点 next 偏移 (0x...)", app.cfg_next, sizeof(app.cfg_next));
    row("节点 ID 偏移 (0x...)", app.cfg_id, sizeof(app.cfg_id));
    row("节点 color 偏移 (0x...)", app.cfg_color, sizeof(app.cfg_color));
    row("节点 precent 偏移 (0x...)", app.cfg_precent, sizeof(app.cfg_precent));
    row("节点 value 偏移 (0x...)", app.cfg_value, sizeof(app.cfg_value));
    row("节点大小 (attr_list.node_size, 0x...)", app.cfg_node_size, sizeof(app.cfg_node_size));
    row("最大节点数 (attr_list.max_nodes)", app.cfg_max_nodes, sizeof(app.cfg_max_nodes));

    ImGui::Separator();
    if (ImGui::Button("保存配置并应用")) OnSaveCfg(app);
    ImGui::TextDisabled(
        "说明:以上为当前模式的 magic number,可在 poq 不同版本间切换。修改后点击\"保存配置并应用\""
        "立即生效,但仅本次会话有效(单文件版不写配置文件,重启回到内置默认)。"
        "内置默认在 src/Core/OffsetsConfig.cpp 的 DefaultMode() 中维护(按 CT),"
        "需要固定偏移时改代码重新编译。");
}

// ============================================================================
// 召唤修改 Tab(task7)
// ============================================================================
// 版本选择复用顶部全局选择器;steam/v1.30 两套召唤逻辑分别调用
// summon_steam / summon_130 独立模块(文件级隔离,不做配置耦合,对齐 python
// summon_tab.py + data/py/summon_tool_{steam,130}.py)。本文件只做 UI 编排。

// 页面顶部说明(task7 #5,逐字保留)
const char* kSummonTip =
    "说明：\n"
    "1. steam版本支持较好，v1.30只有部分功能;\n"
    "1.1. steam版本可以召唤更多具体类别，攻击力普遍更强，刺客除外攻击力弱；\n"
    "1.2. v1.30不支持召唤朱厌、善翼、刺客、指定类型的秦兵，且所有召唤攻击力弱，"
    "无法造成伤害，只能拉仇恨，但秦兵死亡有物品掉落；\n"
    "2. 点击激活后，力士使用“烽火狼烟”，就能召唤对应怪兽；\n"
    "3. 激活状态下再点击“取消”按钮，状态恢复到烽火狼烟默认值，召唤狼；\n"
    "4. 召唤太多队友，需要通过GM命令的look，找到队友对应ID，然后通过“kill {ID}”来取消，"
    "所以不要召唤太多，尤其是steam版本，因为队友比较强不容易挂；\n"
    "5. 召唤太多盟友可以通过GM命令kill all来清除，但这个可能会杀掉一些无辜NPC，甚至破坏剧情，"
    "建议和上一条一样一个一个清理；\n"
    "6. 执行4、5前可以保存并备份“System\\game\\player_{i}.gam”以防万一；\n"
    "7. 读档后需要“重新激活”或“刷新召唤”；";

// UI 数据镜像(对齐 python summon_tab.py: 下拉文案含序号,子类名与工具模块一致)。
const char* kSummonMonNames[10] = {"狼",   "虎",   "熊", "白狼", "木偶",
                                   "狗",   "虎蛟", "狍鸮", "朱厌", "善翼"};
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
    if (app.cfg.current_mode == "mode1")
        return summon_steam::CmdSummon(*mem, *base_opt, args);
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
                const SummonCatUi& cat =
                    SummonQinCats()[static_cast<size_t>(app.sm_soldier - 1)];
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
    if (ImGui::Combo("##sm_sub", &sub_sel, sub_items.data(),
                     static_cast<int>(sub_items.size())) &&
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

    // 第一行:说明(橙色,风格同其他 tab 的使用步骤提示)
    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.15f, 1.0f), "%s", kSummonTip);
    ImGui::Separator();

    // 第二行:激活状态标签(左) + 激活/取消 + 刷新召唤(右)
    // 激活时:"激活状态：激活，当前召唤的怪物：<类别>，<阵营>";阵营=队友(group 0)/盟友(group 1)
    std::string status_text = app.summon_active
                                  ? "激活状态：激活，当前召唤的怪物：" + SummonCurrentSelection(app) +
                                        "，" + (app.sm_group == 0 ? "队友" : "盟友")
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

// ============================================================================
// GM 命令 Tab(task8)
// ============================================================================
// 版本选择复用顶部全局选择器;steam/v1.30 的区别集中在 Core/GmCmd.h 的
// GmVersion 常量(对齐 python gm_cmd.py 的"单模块双版本参数化"),本文件只做
// UI 编排。纯 C++ 实现,不调用 python。

// 页面顶部说明(用户改版:去掉 look/dir/object 条目,条目重新编号 1~4)
const char* kGmTip =
    "使用说明：\n"
    "手动激活GM的方法：\n"
    "1. 在“游戏目录\\System\\wizard\\wizard.lst”里加入“9 {用户名}”，"
    "单机用户名为player，多人为自己设定名字，文件或目录不存在可以自行创建；\n"
    "2. 完成上一步后，重启游戏，在游戏中按回车依次输入“{$objectpoq$}”和“r 9”后，"
    "屏幕会出现激活提示；\n"
    "本工具使用方法：\n"
    "1. 使用前需要按激活按钮激活GM权限，如果一种方式无效，可以尝试另外一种，或者手动"
    "（程序会静默建立wizard.lst文件，并加入当前用户）；\n"
    "2. 激活后根据网上攻略尝试激活命令并执行即可，工具内可以无视GM命令长度限制，"
    "但过长是否会崩溃没有试过；\n"
    "3. 命令可以从网络获取，GM很多技巧可探索：比如杀死杂货铺掌柜或者铁匠铺掌柜或得装备"
    "（记得杀完后用创建命令恢复，名字可以用look看到）；\n"
    "4. 常用命令可以放到同目录下gm_cmd.txt中用于快速输入，用户可以自行编辑文档，"
    "格式在文档中已给出；";

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
    if (app.gm_cmdlist_sel >= static_cast<int>(app.gm_cmd_list.size()))
        app.gm_cmdlist_sel = 0;
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
        if (!sel)
            throw std::runtime_error("未选择进程：请先在顶部选择 POQ.EXE 并初始化。");
        pid = *sel;
        if (!tmp_mem.Open(pid))
            throw std::runtime_error("OpenProcess 失败（请以管理员身份运行）。");
        mem = &tmp_mem;
    }
    ver = (app.cfg.current_mode == "mode1") ? &gm_cmd::SteamVersion()
                                            : &gm_cmd::V130Version();
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
        gm_cmd::Method method =
            (app.gm_method == 1) ? gm_cmd::Method::R9 : gm_cmd::Method::Memory;
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

    // 第1部分:使用说明(橙色,自动换行)
    const float avail_w = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(avail_w - 10.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.15f, 1.0f), "%s", kGmTip);
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    // 第2部分:激活状态行(左) + 激活方式 + 激活/还原(右)
    std::string status_text = app.gm_active ? "GM权限已激活" : "GM权限未激活";
    const ImVec4 status_color =
        app.gm_active ? ImVec4(0.0f, 0.8f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float row_w = ImGui::GetContentRegionAvail().x;
    // 三个按钮(激活/还原、执行、读取配置)统一宽度,统一右对齐
    const float gm_btn_w = ImGui::CalcTextSize("读取配置").x +
                           ImGui::GetStyle().FramePadding.x * 2.0f;
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
            app.SetStatus("已重新读取常用命令 (" + std::to_string(app.gm_cmd_list.size()) +
                          " 条)");
            AppendGmLog(app, "读取配置：已重新读取 " +
                                 std::to_string(app.gm_cmd_list.size()) + " 条常用命令");
        }
    }
    // 下方:展开全部选项(自动限高,超出滚动);点选 -> 命令输入框显示对应命令
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

// ============================================================================
// H2 加解密(task10): H2 数据文件原地加解密。
//   加密态: 首字节 0x80, 其余所有字节 ^0x78; 明文: GBK 编码, tab 分隔 csv。
//   解密: 首字节 0x80 则去掉后其余 ^0x78; 加密: 全部 ^0x78 后开头补 0x80。
//   转换前先备份原文件为 "<原名>.dec.bak"(解密) / "<原名>.enc.bak"(加密)。
// ============================================================================
const char* kH2Tip =
    "使用说明：\n"
    "H2加解密工具：\n"
    "1. 在“游戏目录\\Data\\”存放加密过的游戏参数，例如物品爆率等；\n"
    "2. 解密后的文件是GBK编码，请使用Excel打开、编辑、保存；\n"
    "3. 解密过的文件将不再出现乱码，修改请参考照论坛或者其他教程；\n"
    "4. 需要退出游戏重新进入后生效（存档+读档未确定）；\n"
    "4. 程序在解密和加密前会在原目录备份文件并加上“.dec.bak”或“.env.bak”后缀，该错了可手动恢复；";

std::wstring H2BackupPath(const std::wstring& path, bool decrypt) {
    return path + (decrypt ? L".dec.bak" : L".enc.bak");
}

// 解密:首字节 0x80 则去掉后其余 ^0x78;否则视为已明文原样返回(python decrypt_bytes 同语义)。
std::vector<unsigned char> H2DecryptBytes(const std::vector<unsigned char>& d) {
    if (d.size() < 1 || d[0] != 0x80) return d;
    std::vector<unsigned char> out;
    out.reserve(d.size() - 1);
    for (size_t i = 1; i < d.size(); ++i)
        out.push_back(static_cast<unsigned char>(d[i] ^ 0x78));
    return out;
}
// 加密:全部 ^0x78 后开头补 0x80。
std::vector<unsigned char> H2EncryptBytes(const std::vector<unsigned char>& d) {
    std::vector<unsigned char> out;
    out.reserve(d.size() + 1);
    out.push_back(0x80);
    for (unsigned char c : d) out.push_back(static_cast<unsigned char>(c ^ 0x78));
    return out;
}

// 读取整个文件;失败返回 false。
bool H2ReadFile(const std::wstring& path, std::vector<unsigned char>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

// 磁盘备份原文件(src -> dst,已存在则覆盖)。
bool H2CopyFile(const std::wstring& src, const std::wstring& dst) {
    return ::CopyFileW(src.c_str(), dst.c_str(), /*bFailIfExists=*/FALSE) != 0;
}

// 原地转换: 读原文件 -> 内存转换 -> 写同目录临时文件 -> MoveFileEx 原子替换原文件。
// 任意一步失败都保证原文件未更改(只读原文件,替换失败只删临时文件)。
bool H2ConvertFile(const std::wstring& path, bool decrypt) {
    std::vector<unsigned char> raw;
    if (!H2ReadFile(path, raw)) return false;
    std::vector<unsigned char> out = decrypt ? H2DecryptBytes(raw) : H2EncryptBytes(raw);
    std::wstring tmp = path + L".h2tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool wrote_ok = out.empty() || fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    if (!wrote_ok) {
        _wremove(tmp.c_str());
        return false;
    }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        _wremove(tmp.c_str());
        return false;
    }
    return true;
}

// 执行转换(make_backup=true 时先备份原文件为 .dec/.enc.bak)。
void H2DoConvert(App& app, const std::wstring& path, bool decrypt, bool make_backup) {
    if (make_backup) {
        if (!H2CopyFile(path, H2BackupPath(path, decrypt))) {
            MsgBox("转换失败", "备份文件创建失败，文件未更改，请检查文件", MB_ICONERROR);
            return;
        }
    }
    if (!H2ConvertFile(path, decrypt)) {
        MsgBox("转换失败", "转换失败，文件未更改，请检查文件", MB_ICONERROR);
        return;
    }
    const char* ok = decrypt ? "解密成功，请用Excel编辑属性" : "加密成功，文件已恢复到乱码状态";
    MsgBox("转换成功", ok, MB_ICONINFORMATION);
    app.SetStatus(decrypt ? "H2 解密成功" : "H2 加密成功");
}

// 文件选择框初始目录:已连接/枚举到 poq.exe 时用其所在目录,否则工具所在目录。
std::wstring H2InitialDir(App& app) {
    std::optional<unsigned int> pid;
    if (app.modder.Mem().IsOpen())
        pid = app.modder.Pid();
    else if (auto s = TryGetSelectedPid(app))
        pid = s;
    if (!pid) pid = FindPoqPid(app);
    if (pid) {
        auto path = poq::ProcessMemory::FindModulePath(*pid, "POQ.exe");
        if (path) {
            std::string p = *path;
            size_t pos = p.find_last_of("/\\");
            if (pos != std::string::npos) {
                std::wstring dir = Utf8ToWide(p.substr(0, pos));
                if (!dir.empty() && ::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return dir;
            }
        }
    }
    return AppDirW();
}

// 选择文件后的统一入口:有备份 -> 弹三选一;否则直接转换。
void H2BeginConvert(App& app, const std::wstring& path, bool decrypt) {
    std::wstring bak = H2BackupPath(path, decrypt);
    if (::GetFileAttributesW(bak.c_str()) != INVALID_FILE_ATTRIBUTES) {
        app.h2_path = path;
        app.h2_decrypt = decrypt;
        app.h2_wait_confirm = true;
        return;
    }
    H2DoConvert(app, path, decrypt, /*make_backup=*/true);
}

// 点击解密/加密按钮:弹文件选择框,选好即转换。
void H2OnConvert(App& app, bool decrypt) {
    std::string sel = OpenFileDialog(H2InitialDir(app), L"所有文件 (*)\0*.*\0\0");
    if (sel.empty()) return;  // 用户取消
    H2BeginConvert(app, Utf8ToWide(sel), decrypt);
}

// 已有备份文件的三选一弹窗(覆盖备份转换 / 跳过备份转换 / 取消转换)。
void H2RenderBackupConfirm(App& app) {
    const char* popup_id = "备份已存在##h2bak";
    if (!ImGui::IsPopupOpen(popup_id)) ImGui::OpenPopup(popup_id);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(popup_id, nullptr,
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse)) {
        const float win_w = ImGui::GetWindowSize().x;
        ImGui::PushTextWrapPos(win_w - 40.0f);
        ImGui::TextUnformatted("已存在备份文件，请选择：");
        ImGui::TextUnformatted(WideToUtf8(H2BackupPath(app.h2_path, app.h2_decrypt)).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14));
        const float btn_w = 150.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float total = btn_w * 3.0f + spacing * 2.0f;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - total) * 0.5f);
        if (ImGui::Button("覆盖备份转换", ImVec2(btn_w, 0))) {
            H2DoConvert(app, app.h2_path, app.h2_decrypt, /*make_backup=*/true);
            app.h2_wait_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("跳过备份转换", ImVec2(btn_w, 0))) {
            H2DoConvert(app, app.h2_path, app.h2_decrypt, /*make_backup=*/false);
            app.h2_wait_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消转换", ImVec2(btn_w, 0))) {
            app.h2_wait_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// 目录选择回调:设置初始选中目录。
static int CALLBACK H2BrowseCallback(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData) {
    if (uMsg == BFFM_INITIALIZED && lpData)
        ::SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, lpData);  // lpData = 初始目录宽串
    return 0;
}

// 目录选择框(SHBrowseForFolderW,自带 COM 处理,无需预初始化)。
std::wstring H2PickFolderDialog(const std::wstring& dir) {
    wchar_t disp[MAX_PATH]{};
    BROWSEINFOW bi{};
    bi.hwndOwner = nullptr;
    bi.pidlRoot = nullptr;
    bi.pszDisplayName = disp;
    bi.lpszTitle = L"选择要批量加解密的目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = H2BrowseCallback;
    bi.lParam = reinterpret_cast<LPARAM>(dir.c_str());
    PIDLIST_ABSOLUTE pidl = ::SHBrowseForFolderW(&bi);
    if (!pidl) return L"";  // 用户取消
    wchar_t path[MAX_PATH]{};
    bool ok = ::SHGetPathFromIDListW(pidl, path) != FALSE;
    ::CoTaskMemFree(pidl);
    if (!ok) return L"";
    std::wstring s(path);
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();  // 去末尾反斜杠(根目录除外)
    return s;
}

// 批量结果统计。
struct H2BatchResult {
    int ok = 0;       // 成功转换
    int skipped = 0;  // 跳过(方向已不符)
    int failed = 0;   // 失败(原文件未更改)
};

// 批量转换:递归目录(含子文件夹)内所有 .txt 文件。每个文件独立备份(已有备份则覆盖)。
// 解密只处理加密态(0x80 开头)文件,加密只处理明文文件,避免重复转换破坏数据。
H2BatchResult H2BatchConvert(const std::wstring& dir, bool decrypt) {
    H2BatchResult r;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& p = it->path();
        if (!it->is_regular_file(ec)) continue;
        if (_wcsicmp(p.extension().c_str(), L".txt") != 0) continue;  // 仅 txt 文件
        std::wstring wp = p.wstring();
        std::vector<unsigned char> raw;
        if (!H2ReadFile(wp, raw)) { r.failed++; continue; }
        bool is_enc = !raw.empty() && raw[0] == 0x80;
        if (decrypt ? !is_enc : is_enc) { r.skipped++; continue; }  // 方向不符,跳过
        if (!H2CopyFile(wp, H2BackupPath(wp, decrypt))) { r.failed++; continue; }
        if (!H2ConvertFile(wp, decrypt)) { r.failed++; continue; }
        r.ok++;
    }
    return r;
}

// 目录转换确认后执行,结果弹统计。
void H2RunBatch(App& app, const std::wstring& dir, bool decrypt) {
    H2BatchResult r = H2BatchConvert(dir, decrypt);
    std::string msg = (decrypt ? "解密" : "加密") + std::to_string(r.ok) + " 个文件";
    if (r.skipped > 0) msg += "，跳过 " + std::to_string(r.skipped) + " 个";
    if (r.failed > 0) msg += "，失败 " + std::to_string(r.failed) + " 个（原文件未更改）";
    if (r.failed > 0)
        MsgBox("目录转换", msg + "。", MB_ICONWARNING);
    else
        MsgBox("目录转换", msg + "。", MB_ICONINFORMATION);
    app.SetStatus((decrypt ? "H2 目录解密：" : "H2 目录加密：") + msg);
}

// 目录按钮点击:弹目录选择框,选好进确认弹窗。
void H2OnBatch(App& app, bool decrypt) {
    std::wstring dir = H2PickFolderDialog(H2InitialDir(app));
    if (dir.empty()) return;  // 用户取消
    app.h2_dir = dir;
    app.h2_dir_decrypt = decrypt;
    app.h2_wait_dir_confirm = true;
}

// 目录转换确认弹窗(提示会转换整个目录含子文件夹的 txt 文件,请做好备份)。
void H2RenderDirConfirm(App& app) {
    const char* popup_id = "目录转换确认##h2dir";
    if (!ImGui::IsPopupOpen(popup_id)) ImGui::OpenPopup(popup_id);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(popup_id, nullptr,
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse)) {
        const float win_w = ImGui::GetWindowSize().x;
        ImGui::PushTextWrapPos(win_w - 40.0f);
        ImGui::TextUnformatted("将会转换整个目录（包括子文件夹）中的 txt 文件，请做好备份！");
        ImGui::TextUnformatted(
            app.h2_dir_decrypt ? "已是明文（解密态）的文件将自动跳过。"
                               : "已是加密态的文件将自动跳过。");
        ImGui::TextWrapped("目录：%s", WideToUtf8(app.h2_dir).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14));
        const float btn_w = 140.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float total = btn_w * 2.0f + spacing;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - total) * 0.5f);
        if (ImGui::Button("开始转换", ImVec2(btn_w, 0))) {
            H2RunBatch(app, app.h2_dir, app.h2_dir_decrypt);
            app.h2_wait_dir_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(btn_w, 0))) {
            app.h2_wait_dir_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// H2解密 tab 主渲染(对齐 GM tab 顶部说明样式)。
void RenderH2Tab(App& app) {
    const float avail_w = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(avail_w - 10.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.15f, 1.0f), "%s", kH2Tip);
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    const float btn_w =
        ImGui::CalcTextSize("解密目录").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::TextDisabled("单个文件：");
    if (ImGui::Button("解密", ImVec2(btn_w, 0))) H2OnConvert(app, /*decrypt=*/true);
    ImGui::SameLine();
    if (ImGui::Button("加密", ImVec2(btn_w, 0))) H2OnConvert(app, /*decrypt=*/false);
    ImGui::TextDisabled("整个目录（含子文件夹）：");
    if (ImGui::Button("解密目录", ImVec2(btn_w, 0))) H2OnBatch(app, /*decrypt=*/true);
    ImGui::SameLine();
    if (ImGui::Button("加密目录", ImVec2(btn_w, 0))) H2OnBatch(app, /*decrypt=*/false);

    // 已有备份 -> 三选一弹窗
    if (app.h2_wait_confirm) H2RenderBackupConfirm(app);
    // 目录确认 -> 二选弹窗
    if (app.h2_wait_dir_confirm) H2RenderDirConfirm(app);
}

// ============================================================================
// 版本号:日期部分从编译期 __DATE__("Mmm dd yyyy")生成 YYMMDD,每次编译自动取当天;
// 后缀 "v1"(或 v2/v3...)为手动版本号,由用户指定后改这里。
// 启动弹窗显示形如 "Ver: 260826v1"。
// ============================================================================
constexpr int kVerMonth() {
    constexpr const char* names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int m = 1;
    for (int i = 0; i < 12; ++i) {
        bool eq = true;
        for (int j = 0; j < 3; ++j)
            if (__DATE__[j] != names[i][j]) { eq = false; break; }
        if (eq) { m = i + 1; break; }
    }
    return m;
}
constexpr int kVerYear2() { return (__DATE__[9] - '0') * 10 + (__DATE__[10] - '0'); }
constexpr int kVerDay() {
    int d10 = (__DATE__[4] == ' ') ? 0 : (__DATE__[4] - '0');
    return d10 * 10 + (__DATE__[5] - '0');
}
const char* kVersionText() {
    static char buf[32];
    std::snprintf(buf, sizeof(buf), "Ver: %02d%02d%02dv1", kVerYear2(), kVerMonth(), kVerDay());
    return buf;
}

void RenderWelcomeDialog(App& app) {
    // 每次启动显示的免责声明弹窗:居中,底部中间"确认"关闭。
    if (!app.show_welcome) return;
    if (!ImGui::IsPopupOpen("关于")) ImGui::OpenPopup("关于");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 280), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("关于", nullptr,
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse)) {
        const float win_w = ImGui::GetWindowSize().x;
        const float win_h = ImGui::GetWindowSize().y;
        const char* kLines[] = {
            "本软件仅供娱乐研究，严禁售卖牟利，一切后果与作者无关。",
            "唯一发布途径：百度贴吧",
            "作者：潜移默化的影响",
            "鸣谢：B站up口花生口",
        };
        // 文本区(四行)居中显示,垂直大致居中
        float y = win_h * 0.14f;
        const float line_h = ImGui::GetTextLineHeightWithSpacing();
        for (const char* line : kLines) {
            float tw = ImGui::CalcTextSize(line).x;
            ImGui::SetCursorPos(ImVec2((win_w - tw) * 0.5f, y));
            ImGui::TextUnformatted(line);
            y += line_h;
        }
        // 版本行:与上方内容空一行区分,同样居中(日期由编译期 __DATE__ 自动取当天)
        y += line_h;  // 空一行
        const char* ver = kVersionText();
        float tw = ImGui::CalcTextSize(ver).x;
        ImGui::SetCursorPos(ImVec2((win_w - tw) * 0.5f, y));
        ImGui::TextUnformatted(ver);
        // 底部中间"确认"按钮
        const float btn_w = 120.0f;
        const float btn_h = 30.0f;
        ImGui::SetCursorPos(ImVec2((win_w - btn_w) * 0.5f, win_h - btn_h - 20.0f));
        if (ImGui::Button("确认", ImVec2(btn_w, btn_h))) {
            app.show_welcome = false;
            ImGui::CloseCurrentPopup();
        }
        // Esc 也可关闭(且不再弹)
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            app.show_welcome = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// 渲染待处理的消息弹窗(替代系统 MessageBox,样式与启动免责声明弹窗一致)。
// 多个消息排队:一次弹一个,点"确认"后显示下一个。
void RenderMsgQueue() {
    if (g_msgbox_queue.empty()) return;
    const MessageBoxMsg& m = g_msgbox_queue.front();
    // 标题栏文字 = 类型(提示/错误/警告),与"关于"弹窗标题栏对齐;
    // "##msgbox" 使窗口 ID 固定,不随类型变化。
    const char* kType = "提示";
    if (m.type == MB_ICONERROR) {
        kType = "错误";
    } else if (m.type == MB_ICONWARNING) {
        kType = "警告";
    }
    std::string popup_id = std::string(kType) + "##msgbox";
    if (!ImGui::IsPopupOpen(popup_id.c_str())) ImGui::OpenPopup(popup_id.c_str());
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);  // 固定宽,高随内容
    if (ImGui::BeginPopupModal(popup_id.c_str(), nullptr,
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse)) {
        const float win_w = ImGui::GetWindowSize().x;
        // 内容(标题 + 逗号 + 正文,自动换行)
        std::string full = m.title + "，" + m.text;
        ImGui::PushTextWrapPos(win_w - 40.0f);
        ImGui::TextUnformatted(full.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 16));
        // 底部中间"确认"按钮
        const float btn_w = 120.0f;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - btn_w) * 0.5f);
        if (ImGui::Button("确认", ImVec2(btn_w, 30)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_msgbox_queue.erase(g_msgbox_queue.begin());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// 属性修改(角色)Tab:全局属性 + 角色选择 + 字段网格(task6 指定排列)。
void RenderCharTab(App& app) {
    // 使用步骤提示:角色读取用固定偏移定位,需先选择人物并读取
    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.15f, 1.0f),
                       "使用步骤：第1步：选择修改人物并读取属性；第2步：修改个人属性；第3步：存档后读档查看属性；");
    ImGui::Separator();
    // ---- 全局属性(队伍共享,置顶)----
    ImGui::TextUnformatted("全局属性");
    ImGui::Separator();
    ImGui::TextUnformatted("金钱:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputText("##money", app.money_buf, sizeof(app.money_buf));
    ImGui::SameLine();
    if (ImGui::Button("修改金钱")) OnMoneyApply(app);
    ImGui::SameLine();
    bool inv = app.ck_invincible;
    if (ImGui::Checkbox("无敌模式", &inv)) {
        app.ck_invincible = inv;
        OnInvincibleToggled(app, inv);
    }
    ImGui::SameLine();
    bool kill = app.ck_kill;
    if (ImGui::Checkbox("一击必杀", &kill)) {
        app.ck_kill = kill;
        OnKillToggled(app, kill);
    }

    // ---- 角色选择 + 读取/保存 ----
    ImGui::Separator();
    ImGui::TextUnformatted("角色:");
    ImGui::SameLine();
    std::vector<std::string> char_labels;
    for (size_t i = 0; i < app.char_names.size(); ++i)
        char_labels.push_back("角色" + std::to_string(i + 1) + "-" + app.char_names[i]);
    std::vector<const char*> char_items;
    for (const auto& l : char_labels) char_items.push_back(l.c_str());
    int csel = app.char_selected;
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("##char", &csel, char_items.data(), static_cast<int>(char_items.size()))) {
        if (csel != app.char_selected && csel >= 0 &&
            static_cast<size_t>(csel) < app.char_names.size()) {
            app.char_selected = csel;
            RebuildCharWidgets(app);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("读取属性")) OnCharRead(app);
    ImGui::SameLine();
    if (ImGui::Button("保存属性")) OnCharSave(app);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", app.char_status.c_str());

    ImGui::Separator();

    // ---- 字段网格(task6 指定排列:3 列行 + 2 列行,各行子项等宽)----
    static const char* kLayout[][3] = {
        {"姓名", "别名", "职业"},
        {"年龄", "性别", "等级"},
        {"经验", "下一级", "声望"},
        {"生命", "精气", nullptr},
        {"力量", "体质", nullptr},
        {"敏捷", "悟性", nullptr},
        {"魅力", "智慧", nullptr},
        {"普通攻击", "普通防御", nullptr},
        {"金攻击", "金防御", nullptr},
        {"木攻击", "木防御", nullptr},
        {"水攻击", "水防御", nullptr},
        {"火攻击", "火防御", nullptr},
        {"土攻击", "土防御", nullptr},
        {"闪避率", "命中率", nullptr},
        {"剩余属性点", "剩余技能点", nullptr},
    };

    // 渲染一个字段(标签在上,输入框在下,填满列宽;pair 两个输入框 + "/")。
    auto render_field = [&](const char* label) {
        int idx = FindCharField(label);
        if (idx < 0) {
            ImGui::TextDisabled("%s(未定义)", label);
            return;
        }
        const auto& f = CharFields()[static_cast<size_t>(idx)];
        CharWidget& w = app.char_widgets[static_cast<size_t>(idx)];
        bool disabled = app.char_addrs.empty() || IsCharFieldLocked(app, label);
        ImGui::TextUnformatted(label);
        ImGui::BeginDisabled(disabled);
        std::string id = "##ch" + std::to_string(idx);
        if (f.kind == CharKind::StringPtr) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText(id.c_str(), w.strbuf, sizeof(w.strbuf));
        } else if (f.kind == CharKind::Enum) {
            std::vector<std::pair<int, std::string>> opts;
            for (const auto& kv : f.enum_map)
                opts.emplace_back(kv.first, std::to_string(kv.first) + "：" + kv.second);
            std::vector<const char*> items;
            for (const auto& o : opts) items.push_back(o.second.c_str());
            int cur = -1;
            for (int j = 0; j < static_cast<int>(opts.size()); ++j)
                if (opts[static_cast<size_t>(j)].first == w.intval) {
                    cur = j;
                    break;
                }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo(id.c_str(), &cur, items.data(), static_cast<int>(items.size()))) {
                if (cur >= 0) w.intval = opts[static_cast<size_t>(cur)].first;
            }
        } else if (f.kind == CharKind::Pair) {
            float avail = ImGui::GetContentRegionAvail().x;
            float slash = ImGui::CalcTextSize("/").x + ImGui::GetStyle().ItemSpacing.x;
            float each = (avail - slash) * 0.5f;
            if (each < 30.0f) each = 30.0f;
            ImGui::SetNextItemWidth(each);
            ImGui::InputInt(("##chl" + std::to_string(idx)).c_str(), &w.pair_lo, 0, 0);
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted("/");
            ImGui::SameLine(0, 0);
            ImGui::SetNextItemWidth(each);
            ImGui::InputInt(("##chh" + std::to_string(idx)).c_str(), &w.pair_hi, 0, 0);
        } else {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputInt(id.c_str(), &w.intval, 0, 0);
        }
        ImGui::EndDisabled();
        if (f.hint) ImGui::TextDisabled("%s", f.hint->c_str());
    };

    float grid_h = ImGui::GetContentRegionAvail().y;
    if (grid_h < 80.0f) grid_h = 80.0f;
    ImGui::BeginChild("charfields", ImVec2(-FLT_MIN, grid_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_None);
    for (size_t r = 0; r < sizeof(kLayout) / sizeof(kLayout[0]); ++r) {
        int ncols = kLayout[r][2] ? 3 : 2;
        char tid[32];
        std::snprintf(tid, sizeof(tid), "charrow%zu", r);
        if (ImGui::BeginTable(tid, ncols)) {
            for (int c = 0; c < ncols; ++c)
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            for (int c = 0; c < ncols; ++c) {
                if (ImGui::TableSetColumnIndex(c)) render_field(kLayout[r][c]);
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void RenderTopBar(App& app) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float frame_pad_x = style.FramePadding.x * 2.0f;

    // 右侧按钮组总宽(刷新进程 + 初始化,用于让下拉框占满剩余宽度并右对齐)
    const char* kRefresh = "刷新进程";
    const char* kInit = "初始化";
    const float refresh_w = ImGui::CalcTextSize(kRefresh).x + frame_pad_x;
    const float init_w = ImGui::CalcTextSize(kInit).x + frame_pad_x;
    const float buttons_w = refresh_w + spacing + init_w;

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
    float proc_w = row_w - ver_label_w - ver_w - proc_label_w - buttons_w - spacing * 5.0f;
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
                try {
                    app.modder.Close();  // 还原旧模式注入,避免残留 hook
                } catch (...) {
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
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings);

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
        if (ImGui::BeginTabItem("属性修改")) {
            RenderCharTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("物品修改")) {
            RenderBasicFields(app);
            ImGui::Separator();
            float table_h = ImGui::GetContentRegionAvail().y;
            if (table_h < 60.0f) table_h = 60.0f;
            RenderAttrTable(app, table_h);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("召唤修改")) {
            RenderSummonTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("GM命令")) {
            RenderGmTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("H2解密")) {
            RenderH2Tab(app);
            ImGui::EndTabItem();
        }
        if (!app.hidden && ImGui::BeginTabItem("内存查看")) {
            RenderMemTab(app);
            ImGui::EndTabItem();
        }
        if (!app.hidden && ImGui::BeginTabItem("偏移设置")) {
            RenderCfgTab(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // 状态栏
    ImGui::Separator();
    ImGui::TextUnformatted(app.status.c_str());

    ImGui::End();
}

// ============================================================================
// DirectX 11 样板(抄自 imgui example_win32_directx11,含 WARP 软渲染回退)
// ============================================================================

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel,
        &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)  // 无 GPU 驱动时回退 WARP 软件渲染
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2,
            D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel,
            &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // 禁用 ALT 菜单
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

ImFont* LoadChineseFont(float dpi_scale, bool bold) {
    ImGuiIO& io = ImGui::GetIO();
    static const char* kRegular[] = {
        "C:/Windows/Fonts/msyh.ttc",   // 微软雅黑
        "C:/Windows/Fonts/simhei.ttf",  // 黑体
        "C:/Windows/Fonts/simsun.ttc",  // 宋体
        "C:/Windows/Fonts/simsunb.ttf",
    };
    static const char* kBold[] = {
        "C:/Windows/Fonts/msyhbd.ttc",  // 微软雅黑 粗体
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsunb.ttf",
        "C:/Windows/Fonts/simsun.ttc",
    };
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();
    const float size = 16.0f * dpi_scale;
    const char* const* cands = bold ? kBold : kRegular;
    const int count = bold ? static_cast<int>(sizeof(kBold) / sizeof(kBold[0]))
                           : static_cast<int>(sizeof(kRegular) / sizeof(kRegular[0]));
    for (int i = 0; i < count; ++i) {
        ImFontConfig cfg;
        cfg.FontNo = 0;  // ttc 集合内第 0 个
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        ImFont* f = io.Fonts->AddFontFromFileTTF(cands[i], size, &cfg, ranges);
        if (f) return f;
    }
    return nullptr;
}

// ============================================================================
// 入口(GUI 子系统,无控制台窗口)
// ============================================================================

int RunApp(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // 任务栏使用自定义图标(与 exe 图标一致)
    ::SetCurrentProcessExplicitAppUserModelID(L"PoqEditor");

    // DPI 感知 + 主显示器缩放
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale =
        ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    // 创建窗口(图标来自 .rc 内嵌资源)
    HICON hIcon = ::LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
                      hIcon, nullptr, nullptr, nullptr, L"PoqEditorImgui", nullptr};
    wc.hIconSm = hIcon;
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"秦殇修改器",
                                WS_OVERLAPPEDWINDOW, 100, 100,
                                (int)(1100 * main_scale), (int)(720 * main_scale),
                                nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        ::MessageBoxW(nullptr, L"DirectX 11 初始化失败(硬件/WARP 都不可用)。", L"PoqEditor", MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // imgui 初始化
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyBlueTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 中文字体(regular + bold)
    ImFont* font_cn = LoadChineseFont(main_scale, false);
    ImFont* font_bold = LoadChineseFont(main_scale, true);
    if (!font_cn) font_cn = io.Fonts->AddFontDefault();
    io.FontDefault = font_cn;
    // bold 加载失败则回退 regular
    if (font_bold == nullptr) font_bold = font_cn;

    App app;
    // 常用命令下拉(工具目录 gm_cmd.txt,随工具走);格式错误行 -> 弹窗提示(有效行仍保留)
    int gm_cmd_err = LoadGmCmdList(app, /*keep_previous_on_error=*/false);
    if (gm_cmd_err > 0)
        MsgBox("读取配置", "常用命令文件 gm_cmd.txt 有 " + std::to_string(gm_cmd_err) +
                               " 行格式错误，已忽略这些行。",
               MB_ICONWARNING);

    const ImVec4 clear_color = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);

    // 主循环
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) ==
                                       DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // F12 切换调试 tab(内存查看 / 偏移设置)
        if (ImGui::IsKeyPressed(ImGuiKey_F12)) app.hidden = !app.hidden;

        // 无敌/一击必杀定时写回(每 500ms,对所有队员)
        TickLock(app, io.DeltaTime);
        // GM 状态周期刷新(每 2s,仅 GM tab 可见时)
        TickGm(app, io.DeltaTime);

        RenderMainWindow(app);
        RenderWelcomeDialog(app);
        RenderMsgQueue();

        // 渲染
        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
                                                   (const float*)&clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // 关闭:还原自注入的字节并释放进程句柄
    try {
        app.modder.Close();
    } catch (...) {
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

}  // namespace poq

// 全局 WinMain 入口(GUI 子系统,无控制台窗口):桥接到 poq::RunApp。
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return poq::RunApp(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
}
