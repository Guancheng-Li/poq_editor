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
#include <commdlg.h>

#include <d3d11.h>
#include <tchar.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

void RenderWelcomeDialog(App& app) {
    // 每次启动显示的免责声明弹窗:居中,底部中间"确认"关闭。
    if (!app.show_welcome) return;
    if (!ImGui::IsPopupOpen("关于")) ImGui::OpenPopup("关于");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 240), ImGuiCond_Appearing);
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
        float y = win_h * 0.16f;
        const float line_h = ImGui::GetTextLineHeightWithSpacing();
        for (const char* line : kLines) {
            float tw = ImGui::CalcTextSize(line).x;
            ImGui::SetCursorPos(ImVec2((win_w - tw) * 0.5f, y));
            ImGui::TextUnformatted(line);
            y += line_h;
        }
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
    ImGui::Begin("秦殇内存修改器", nullptr,
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
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"秦殇内存修改器",
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
