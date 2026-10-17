// 秦殇物品修改器 (PoqEditor) —— 角色/道具修改 Tab(逻辑与 UI)
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

// 按游戏实际加载的引擎模块自动选版本模式:EasyRPG.dll=Steam(mode1),EasyRPG116.dll=v1.30(mode0)。
// 两版本 hook/flag 偏移不同,自动选避免用户手选错。
static void AutoSelectMode(App& app, unsigned int pid) {
    std::string want;
    if (ProcessMemory::FindModuleBase(pid, "EasyRPG.dll"))
        want = "mode1";
    else if (ProcessMemory::FindModuleBase(pid, "EasyRPG116.dll"))
        want = "mode0";
    if (want.empty() || want == app.cfg.current_mode) return;
    app.cfg.SetCurrent(want);
    app.modder.Config() = app.cfg;
    app.basic_fields = app.cfg.Current().basic_fields;
    app.RebuildBasic();
    app.ReloadCfgBuffers();
    for (size_t i = 0; i < app.mode_ids.size(); ++i)
        if (app.mode_ids[i] == want) {
            app.mode_selected = static_cast<int>(i);
            break;
        }
}

void OnInit(App& app) {
    auto pid = TryGetSelectedPid(app);
    if (!pid) {
        app.SetStatus("请先选择一个进程。");
        return;
    }
    AutoSelectMode(app, *pid);  // 按引擎模块自动选 steam / v1.30
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
            AutoSelectMode(app, *new_pid);  // 重启后按新进程引擎模块重选版本
            ptr = app.modder.Initialize(*pid);
            restarted = true;
        }
        uintptr_t slot = app.modder.CurItemSlotAddr();
        std::snprintf(app.raw_addr, sizeof(app.raw_addr), "0x%s", Hex8(slot).c_str());
        const bool dbg = !app.hidden;
        std::string prefix = restarted ? "检测到游戏重启,已自动重新选择进程。" : "";
        const bool injected = app.modder.IsInjected();
        if (dbg) {
            std::string base_info = "hook模块=" + app.cfg.Current().hook.module + " hook@0x" +
                                    Hex8(app.modder.HookAddr()) + " cur_item槽=0x" + Hex8(slot);
            if (!injected) {
                app.SetStatus(prefix + "初始化成功(注入失败,道具修改不可用)。请重新初始化。" +
                              base_info);
            } else if (ptr == 0 || ptr == 0xFFFFFFFF) {
                app.SetStatus(
                    prefix +
                    "初始化成功(已注入)。当前无手持物品,请在游戏中拾起物品后点\"读取属性\"。" +
                    base_info);
            } else {
                app.SetStatus(prefix + "初始化成功(已注入)。手持物品指针 = 0x" + Hex8(ptr) +
                              base_info);
            }
        } else {
            if (!injected) {
                app.SetStatus(prefix + "初始化成功(注入失败,道具修改不可用)。请重新初始化。");
            } else if (ptr == 0 || ptr == 0xFFFFFFFF) {
                app.SetStatus(
                    prefix +
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
                if (w.is_enum)
                    w.enum_val = it->second;
                else
                    w.intval = it->second;
            }
        }
    }
}

// 把界面上的 道具名字/道具别名/类型 记进快照(供"复制上一件物品"按需复制)
void SnapshotNames(App& app) {
    app.last_name.clear();
    app.last_alias.clear();
    app.last_type = -1;
    for (size_t i = 0; i < app.basic_fields.size() && i < app.basic.size(); ++i) {
        const std::string& lb = app.basic_fields[i].label;
        if (lb == "道具名字")
            app.last_name = app.basic[i].strbuf;
        else if (lb == "道具别名")
            app.last_alias = app.basic[i].strbuf;
        else if (lb == "类型")
            app.last_type = app.basic[i].is_enum ? app.basic[i].enum_val : app.basic[i].intval;
    }
}

// 字段值的可读显示: 枚举字段(如 类型/五行)显示成"值：名字", 与下拉框一致; 其它直接数字。
std::string FieldValueDisplay(const App& app, const char* label, int v) {
    for (const auto& f : app.cfg.Current().basic_fields) {
        if (f.label != label) continue;
        auto it = f.enum_map.find(v);
        if (it != f.enum_map.end())
            return f.enum_display_value ? (std::to_string(v) + "：" + it->second) : it->second;
        break;
    }
    return std::to_string(v);
}

BasicState CollectBasicState(const App& app) {
    BasicState st;
    for (size_t i = 0; i < app.basic_fields.size(); ++i) {
        const auto& f = app.basic_fields[i];
        const App::BasicWidget& w = app.basic[i];
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
void ReadFromBase(App& app, uintptr_t ib) {
    FillBasic(app, app.modder.ReadBasicFields(ib));
    app.attrs = app.modder.ReadAttributes(ib);
    app.attr_sel.assign(app.attrs.size(), false);
    app.last_attrs = app.attrs;  // 快照:供"复制上一件物品"用
    SnapshotNames(app);          // 名字/别名也快照(复制时询问是否带过来)
    app.last_item_base = ib;
    if (!app.hidden) {
        app.SetStatus("读取完成。物品基址 = 0x" + Hex8(ib) + ",属性 " +
                      std::to_string(app.attrs.size()) + " 条。");
    } else {
        app.SetStatus("读取完成,属性 " + std::to_string(app.attrs.size()) + " 条。");
    }
}

void OnRead(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先点击\"初始化\"。");
        return;
    }
    try {
        uintptr_t ib = 0;
        if (app.modder.BagFlagActive()) {
            // 背包拿起边缘门:闩锁 = 最近一次"从背包拿起(0->1)"的物品,读它即可。
            // 悬停/场景读取在 flag 平直期不会入闩锁,无需再等捕获。
            uintptr_t p = app.modder.GetItemPointer();
            if (p < 0x10000) {
                app.SetStatus(
                    "请先在游戏里从背包拿起一件物品(点一下让图标贴到光标),再点\"读取属性\"。");
                return;
            }
            ib = p + app.modder.Config().Current().item_base_adjust;
        } else if (app.modder.CaptureFreezeSupported()) {
            // 解冻让新捕获可通过;绝不清槽——不匹配门控的读取不改 cur_item,
            // 失败也保持原地址(只会被 cave 的合法捕获改写)。
            app.SetStatus("正在捕获手持物品…(请保持武器握在手上)");
            app.modder.SetCaptureFrozen(false);
            for (int i = 0; i < 60; ++i) {  // 最长 ~3 秒
                uintptr_t p = app.modder.GetItemPointer();
                if (p >= 0x10000) {
                    ib = p + app.modder.Config().Current().item_base_adjust;
                    break;
                }
                Sleep(50);
            }
            app.modder.SetCaptureFrozen(true);
            if (!ib) {
                app.SetStatus("未读到手持物品,请手持武器后重试(cur_item 保持原地址不变)。");
                return;
            }
        } else {
            ib = app.modder.GetItemBase();
        }
        ReadFromBase(app, ib);
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
        // 保存成功后刷新快照(含用户编辑后的值),供"复制上一件物品"用。
        app.last_attrs = props;
        SnapshotNames(app);
        app.last_item_base = ib;
        std::string msg = "保存完成:基础属性 " + std::to_string(bs.ints.size() + bs.strs.size()) +
                          " 项,属性 " + std::to_string(written) + " 条已写入。";
        if (skipped > 0) {
            msg += "(" + std::to_string(skipped) + " 条新增属性因节点池无空闲槽被跳过)";
            MsgBox("部分属性未写入",
                   "有 " + std::to_string(skipped) +
                       " 条新增属性因节点池无空闲槽被跳过。\n"
                       "若刚丢弃过武器或读档过,可点击\"重置节点池\"回收全部槽位后重试。\n"
                       "已写入的属性不受影响。",
                   MB_ICONWARNING);
        }
        msg += "请在游戏中把物品扔到地上再捡起使修改生效。";
        app.SetStatus(msg);
        MsgBox("修改成功", "请在游戏中把物品扔到地上在捡起来使修改生效", MB_ICONINFORMATION);
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("保存失败", msg, MB_ICONERROR);
        app.SetStatus("保存失败:" + msg);
    }
}

static void ApplyCopyPrevItem(App& app, bool withIdent);  // 先问后做: 真正写内存

// 把缓存的上一件物品属性写到当前物品:
// 现有节点原位覆盖(前 min(m,k) 条),多出来的词条从节点池分配接到链尾,
// 少了则截断。名字/别名/类型按用户选择决定是否一起写。
void OnCopyPrevItem(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先点击\"初始化\"。");
        return;
    }
    if (app.last_attrs.empty()) {
        app.SetStatus("还没有读取过任何物品的属性,无法复制。");
        return;
    }
    // 先问名字/别名/类型要不要一起复制, 再动手(取消 = 什么都不做)
    std::string q = "上一件物品:";
    if (!app.last_name.empty()) q += "\n  名字: " + app.last_name;
    if (!app.last_alias.empty()) q += "\n  别名: " + app.last_alias;
    if (app.last_type >= 0) q += "\n  类型: " + FieldValueDisplay(app, "类型", app.last_type);
    AskIncludeIdent(app, "复制上一件物品", q,
                    "这三项(名字/别名/类型)要不要一起复制过来？词条一定复制。",
                    "已取消复制上一件物品(什么都没改)。",
                    [&app](bool with_ident) { ApplyCopyPrevItem(app, with_ident); });
}

// 真正做"复制上一件物品": withIdent=true 连名字/别名/类型一起写。
static void ApplyCopyPrevItem(App& app, bool withIdent) {
    try {
        uintptr_t ib = app.modder.GetItemBase();
        auto written_skipped = app.modder.WriteAttributes(ib, app.last_attrs);
        int written = written_skipped.first;
        int skipped = written_skipped.second;
        std::string msg =
            std::string(withIdent ? "已复制上一件物品的属性+名字/别名/类型,写入 "
                                  : "已复制上一件物品的属性(名字/别名/类型保持原样),写入 ") +
            std::to_string(written) + " 条。";
        if (ib == app.last_item_base) {
            msg += "(注意:当前物品就是缓存的那件)";
        }
        if (skipped > 0) {
            msg += "(" + std::to_string(skipped) +
                   " 条因节点池无空闲槽被跳过,可点\"重置节点池\"后重试)";
        }
        msg += "请在游戏中把物品扔到地上再捡起使修改生效。";
        // 表格同步为已写入的内容,与内存一致
        app.attrs = app.last_attrs;
        app.attr_sel.assign(app.attrs.size(), false);
        if (withIdent) {
            BasicState bs;
            if (!app.last_name.empty()) bs.strs["道具名字"] = app.last_name;
            if (!app.last_alias.empty()) bs.strs["道具别名"] = app.last_alias;
            if (app.last_type >= 0) bs.ints["类型"] = app.last_type;
            if (!bs.strs.empty() || !bs.ints.empty()) app.modder.WriteBasicFields(ib, bs);
            for (size_t i = 0; i < app.basic_fields.size() && i < app.basic.size(); ++i) {
                const std::string& lb = app.basic_fields[i].label;
                if (!app.last_name.empty() && lb == "道具名字")
                    std::snprintf(app.basic[i].strbuf, sizeof(app.basic[i].strbuf), "%s",
                                  app.last_name.c_str());
                else if (!app.last_alias.empty() && lb == "道具别名")
                    std::snprintf(app.basic[i].strbuf, sizeof(app.basic[i].strbuf), "%s",
                                  app.last_alias.c_str());
                else if (app.last_type >= 0 && lb == "类型") {
                    if (app.basic[i].is_enum)
                        app.basic[i].enum_val = app.last_type;
                    else
                        app.basic[i].intval = app.last_type;
                }
            }
        }
        app.SetStatus(msg);
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        app.SetStatus("复制失败:" + msg);
    }
}

static void ApplyLoadedTpl(App& app, const TemplateLoadResult& res, bool withIdent);

void OnLoadTpl(App& app) {
    std::string path =
        OpenFileDialog(AppDirW(), L"JSON 文件 (*.json)\0*.json\0所有文件 (*)\0*.*\0\0");
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
    // 名字/别名/类型: 模板里有就先问要不要一起导入, 再动手(取消 = 什么都不做)
    std::string nm, al, ty;
    {
        auto n1 = res.basic_strings.find("道具名字");
        if (n1 != res.basic_strings.end()) nm = n1->second;
        auto a1 = res.basic_strings.find("道具别名");
        if (a1 != res.basic_strings.end()) al = a1->second;
        auto t1 = res.basic_values.find("类型");
        if (t1 != res.basic_values.end()) ty = FieldValueDisplay(app, "类型", t1->second);
    }
    if (nm.empty() && al.empty() && ty.empty()) {
        ApplyLoadedTpl(app, res, true);  // 没有这三项, 直接全量导入
        return;
    }
    std::string q = "模板里带有:";
    if (!nm.empty()) q += "\n  道具名字: " + nm;
    if (!al.empty()) q += "\n  道具别名: " + al;
    if (!ty.empty()) q += "\n  类型: " + ty;
    AskIncludeIdent(app, "导入模板", q,
                    "这三项(名字/别名/类型)要不要一起导入？词条一定导入。",
                    "已取消导入(什么都没改)。",
                    [&app, res](bool with_ident) { ApplyLoadedTpl(app, res, with_ident); });
}

// 真正应用模板: withIdent=false 时不导入 类型 与 名字/别名, 其余照常。
static void ApplyLoadedTpl(App& app, const TemplateLoadResult& res, bool withIdent) {
    for (size_t i = 0; i < app.basic_fields.size(); ++i) {
        const auto& f = app.basic_fields[i];
        auto it = res.basic_values.find(f.label);
        if (it == res.basic_values.end()) continue;
        if (!withIdent && f.label == "类型") continue;  // 类型归入"名字/别名"一档
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
    if (withIdent) {
        auto n1 = res.basic_strings.find("道具名字");
        auto a1 = res.basic_strings.find("道具别名");
        for (size_t i = 0; i < app.basic_fields.size() && i < app.basic.size(); ++i) {
            const std::string& lb = app.basic_fields[i].label;
            if (n1 != res.basic_strings.end() && lb == "道具名字")
                std::snprintf(app.basic[i].strbuf, sizeof(app.basic[i].strbuf), "%s",
                              n1->second.c_str());
            else if (a1 != res.basic_strings.end() && lb == "道具别名")
                std::snprintf(app.basic[i].strbuf, sizeof(app.basic[i].strbuf), "%s",
                              a1->second.c_str());
        }
    }
    if (!res.errors.empty()) {
        std::string msg = "部分属性存在错误,跳过加载相关词条,请检查原始json\n\n";
        for (const auto& e : res.errors) msg += e + "\n";
        MsgBox("部分属性存在错误", msg, MB_ICONWARNING);
    }
    app.SetStatus(std::string("已从模板载入:") +
                  (withIdent ? "含名字/别名/类型," : "名字/别名/类型未导入,") + "基础属性 " +
                  std::to_string(res.basic_values.size()) + " 项,属性 " +
                  std::to_string(res.attrs.size()) + " 条。");
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
            if (end == t.c_str() || *end != '\0')
                throw std::runtime_error(std::string(key) + " 不是整数");
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
    static const std::set<std::string> s = {"普通防御", "金防御", "木防御",
                                            "水防御",   "火防御", "土防御"};
    return s;
}
// 一击必杀涉及的攻击字段(普通攻击 + 五行攻击,均为 pair)。
const std::set<std::string>& AtkLabels() {
    static const std::set<std::string> s = {"普通攻击", "金攻击", "木攻击",
                                            "水攻击",   "火攻击", "土攻击"};
    return s;
}

// 字段在当前锁状态下是否应禁用编辑(无敌:生命/精气/防御/闪避率;一击必杀:攻击/命中率)。
bool IsCharFieldLocked(const App& app, const std::string& label) {
    // 字段固有只读优先(计划 P1 #2.3: UI 禁用状态直接来自字段定义)
    if (app.ck_invincible) {
        if (label == "生命" || label == "精气" || label == "闪避率" || DefLabels().count(label))
            return true;
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
        if (vit == values.end()) continue;  // 读取失败: loaded 保持 false -> 不会被保存(计划 P1 #4)
        const CharValue& v = vit->second;
        if (f.kind == CharKind::StringPtr) {
            std::snprintf(w.strbuf, sizeof(w.strbuf), "%s", v.str.c_str());
        } else if (f.kind == CharKind::Pair) {
            w.pair_lo = v.lo;
            w.pair_hi = v.hi;
        } else {
            w.intval = v.lo;
        }
        w.loaded = true;  // 只有真正读到才允许保存
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
        auto located = LocateByOffsets(app.modder.Mem(), app.modder.Pid(), module, app.char_filter);
        if (app.hidden)
            app.char_slot_probe.clear();
        else
            app.char_slot_probe = ProbeSlotNames(app.modder.Mem(), app.modder.Pid(), module);
        if (located.empty()) {
            app.char_status = "未定位到队员(可尝试取消过滤后再次读取属性)。";
            app.SetStatus("未定位到队员(可尝试取消过滤后再次读取属性)。");
            return;
        }
        // 记住读取前选中的角色: 读取会刷新所有人, 但选中项不跳
        std::string prev_sel = (app.char_selected >= 0 &&
                                static_cast<size_t>(app.char_selected) < app.char_names.size())
                                   ? app.char_names[static_cast<size_t>(app.char_selected)]
                                   : "";
        app.char_addrs.clear();
        app.char_names.clear();
        for (auto& kv : located) {
            app.char_addrs[kv.first] = kv.second;  // map 供按名查找
            app.char_names.push_back(kv.first);    // 槽位顺序保留下拉框
        }
        // 选中保持: 还在列表里就维持原选择; 不在(角色离队/被过滤)才默认第一项
        app.char_selected = 0;
        for (size_t i = 0; i < app.char_names.size(); ++i)
            if (app.char_names[i] == prev_sel) {
                app.char_selected = static_cast<int>(i);
                break;
            }
        // 全局属性:从第 1 人读
        const std::string& first = app.char_names.front();
        auto g = ReadGlobalChar(app.modder.Mem(), app.char_addrs[first]);
        auto mit = g.find("金钱");
        if (mit != g.end())
            std::snprintf(app.money_buf, sizeof(app.money_buf), "%d", mit->second.lo);
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
    // 1) 收集字段值: 只收 "可写 且 读取成功" 的字段(计划 P1 #4)
    //    未加载的字段跳过 -> 不会把"读取失败"当成"值=0"写回去
    std::map<std::string, CharValue> values;
    int skipped_unloaded = 0;
    for (size_t i = 0; i < CharFields().size(); ++i) {
        const auto& f = CharFields()[i];
        const CharWidget& w = app.char_widgets[i];
        if (!f.writable || !w.loaded) {
            if (!f.writable || !w.loaded) ++skipped_unloaded;
            continue;
        }
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
    // 2) 校验收集到的字段(有非法则弹窗并拒绝,整次不写)
    std::vector<std::string> errors;
    for (const auto& f : CharFields()) {
        auto vit = values.find(f.label);
        if (vit == values.end()) continue;  // 未收集(只读/未加载)
        auto err = ValidateValue(f, vit->second);
        if (err) errors.push_back(*err);
    }
    if (!errors.empty()) {
        std::string msg = "以下字段不合法,未保存:\n\n";
        for (const auto& e : errors) msg += e + "\n";
        MsgBox("校验失败", msg, MB_ICONERROR);
        app.SetStatus("校验失败,未保存:" + name + "。");
        return;
    }
    // 3) 校验通过,逐字段写入并统计失败(计划 P2 #11: 部分失败要如实报告)
    int saved = 0;
    std::vector<std::string> failed;
    for (const auto& f : CharFields()) {
        auto vit = values.find(f.label);
        if (vit == values.end()) continue;  // 未收集(只读/未加载)
        if (WriteCharField(app.modder.Mem(), base, f, vit->second))
            ++saved;
        else
            failed.push_back(f.label);
    }
    std::string tail_msg;
    if (skipped_unloaded > 0) {
        tail_msg =
            "\n\n跳过 " + std::to_string(skipped_unloaded) + " 个字段(只读或读取失败, 未写入)";
    }
    if (!failed.empty()) {
        std::string m = "成功写入 " + std::to_string(saved) + " 个,失败 " +
                        std::to_string(failed.size()) + " 个。\n失败字段:";
        for (const auto& f : failed) m += " " + f;
        m += tail_msg;
        app.char_status = "保存部分失败:" + name;
        app.SetStatus("保存部分失败:" + name + "。");
        MsgBox("保存完成(部分失败)", m, MB_ICONWARNING);
        return;
    }
    app.char_status =
        "保存完成:" + name + " " + std::to_string(saved) + " 个字段已写入。请在游戏中验证。";
    app.SetStatus("角色属性保存完成:" + name + "。");
    MsgBox("保存完成",
           "请存档后重新读档查看变化，部分属性词条可能因道具属性加成与设定值不同" + tail_msg,
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
    // 计划 P2(#12): 单字段路径也要检查写入结果, 不能忽略失败仍提示成功
    if (!WriteCharField(app.modder.Mem(), app.char_addrs[first], gf, v)) {
        app.SetStatus("金钱写入失败。");
        MsgBox("写入失败", "金钱写入失败(目标进程可能已退出或页面不可写)。", MB_ICONERROR);
        return;
    }
    MsgBox("金钱已修改", "修改后丢弃金钱、购买商品、捡起金钱，数额才会更新", MB_ICONINFORMATION);
}

// ---- 无敌 / 一击必杀锁 ----
// 立即把已启用锁的字段写为 9999(对所有队员,不等定时器)。
void LockWriteOnce(App& app) {
    if (!app.modder.Mem().IsOpen() || app.char_addrs.empty()) return;
    if (!app.ck_invincible && !app.ck_kill) return;
    // 计划 260929 #13: 用户主动写入不能静默吞失败 —— 统计并上报(仍逐字段尽力写,
    // 单个字段失败不中断锁定)。
    int ok_n = 0, fail_n = 0;
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
                        if (WriteCharField(app.modder.Mem(), base, f, v))
                            ++ok_n;
                        else
                            ++fail_n;
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
                        if (WriteCharField(app.modder.Mem(), base, f, v))
                            ++ok_n;
                        else
                            ++fail_n;
                    } else if (f.label == "命中率") {
                        CharValue v;
                        v.lo = 9999;
                        if (WriteCharField(app.modder.Mem(), base, f, v))
                            ++ok_n;
                        else
                            ++fail_n;
                    }
                }
            }
        } catch (const std::exception&) {
            ++fail_n;  // 该角色整体不可写(如已退出地图), 继续其余角色
        }
    }
    if (fail_n > 0)
        app.SetStatus("锁定写入: 成功 " + std::to_string(ok_n) + " 个, 失败 " +
                      std::to_string(fail_n) + " 个(角色可能未加载)");
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
            } catch (const std::exception&) {
                app.SetStatus("解除无敌: 角色 " + kv.first + " 恢复失败(生命/精气未还原)");
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
            } catch (const std::exception&) {
                app.SetStatus("解除一击必杀: 角色 " + kv.first + " 攻击/命中率恢复失败");
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
        bool cond_inactive =
            f.conditional && f.conditional->field == "类型" &&
            !(type_val && std::find(f.conditional->values.begin(), f.conditional->values.end(),
                                    *type_val) != f.conditional->values.end());
        bool disabled = f.readonly || forced || cond_inactive;

        std::string label = f.label;
        if (f.readonly) label += "（只读）";

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::BeginDisabled(disabled);

        if (f.type == "string_ptr") {
            ImGui::InputText(("##b" + std::to_string(i)).c_str(), w.strbuf, sizeof(w.strbuf),
                             ImGuiInputTextFlags_None);
        } else if (w.is_enum) {
            // 下拉选项:值->名字(enum_display_value 显示 "值：名字")
            std::vector<std::pair<int, std::string>> options;
            for (const auto& kv : f.enum_map) {
                std::string item =
                    f.enum_display_value ? std::to_string(kv.first) + "：" + kv.second : kv.second;
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
            if (forced)
                edit = f.force_when->value;
            else if (cond_inactive)
                edit = -1;
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
    ImGui::SameLine();
    if (ImGui::Button("重置节点池")) {
        if (!app.modder.Mem().IsOpen()) {
            app.SetStatus("请先点击\"初始化\"。");
        } else if (app.modder.ResetPool()) {
            app.SetStatus("节点池已清零,全部槽位恢复可用。");
        } else {
            app.SetStatus("重置节点池失败(池不存在或清零失败)。");
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "把新增词条用的节点池整体清零回收。\n"
            "在游戏里丢弃过带新词条的武器、或读档之后,\n"
            "旧链表已作废但池槽不清零会一直占着,点此全部回收。\n"
            "注意:若还有存活武器的词条挂在池内,清零会废掉那些词条。");
    }
    ImGui::SameLine();
    if (ImGui::Button("复制上一件物品")) OnCopyPrevItem(app);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "把上次读取/保存的那件物品的词条,复制到当前物品上。\n"
            "不复制基础属性(名字、耐久等各物品独立)。\n"
            "用法:读取并改好一把武器后,捡起新武器,直接点此按钮。");
    }
    ImGui::PopStyleVar();
    ImGui::SameLine();
    ImGui::Text("属性数：%d", static_cast<int>(app.attrs.size()));

    ImGui::Separator();

    static const std::vector<std::string>& name_items = ItemData::AttrDisplayListRef();
    static std::vector<const char*> name_items_c;
    if (name_items_c.empty())
        for (const auto& s : name_items) name_items_c.push_back(s.c_str());
    std::vector<const char*> percent_items_c;
    for (const auto& kv : ItemData::PercentNames()) percent_items_c.push_back(kv.second.c_str());
    // 属性颜色下拉:颜色字段 + node_flag@0x10(隐藏/激活) 合成 6 项。
    // 颜色:0白 1蓝 2浅蓝 3金 4不显示;浅蓝按 active 拆「激活/未激活」两项。
    static const char* kColorStateItems[] = {
        "白色", "蓝色(K)", "浅蓝色激活(C)", "浅蓝色未激活(H)", "金色(Z)", "不显示",
    };
    auto color_to_idx = [](uint32_t c, uint32_t act) -> int {
        switch (c) {
            case 0:
                return 0;
            case 1:
                return 1;
            case 2:
                return act ? 2 : 3;
            case 3:
                return 4;
            default:
                return 5;  // 4 及未知 -> 不显示
        }
    };
    auto color_from_idx = [](int idx, uint32_t& c, uint32_t& act) {
        switch (idx) {
            case 0:
                c = 0;
                act = 1;
                break;
            case 1:
                c = 1;
                act = 1;
                break;
            case 2:
                c = 2;
                act = 1;
                break;  // 浅蓝激活
            case 3:
                c = 2;
                act = 0;
                break;  // 浅蓝未激活(隐藏)
            case 4:
                c = 3;
                act = 1;
                break;
            default:
                c = 4;
                act = 1;
                break;
        }
    };

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_NoHostExtendY;
    if (ImGui::BeginTable("attrs", 5, flags, ImVec2(0.0f, height))) {
        ImGui::TableSetupColumn("选择", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("属性名", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("属性颜色", ImGuiTableColumnFlags_WidthFixed, 178.0f);
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
                if (ImGui::Checkbox("##sel", &sel)) app.attr_sel[static_cast<size_t>(r)] = sel;
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
                int cur = color_to_idx(p.color, p.active);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##color", &cur, kColorStateItems, 6)) {
                    color_from_idx(cur, p.color, p.active);
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
// 属性修改(角色)Tab:全局属性 + 角色选择 + 字段网格(task6 指定排列)。
void RenderCharTab(App& app) {
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
    for (size_t i = 0; i < app.char_names.size(); ++i) {
        // 下拉格式 = "角色N-职业-名字"(前缀"角色N-" + 前面角色类型,后面角色名字)
        auto it = KnownMembers().find(app.char_names[i]);
        std::string cls = (it != KnownMembers().end()) ? it->second + "-" : "";
        char_labels.push_back("角色" + std::to_string(i + 1) + "-" + cls + app.char_names[i]);
    }
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
    bool filter_on = app.char_filter;
    if (ImGui::Checkbox("角色属性校验过滤", &filter_on)) {
        if (!filter_on) app.char_filter_warn = true;
        app.char_filter = filter_on;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", app.char_status.c_str());
    if (!app.hidden && !app.char_slot_probe.empty()) {
        ImGui::TextDisabled("槽位探测:");
        for (size_t i = 0; i < app.char_slot_probe.size(); ++i) {
            ImGui::TextDisabled("  槽%zu=%s", i + 1, app.char_slot_probe[i].c_str());
        }
    }
    // 关闭"角色属性校验过滤"时的警告弹窗(居中、不可移动)
    if (app.char_filter_warn) {
        app.char_filter_warn = false;
        ImGui::OpenPopup("警告##charfilter");
    }
    ImGui::SetNextWindowSize(ImVec2(460, 170), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(
            "警告##charfilter", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
        // 每帧强制整窗居中(不依赖 Appearing 锚点,避免 AutoResize 导致偏位)
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetWindowPos(ImVec2(center.x - ImGui::GetWindowSize().x * 0.5f,
                                   center.y - ImGui::GetWindowSize().y * 0.5f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 20);
        ImGui::TextWrapped("如果取消校验过滤，可能识别出其他角色，请谨慎修改！");
        ImGui::Spacing();
        const float btn_w = 120;
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - btn_w) * 0.5f);
        ImGui::SetCursorPosY(ImGui::GetWindowSize().y - 40);
        if (ImGui::Button("确认", ImVec2(btn_w, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();

    // ---- 字段网格(task6 指定排列:3 列行 + 2 列行)----
    // 每格 = {字段名, 宽度权重}。权重用于让"经验"占两格宽(它是成对字段、渲染成两个输入框),
    // 于是该行四个输入框(经验 lo/hi + 声望 + 视野)等宽。
    struct CharCol {
        const char* label;
        int w;
    };
    static const CharCol kLayout[][3] = {
        {{"姓名", 1}, {"别名", 1}, {"职业", 1}},
        {{"年龄", 1}, {"性别", 1}, {"等级", 1}},
        {{"经验", 2}, {"声望", 1}, {"视野", 1}},
        {{"生命", 1}, {"精气", 1}, {nullptr, 0}},
        {{"力量", 1}, {"体质", 1}, {nullptr, 0}},
        {{"敏捷", 1}, {"悟性", 1}, {nullptr, 0}},
        {{"魅力", 1}, {"智慧", 1}, {nullptr, 0}},
        {{"普通攻击", 1}, {"普通防御", 1}, {nullptr, 0}},
        {{"金攻击", 1}, {"金防御", 1}, {nullptr, 0}},
        {{"木攻击", 1}, {"木防御", 1}, {nullptr, 0}},
        {{"水攻击", 1}, {"水防御", 1}, {nullptr, 0}},
        {{"火攻击", 1}, {"火防御", 1}, {nullptr, 0}},
        {{"土攻击", 1}, {"土防御", 1}, {nullptr, 0}},
        {{"命中率", 1}, {"闪避率", 1}, {nullptr, 0}},  // 命中率/闪避率 左右对调
        {{"移动速度", 1}, {"剩余属性点", 1}, {"剩余技能点", 1}},
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
        // 禁用条件: 没定位到人 / 模式锁定 / 字段固有只读 / 读取失败(不能把"读失败"改成 0 保存)
        bool disabled =
            app.char_addrs.empty() || IsCharFieldLocked(app, label) || !f.writable || !w.loaded;
        if (!f.writable) {
            ImGui::TextDisabled("%s(只读)", label);
            return;
        }
        if (!w.loaded) {
            ImGui::TextDisabled("%s(读取失败)", label);
            return;
        }
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
        int ncols = kLayout[r][2].label ? 3 : 2;
        char tid[32];
        std::snprintf(tid, sizeof(tid), "charrow%zu", r);
        if (ImGui::BeginTable(tid, ncols)) {
            for (int c = 0; c < ncols; ++c)
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch,
                                        static_cast<float>(kLayout[r][c].w));
            ImGui::TableNextRow();
            for (int c = 0; c < ncols; ++c) {
                if (ImGui::TableSetColumnIndex(c)) render_field(kLayout[r][c].label);
            }
            ImGui::EndTable();
        }
    }
    // 2026-10-17 实测: 视野(+0x198)/移动速度(+0x190) 直写都安全, 但游戏只在换图/
    // 进图时读取 —— 提示用户生效时机, 免得以为没生效反复改。
    ImGui::TextDisabled("视野/移动速度改写换图/读档后生效。");
    ImGui::EndChild();
}

}  // namespace poq
