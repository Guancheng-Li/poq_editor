// 秦殇物品修改器 (PoqEditor) —— GAM 存档修改 Tab(task13)
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
// GAM修改(task13): .gam 存档物品解析/修改/无中生有。
//   记录帧 [len][content][count][echo=len+12]; 字段相对"别名尾 aend"固定偏移;
//   词条=G_Info 字符串字段(GBK 文本); 地图=顶层容器(场景/物体组), 物品按区间归属。
//   全部文件级, 不依赖游戏注入。
// ============================================================================

static std::string GbkToUtf8Str(const std::string& g) {
    // 词条文本固定为 GBK(936), 不依赖系统区域(CP_ACP 在非中文系统会转乱码)
    if (g.empty()) return "";
    int n = ::MultiByteToWideChar(936, 0, g.data(), (int)g.size(), nullptr, 0);
    if (n <= 0) return g;
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(936, 0, g.data(), (int)g.size(), &w[0], n);
    return WideToUtf8(w);
}
static std::string Utf8ToGbkStr(const std::string& u) {
    if (u.empty()) return "";
    std::wstring w = Utf8ToWide(u);
    if (w.empty()) return u;
    int n = ::WideCharToMultiByte(936, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return u;
    std::string g((size_t)n, '\0');
    ::WideCharToMultiByte(936, 0, w.data(), (int)w.size(), &g[0], n, nullptr, nullptr);
    return g;
}

// 道具修改属性名(UTF-8) -> 该属性在 G_Info 里的词条名(UTF-8)。差异:
// "普通攻击"↔"普通攻击力"; "X属性攻击"↔"X系攻击力"; "普通防御"↔"普通防御力";
// "X属性防御"↔"X系防御力"。
static std::string AttrGInfoAliasU8(const std::string& n) {
    return poq::ItemData::AttrGInfoName(n);  // 唯一映射表在 ItemData(GAM 写盘也用它)
}

// "29：普通攻击"(AttrDisplay 带 id 前缀+全角冒号) -> 纯名 "普通攻击"
static std::string AttrBareNameU8(const std::string& disp) {
    size_t p = disp.find("：");  // 全角冒号(UTF-8 3B)
    if (p == std::string::npos) return disp;
    return disp.substr(p + 3);
}

// GAM 字段名 -> "道具修改"字段标签(借其类型/枚举/范围做下拉与合法校验)
static const char* GamFieldToItemLabel(const std::string& name) {
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

// 前置声明(GamOnSave 会调用这些后定义的函数)
static int GamSelMode(const App& app);
static void ApplyGamPasteAttr(App& app, int idx, bool withIdent);  // 粘贴属性(先问后做)
static void GamCommitGInfo(App& app, poq::GamItem& it);

// 物品显示名: 中文名(别名), 没有中文映射时仅别名。
static std::string GamItemDispName(const App& app, const std::string& alias) {
    if (app.gam_item_names_loaded) {
        std::string cn = app.gam_item_names.Cn(alias);
        if (!cn.empty()) return cn + "(" + alias + ")";
    }
    return alias;
}

// 生成物品属性提示(悬停显示): 解析记录 -> 固定字段 + 词条文本。
static std::string GamFormatItemTip(const App& app, const std::vector<uint8_t>& rec) {
    poq::GamItem it;
    if (!poq::GamItem::ParseRecord(rec, it)) return "";
    std::string out = GamItemDispName(app, it.alias);
    bool ok = false;
    int32_t lvl = it.GetField("lvl", &ok);
    if (ok) out += "  Lv" + std::to_string((int)lvl);
    out += "\n";
    // 关键字段: 价格/重量/类型/鉴定/孔数/五行/需求/攻防
    auto addf = [&](const char* label, const char* name) {
        bool o = false;
        int32_t v = it.GetField(name, &o);
        if (o && v != -1) out += std::string(label) + std::to_string((int)v) + " ";
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
    out += "\n词条:\n" + GbkToUtf8Str(it.GetGInfo());
    return out;
}

// 持久化"上次打开 gam 的目录": 放 exe 同级 poqeditor_gam.ini(UTF-8 无 BOM 单行路径)
static std::wstring GamPrefsPath() {
    return AppDirW() + L"\\poqeditor_gam.ini";
}

static void GamSavePrefs(const std::string& last_dir) {
    FILE* fp = _wfopen(GamPrefsPath().c_str(), L"wb");
    if (!fp) return;
    fwrite(last_dir.data(), 1, last_dir.size(), fp);
    fclose(fp);
}

std::string GamLoadPrefs() {
    FILE* fp = _wfopen(GamPrefsPath().c_str(), L"rb");
    if (!fp) return "";
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    std::string s(buf, n);
    size_t pos = s.find_first_of("\r\n");
    if (pos != std::string::npos) s = s.substr(0, pos);
    return s;
}

// 时间戳备份路径: <dir>/<base>.<yyyymmddhhMMss>.bak.<ext> (后缀保持原扩展名)
std::wstring TimestampedBakPath(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"" : path.substr(0, slash + 1);
    std::wstring fn = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    size_t dot = fn.find_last_of(L'.');
    std::wstring base = (dot == std::wstring::npos) ? fn : fn.substr(0, dot);
    std::wstring ext = (dot == std::wstring::npos) ? L"" : fn.substr(dot);
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t ts[32];
    ::swprintf(ts, 32, L"%04d%02d%02d%02d%02d%02d", (int)st.wYear, (int)st.wMonth, (int)st.wDay,
               (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    return dir + base + L"." + ts + L".bak" + ext;
}

static std::wstring GamInitialDir(App& app) {
    // 游戏运行时优先定位到其存档目录 System\game
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
                std::string exedir = p.substr(0, pos);
                std::wstring sdir = Utf8ToWide(exedir + "\\System\\game");
                if (::GetFileAttributesW(sdir.c_str()) != INVALID_FILE_ATTRIBUTES) return sdir;
                std::wstring dir = Utf8ToWide(exedir);
                if (!dir.empty() && ::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return dir;
            }
        }
    }
    // 游戏未运行:记忆上次打开目录
    if (!app.gam_last_dir.empty()) {
        std::wstring w = Utf8ToWide(app.gam_last_dir);
        if (::GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES) return w;
    }
    return AppDirW();
}

static void GamOnOpen(App& app) {
    std::string sel =
        OpenFileDialog(GamInitialDir(app), L"GAM 存档 (*.gam)\0*.gam\0所有文件 (*)\0*.*\0\0");
    if (sel.empty()) return;
    poq::GamFile gf;
    std::string err;
    if (!poq::GamFile::Load(sel, gf, err)) {
        MsgBox("打开失败", err, MB_ICONERROR);
        return;
    }
    // 打开时自动备份原文件: <base>.<yyyymmddhhMMss>.bak.gam
    {
        std::wstring w = Utf8ToWide(sel);
        std::wstring bak = TimestampedBakPath(w);
        if (::CopyFileW(w.c_str(), bak.c_str(), FALSE)) TrackBackup(w, bak);
    }
    app.gam_file = std::move(gf);
    app.gam_path = sel;
    app.gam_loaded = true;
    app.gam_sel_container = -1;
    app.gam_sel_item = -1;
    app.gam_edit_loaded_item = -1;
    app.gam_ginfo_pending = false;
    // 记忆上次打开目录(下次打开gam默认进该目录, 持久化到 exe 同级 ini)
    {
        size_t pos = sel.find_last_of("/\\");
        if (pos != std::string::npos) app.gam_last_dir = sel.substr(0, pos);
    }
    GamSavePrefs(app.gam_last_dir);
    app.SetStatus("GAM 已打开: " + app.gam_path + "（容器 " +
                  std::to_string(app.gam_file.containers.size()) + "，物品 " +
                  std::to_string(app.gam_file.items.size()) + "）");
}

static void GamOnSave(App& app) {
    if (!app.gam_loaded) {
        MsgBox("提示", "请先打开 gam 存档。", MB_ICONINFORMATION);
        return;
    }
    // 提交当前物品的词条编辑(装配回 G_Info)
    if (GamSelMode(app) == 1) GamCommitGInfo(app, app.gam_file.items[(size_t)app.gam_sel_item]);
    // 组装待插入记录(所有 is_new 物品 -> 按其当前内容重编)
    std::vector<std::pair<int, std::vector<uint8_t>>> added;
    for (size_t i = 0; i < app.gam_file.items.size(); ++i) {
        const auto& it = app.gam_file.items[i];
        if (!it.is_new) continue;
        int ci = -1;
        for (size_t c = 0; c < app.gam_file.containers.size(); ++c)
            for (int idx : app.gam_file.containers[c].item_idx)
                if (idx == (int)i) {
                    ci = (int)c;
                    break;
                }
        if (ci >= 0) {
            std::vector<uint8_t> rec;
            if (poq::GamItem::BuildRecord(it, rec)) added.push_back({ci, std::move(rec)});
        }
    }
    std::string err;
    if (!poq::GamFile::Save(app.gam_file, app.gam_path, added, err)) {
        MsgBox("保存失败", err, MB_ICONERROR);
        return;
    }
    app.SetStatus("GAM 已保存：" + app.gam_path + "（新增 " + std::to_string(added.size()) +
                  " 件）");
    // 重载同步内存(位置/偏移已整体后移)
    poq::GamFile gf;
    std::string e;
    if (poq::GamFile::Load(app.gam_path, gf, e)) {
        int keep_c = app.gam_sel_container, keep_i = app.gam_sel_item;
        app.gam_file = std::move(gf);
        app.gam_sel_container = keep_c;
        app.gam_sel_item = keep_i;
        app.gam_edit_loaded_item = -1;
    }
}

// 非装备记录(技能等): 词条数组里含"装备属性表(0x00..0x62)之外"的 id(技能属性在 0x10x/0x11x)。
// 这类记录的词条名/id 不在下拉表里, 编辑会把它们从词条数组里丢掉 -> 禁用属性编辑。
static bool GamIsNonEquipment(const poq::GamItem& it) {
    if (!it.has_affixes) return false;
    for (const auto& a : it.affixes)
        if (poq::ItemData::AttrNames().find((uint32_t)a.id) == poq::ItemData::AttrNames().end())
            return true;
    return false;
}

static int GamSelMode(const App& app) {
    if (app.gam_sel_container >= 0 && app.gam_sel_item >= 0 &&
        app.gam_sel_item < (int)app.gam_file.items.size())
        return 1;  // 选中物品
    if (app.gam_sel_container >= 0 && app.gam_sel_container < (int)app.gam_file.containers.size())
        return 2;  // 仅选中地图
    return 0;
}

// 复制指定物品的整条记录到剪贴板。
static void GamCopyItemAt(App& app, int idx) {
    if (idx < 0 || idx >= (int)app.gam_file.items.size()) return;
    const poq::GamItem& it = app.gam_file.items[(size_t)idx];
    if (it.is_new) {
        poq::GamItem::BuildRecord(it, app.gam_clip_record);
    } else if (it.rec_end <= app.gam_file.buf.size()) {
        app.gam_clip_record.assign(app.gam_file.buf.begin() + (ptrdiff_t)it.pos,
                                   app.gam_file.buf.begin() + (ptrdiff_t)it.rec_end);
    }
    app.gam_has_record = !app.gam_clip_record.empty();
    app.SetStatus(app.gam_has_record ? "已复制物品：" + it.alias : "复制物品失败");
}

static void GamOnCopyItem(App& app) {
    if (GamSelMode(app) != 1) {
        MsgBox("提示", "请先在左侧选中一件物品，再点\"复制物品\"。", MB_ICONINFORMATION);
        return;
    }
    GamCopyItemAt(app, app.gam_sel_item);
}

// 把新建物品放到目标容器已有物品右侧(错开坐标, 避免叠位/不可见; 无已有物品则保留源坐标)
static void GamPlaceNewItem(App& app, int item_idx) {
    if (item_idx < 0 || item_idx >= (int)app.gam_file.items.size()) return;
    if (app.gam_sel_container < 0 || app.gam_sel_container >= (int)app.gam_file.containers.size())
        return;
    auto& cont = app.gam_file.containers[(size_t)app.gam_sel_container];
    long maxx = -0x7FFFFFFF;
    for (int idx : cont.item_idx) {
        if (idx < 0 || idx >= (int)app.gam_file.items.size()) continue;
        if (idx == item_idx) continue;
        bool ok = false;
        int32_t x = app.gam_file.items[(size_t)idx].GetField("f_c8", &ok);
        if (ok && x >= 0 && x < 0x1000000) maxx = std::max<long>(maxx, (long)x);
    }
    int newx = (maxx > -0x7FFFFFFF) ? (int)(maxx + 40) : 100;
    app.gam_file.items[(size_t)item_idx].SetField("f_c8", newx);
}

// 把剪贴板记录作为新物品粘贴到容器 ci。
static void GamPasteInto(App& app, int ci) {
    if (!app.gam_has_record || app.gam_clip_record.empty()) {
        MsgBox("提示", "剪贴板中没有物品记录，请先\"复制物品\"或从目录\"创建物品\"。",
               MB_ICONINFORMATION);
        return;
    }
    if (ci < 0 || ci >= (int)app.gam_file.containers.size()) return;
    poq::GamItem it;
    if (!poq::GamItem::ParseRecord(app.gam_clip_record, it)) {
        MsgBox("粘贴失败", "剪贴板记录不是合法物品帧。", MB_ICONERROR);
        return;
    }
    it.is_new = true;
    auto& items = app.gam_file.items;
    int idx = (int)items.size();
    items.push_back(std::move(it));
    app.gam_file.containers[(size_t)ci].item_idx.push_back(idx);
    app.gam_sel_container = ci;
    app.gam_sel_item = idx;
    app.gam_edit_loaded_item = -1;
    GamPlaceNewItem(app, idx);  // 错开坐标避免叠位/不可见
    app.SetStatus("已粘贴物品：" + items[(size_t)idx].alias);
}

static void GamOnPasteItem(App& app) {
    if (GamSelMode(app) < 2) {
        MsgBox("提示", "请先在左侧选中一个地图(容器)。", MB_ICONINFORMATION);
        return;
    }
    GamPasteInto(app, app.gam_sel_container);
}

static void GamOnCopyAttr(App& app) {
    if (GamSelMode(app) != 1) {
        MsgBox("提示", "请先选中一件物品(在右侧编辑界面)。", MB_ICONINFORMATION);
        return;
    }
    const poq::GamItem& it = app.gam_file.items[(size_t)app.gam_sel_item];
    if (GamIsNonEquipment(it)) {
        MsgBox("提示", "这是非装备记录（技能等），不支持复制属性。", MB_ICONINFORMATION);
        return;
    }
    app.gam_clip_fields.clear();
    for (const auto& f : poq::GamFile::FieldList()) {
        bool ok = false;
        int32_t v = it.GetField(f.name, &ok);
        if (ok) app.gam_clip_fields[f.name] = v;
    }
    app.gam_clip_ginfo = it.GetGInfo();
    app.gam_clip_name = GbkToUtf8Str(it.name);  // 名字(UTF-8, 供询问框显示)
    app.gam_clip_alias = it.alias;
    app.gam_has_attrs = true;
    app.SetStatus("已复制属性：" + it.alias);
}

static void GamOnPasteAttr(App& app) {
    if (GamSelMode(app) != 1) {
        MsgBox("提示", "请先选中一件物品(在右侧编辑界面)。", MB_ICONINFORMATION);
        return;
    }
    if (!app.gam_has_attrs) {
        MsgBox("提示", "剪贴板中没有属性，请先\"复制属性\"。", MB_ICONINFORMATION);
        return;
    }
    poq::GamItem& it = app.gam_file.items[(size_t)app.gam_sel_item];
    if (GamIsNonEquipment(it)) {
        MsgBox("提示", "这是非装备记录（技能等），属性编辑已禁用，不能粘贴属性。",
               MB_ICONINFORMATION);
        return;
    }
    // 先问"名字/别名/类型"要不要一起粘贴, 再动手(取消 = 什么都不做)
    int idx = app.gam_sel_item;
    std::string q = "来源物品:";
    if (!app.gam_clip_name.empty()) q += "\n  名字: " + app.gam_clip_name;
    if (!app.gam_clip_alias.empty()) q += "\n  别名: " + app.gam_clip_alias;
    {
        auto ft = app.gam_clip_fields.find("type");
        int tv = ft != app.gam_clip_fields.end() ? (int)ft->second : -1;
        q += "\n  类型: " + FieldValueDisplay(app, "类型", tv);  // 枚举显示"值：名字"
    }
    q += "\n\n这三项(名字/别名/类型)要不要一起粘贴？其余属性按所选方式粘贴。";
    App* ap = &app;
    MsgBoxAsk("粘贴属性", q, [ap, idx](int choice) {
        if (choice == 2) {
            ap->SetStatus("已取消粘贴属性(什么都没改)。");
            return;
        }
        ApplyGamPasteAttr(*ap, idx, choice == 0);
    });
}

// 真正做粘贴: withIdent=true 连名字/别名/类型一起; false 只粘贴其余属性。
static void ApplyGamPasteAttr(App& app, int idx, bool withIdent) {
    if (idx < 0 || idx >= (int)app.gam_file.items.size()) return;
    poq::GamItem& it = app.gam_file.items[(size_t)idx];
    bool ginfo_ok = true;
    for (const auto& kv : app.gam_clip_fields) {
        if (!withIdent && kv.first == "type") continue;  // 类型归入"名字/别名"一档
        it.SetField(kv.first.c_str(), kv.second);
    }
    if (!app.gam_clip_ginfo.empty()) ginfo_ok = it.SetGInfo(app.gam_clip_ginfo);
    if (withIdent) {
        if (!app.gam_clip_name.empty()) it.SetName(Utf8ToGbkStr(app.gam_clip_name));
        if (!app.gam_clip_alias.empty()) it.SetAlias(app.gam_clip_alias);
    }
    app.gam_edit_loaded_item = -1;  // 刷新界面
    std::string msg = std::string(withIdent ? "已粘贴属性+名字/别名/类型到 "
                                            : "已粘贴属性(名字/别名/类型保持原样)到 ") +
                      it.alias;
    if (!ginfo_ok) msg += "（词条文本无法原位覆盖，已保留原词条）";
    msg += "。记得点\"保存文件\"。";
    app.SetStatus(msg);
}

static void GamOnCreate(App& app) {
    if (!app.gam_cat_loaded || app.gam_sel_cat < 0 ||
        app.gam_sel_cat >= (int)app.gam_catalog.cats.size()) {
        MsgBox("提示", "物品目录未加载（缺少 gam_catalog.bin，请放 exe 目录或 data 子目录）。",
               MB_ICONINFORMATION);
        return;
    }
    const auto& ents = app.gam_catalog.items[(size_t)app.gam_sel_cat];
    if (ents.empty() || app.gam_sel_cat_item < 0 || app.gam_sel_cat_item >= (int)ents.size())
        return;
    if (GamSelMode(app) < 2) {
        MsgBox("提示", "请先在左侧选中一个地图(容器)。", MB_ICONINFORMATION);
        return;
    }
    const auto& e = ents[(size_t)app.gam_sel_cat_item];
    poq::GamItem it;
    if (!poq::GamItem::ParseRecord(e.rec, it)) {
        MsgBox("创建失败", "目录记录不是合法物品帧。", MB_ICONERROR);
        return;
    }
    it.is_new = true;
    auto& items = app.gam_file.items;
    int idx = (int)items.size();
    items.push_back(std::move(it));
    app.gam_file.containers[(size_t)app.gam_sel_container].item_idx.push_back(idx);
    app.gam_sel_item = idx;
    app.gam_edit_loaded_item = -1;
    GamPlaceNewItem(app, idx);  // 错开坐标避免叠位/不可见
    app.SetStatus("已创建物品：" + GamItemDispName(app, e.name));
}

static void RenderGamTree(App& app) {
    ImGui::BeginChild("gamtree", ImVec2(0.0f, ImGui::GetContentRegionAvail().y),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    static char filt[128] = {0};
    ImGui::TextUnformatted("地图(区域) -> 房间/物体 -> 物品：");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##gamfilt", filt, sizeof(filt));
    ImGui::Separator();
    ImGui::BeginChild("gamtree2");
    std::string lowf = ToLower(filt);

    auto seg = [](const std::string& path, int a, int b) {  // "/" 分割取 [a,b)
        std::vector<std::string> segs;
        size_t i = 0;
        while (true) {
            size_t j = path.find('/', i);
            if (j == std::string::npos) {
                segs.push_back(path.substr(i));
                break;
            }
            segs.push_back(path.substr(i, j - i));
            i = j + 1;
        }
        std::string out;
        for (int k = a; k < b && k < (int)segs.size(); ++k) {
            if (!out.empty()) out += "/";
            out += segs[(size_t)k];
        }
        return out;
    };
    // 构建分级结构(P1: 缓存进 App, 物品数变化才重建 —— 世界档 558 容器每帧构建太浪费)
    auto& regions = app.gam_regions;
    if ((int)app.gam_file.items.size() != app.gam_regions_items) {
        app.gam_regions.clear();
        app.gam_regions_items = (int)app.gam_file.items.size();
        std::map<std::string, int> ridx;
        std::string cur_region;
        for (int ci = 0; ci < (int)app.gam_file.containers.size(); ++ci) {
            const auto& c = app.gam_file.containers[(size_t)ci];
            if (c.item_idx.empty()) continue;
            std::string path = app.gam_scene_loaded ? app.gam_scene_cat.MatchPath(c.alias) : "";
            std::string region_disp, room_label;
            bool is_scene = false;
            if (!path.empty()) {
                region_disp = seg(path, 0, 2);  // 秦殇/邯郸
                room_label = seg(path, 3, 4);   // 武器铺
                if (room_label.empty()) room_label = c.alias;
                cur_region = region_disp;
                is_scene = true;
            } else {
                room_label = c.alias;  // 物体(未匹配场景)
                region_disp = cur_region.empty() ? "未分类" : cur_region;
            }
            if (ridx.find(region_disp) == ridx.end()) {
                ridx[region_disp] = (int)app.gam_regions.size();
                app.gam_regions.push_back(
                    App::GamRegion{region_disp, ToLower(region_disp), {}, {}});
            }
            App::GamRoom r{room_label, ci, ToLower(room_label)};
            if (is_scene)
                app.gam_regions[(size_t)ridx[region_disp]].rooms.push_back(std::move(r));
            else
                app.gam_regions[(size_t)ridx[region_disp]].objs.push_back(std::move(r));
        }
    }

    // 渲染
    // treenode 联动: 选中物品所在房间折叠时, 物品行根本不渲染 -> 选中态不可见。
    // 选中项变化后, 自动展开它所在的房间一次(用户手动折叠不会被反复顶开)。
    if (app.gam_sel_item != app.gam_tree_seen_sel) {
        app.gam_tree_seen_sel = app.gam_sel_item;
        app.gam_tree_open_cont = -1;
        if (app.gam_sel_item >= 0 && app.gam_sel_item < (int)app.gam_file.items.size()) {
            for (const auto& c : app.gam_file.containers) {
                for (int idx : c.item_idx) {
                    if (idx == app.gam_sel_item) {
                        app.gam_tree_open_cont = (int)(&c - app.gam_file.containers.data());
                        break;
                    }
                }
                if (app.gam_tree_open_cont >= 0) break;
            }
        }
    }
    auto item_matches = [&](int idx) {
        if (idx < 0 || idx >= (int)app.gam_file.items.size()) return false;
        const auto& it = app.gam_file.items[(size_t)idx];
        if (it.removed) return false;
        if (lowf.empty()) return true;
        // P1: 装载时已把 alias/显示名转好小写缓存, 每帧零分配
        return it.alias_low.find(lowf) != std::string::npos ||
               it.disp_low.find(lowf) != std::string::npos;
    };
    auto cont_live_count = [&](int ci) {
        int n = 0;
        for (int idx : app.gam_file.containers[(size_t)ci].item_idx)
            if (idx >= 0 && (size_t)idx < app.gam_file.items.size() &&
                !app.gam_file.items[(size_t)idx].removed)
                ++n;
        return n;
    };
    auto render_items = [&](int ci) {
        const auto& c = app.gam_file.containers[(size_t)ci];
        for (int idx : c.item_idx) {
            if (idx < 0 || idx >= (int)app.gam_file.items.size()) continue;
            auto& it = app.gam_file.items[(size_t)idx];
            if (it.removed) continue;
            if (!item_matches(idx)) continue;
            bool ok = false;
            int32_t lvl = it.GetField("lvl", &ok);
            std::string disp = GamItemDispName(app, it.alias);
            if (disp.size() > 220) disp = disp.substr(0, 220);
            char buf[320];
            std::string tag;
            if (it.is_new) tag += " [新增]";
            if (GamIsNonEquipment(it)) tag += " [非装备记录]";
            std::snprintf(buf, sizeof(buf), "(%d) %s  Lv%d%s##i%s%zX", idx, disp.c_str(),
                          ok ? (int)lvl : 0, tag.c_str(), it.is_new ? "n" : "",
                          it.is_new ? (size_t)idx : it.pos);
            ImGui::PushID(idx);
            if (ImGui::SmallButton("删除")) {  // 删除该物品(内存标记,保存时生效)
                it.removed = true;
                if (app.gam_sel_item == idx) {
                    app.gam_sel_item = -1;
                    app.gam_edit_loaded_item = -1;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("复制")) GamCopyItemAt(app, idx);  // 复制该物品记录
            ImGui::SameLine();
            bool sel = (app.gam_sel_item == idx);
            if (ImGui::Selectable(buf, sel)) {
                app.gam_sel_container = ci;
                app.gam_sel_item = idx;
                app.gam_edit_loaded_item = -1;
            }
            ImGui::PopID();
        }
    };
    auto render_group = [&](const App::GamRoom& room, const char* idpfx) {
        const auto& c = app.gam_file.containers[(size_t)room.cont];
        bool cont_sel = (app.gam_sel_container == room.cont && app.gam_sel_item < 0);
        char lab[320];
        std::snprintf(lab, sizeof(lab), "%s (%d)##%s%X", room.label.c_str(),
                      cont_live_count(room.cont), idpfx, (unsigned)(c.start & 0xFFFFFFFF));
        ImGuiTreeNodeFlags f =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (cont_sel) f |= ImGuiTreeNodeFlags_Selected;
        if (room.cont == app.gam_tree_open_cont) {
            ImGui::SetNextItemOpen(true);  // 选中联动: 本帧强制展开一次
            app.gam_tree_open_cont = -1;
        }
        bool open = ImGui::TreeNodeEx(lab, f);
        if (ImGui::IsItemClicked()) {  // 点标签=选中该容器;点箭头=仅展开(OpenOnArrow)
            app.gam_sel_container = room.cont;
            app.gam_sel_item = -1;
        }
        ImGui::SameLine();
        ImGui::PushID(1000000 + room.cont);
        if (ImGui::SmallButton("删除")) {  // 删该容器下所有物品
            for (int idx : c.item_idx)
                if (idx >= 0 && (size_t)idx < app.gam_file.items.size())
                    app.gam_file.items[(size_t)idx].removed = true;
            app.gam_sel_container = room.cont;
            app.gam_sel_item = -1;
            app.gam_edit_loaded_item = -1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("粘贴")) GamPasteInto(app, room.cont);  // 粘贴剪贴板物品到该容器
        ImGui::PopID();
        if (open) {
            render_items(room.cont);
            ImGui::TreePop();
        }
    };
    for (int ri = 0; ri < (int)regions.size(); ++ri) {
        const App::GamRegion& R = regions[(size_t)ri];
        // 过滤: 区域名 / 任一房间名 / 任一物品名 命中才显示
        bool any = lowf.empty() || R.disp_low.find(lowf) != std::string::npos;
        if (!any) {
            for (const auto& rm : R.rooms) {
                if (ToLower(rm.label).find(lowf) != std::string::npos) {
                    any = true;
                    break;
                }
                for (int idx : app.gam_file.containers[(size_t)rm.cont].item_idx)
                    if (item_matches(idx)) {
                        any = true;
                        break;
                    }
                if (any) break;
            }
        }
        if (!any) {
            for (const auto& ob : R.objs) {
                if (ToLower(ob.label).find(lowf) != std::string::npos) {
                    any = true;
                    break;
                }
                for (int idx : app.gam_file.containers[(size_t)ob.cont].item_idx)
                    if (item_matches(idx)) {
                        any = true;
                        break;
                    }
                if (any) break;
            }
        }
        if (!any) continue;
        std::string rlabel = R.disp + "##r" + std::to_string(ri);
        if (!ImGui::TreeNodeEx(rlabel.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth |
                                                   ImGuiTreeNodeFlags_DefaultOpen)) {
            continue;
        }
        for (const auto& room : R.rooms) render_group(room, "room");
        for (const auto& obj : R.objs) render_group(obj, "obj");
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

// 词条颜色 -> 中文标签
static const char kGamColorLetters[] = {0, 'K', 'C', 'Z', 'H'};

// 把当前物品的 G_Info 解析进编辑状态(隐藏头行, 只留词条)
static void GamLoadGInfoRows(App& app, poq::GamItem& it) {
    app.gam_grows.clear();
    it.ParseGInfoRows(app.gam_grows);
    app.gam_grow_sel.assign(app.gam_grows.size(), 0);  // 勾选状态随行数重置
    app.gam_gheader = it.GetGInfoHeader();
    // 记下"装载时的装配结果"。游戏写的文本有些行尾没空格, 而 BuildGInfoText 一律补一个,
    // 所以直接比对原文会误判成"改过"; 用装载时装配的结果做基准, 才能区分出真正的用户编辑。
    app.gam_gtext_loaded.clear();
    poq::GamItem::BuildGInfoText(app.gam_gheader, app.gam_grows, app.gam_gtext_loaded);
}

// 把编辑态的词条装配回 G_Info(等长/变长均可; 变长由保存时重排落盘)
static void GamCommitGInfo(App& app, poq::GamItem& it) {
    std::string txt;
    if (!poq::GamItem::BuildGInfoText(app.gam_gheader, app.gam_grows, txt)) return;
    if (txt == app.gam_gtext_loaded) return;  // 用户没动过词条: 不写, 免得把没改的物品标脏
    if (it.SetGInfo(txt)) app.gam_gtext_loaded = txt;
}

static void RenderGamEdit(App& app) {
    if (GamSelMode(app) != 1) {
        ImGui::TextDisabled("左侧选中地图后，点选一件物品进入编辑。");
        return;
    }
    poq::GamItem& it = app.gam_file.items[(size_t)app.gam_sel_item];
    // 非装备记录(技能等): 词条 id 不在装备属性表里, 编辑会破坏它 -> 直接不给改
    if (GamIsNonEquipment(it)) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "这是非装备记录（技能等），属性编辑已禁用。");
        ImGui::TextWrapped(
            "它的词条用的是另一套属性 id（0x10x/0x11x，如 驯兽最大数量 / "
            "召唤伙伴个数），不在装备属性表内。"
            "编辑器无法安全改写它的词条数组，所以这里只读；需要删除的话在左侧树上用\"删除\"。");
        if (app.gam_loaded) {
            ImGui::Separator();
            ImGui::Text("记录: %s  长度 %d  词条节点 %d", it.alias.c_str(), (int)it.content.size(),
                        (int)it.affixes.size());
            for (const auto& a : it.affixes)
                ImGui::BulletText("id 0x%03X  值 %g  颜色 %u  激活 %u", (unsigned)a.id, a.value,
                                  a.color, a.active);
        }
        return;
    }
    // 选中物品变化: 先提交上一件的词条编辑, 再载入新物品
    if (app.gam_edit_loaded_item != app.gam_sel_item) {
        if (app.gam_edit_loaded_item >= 0 &&
            app.gam_edit_loaded_item < (int)app.gam_file.items.size())
            GamCommitGInfo(app, app.gam_file.items[(size_t)app.gam_edit_loaded_item]);
        GamLoadGInfoRows(app, it);
        std::snprintf(app.gam_alias_buf, sizeof(app.gam_alias_buf), "%s", it.alias.c_str());
        std::snprintf(app.gam_name_buf, sizeof(app.gam_name_buf), "%s",
                      GbkToUtf8Str(it.name).c_str());
        app.gam_edit_loaded_item = app.gam_sel_item;
    }
    // 名称(标签放输入框左边)
    ImGui::Text("记录长度 %u 字节%s", it.len, it.is_new ? "   （新增，保存后写入所选地图）" : "");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("道具名字");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::InputText("##gamname", app.gam_name_buf, sizeof(app.gam_name_buf))) {
        if (!it.SetName(Utf8ToGbkStr(app.gam_name_buf)))  // 非法则还原
            std::snprintf(app.gam_name_buf, sizeof(app.gam_name_buf), "%s",
                          GbkToUtf8Str(it.name).c_str());
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("道具别名");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::InputText("##gamalias", app.gam_alias_buf, sizeof(app.gam_alias_buf),
                         ImGuiInputTextFlags_CharsNoBlank)) {
        if (!it.SetAlias(app.gam_alias_buf))  // 非法则还原
            std::snprintf(app.gam_alias_buf, sizeof(app.gam_alias_buf), "%s", it.alias.c_str());
    }
    ImGui::Separator();
    // 基础字段网格(3 列)
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = avail >= 800.0f ? 4 : (avail >= 500.0f ? 3 : 2);
    if (ImGui::BeginTable("gambasic", cols, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        const auto& bfs = app.cfg.Current().basic_fields;
        auto findCfg = [&](const char* label) -> const poq::BasicFieldCfg* {
            for (const auto& bf : bfs)
                if (bf.label == label) return &bf;
            return nullptr;
        };
        for (const auto& f : poq::GamFile::FieldList()) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.label);
            bool ok = false;
            int32_t v = it.GetField(f.name, &ok);
            int ed = ok ? (int)v : -1;
            const char* lbl = GamFieldToItemLabel(f.name);
            const poq::BasicFieldCfg* bf = lbl ? findCfg(lbl) : nullptr;
            std::string id = "##gam" + std::string(f.name);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (bf && !bf->enum_map.empty()) {
                // 枚举 -> 下拉框(与"道具修改"一致;enum_display_value 显示 "值：名字")
                std::vector<std::pair<int, std::string>> opts;
                for (const auto& kv : bf->enum_map)
                    opts.push_back({kv.first, bf->enum_display_value
                                                  ? std::to_string(kv.first) + "：" + kv.second
                                                  : kv.second});
                std::vector<const char*> items;
                for (auto& o : opts) items.push_back(o.second.c_str());
                int cur = -1;
                for (size_t j = 0; j < opts.size(); ++j)
                    if (opts[j].first == ed) {
                        cur = (int)j;
                        break;
                    }
                if (ImGui::Combo(id.c_str(), &cur, items.data(), (int)items.size())) {
                    if (cur >= 0) it.SetField(f.name, opts[(size_t)cur].first);
                }
            } else if (ImGui::InputInt(id.c_str(), &ed)) {
                if (bf) ed = bf->Clamp(ed);  // 合法校验与"道具修改"一致
                it.SetField(f.name, ed);
            }
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    // 词条: 一行一个属性(隐藏模板头), 保存时装配回 G_Info
    ImGui::TextUnformatted("词条（一行一个属性，保存时装配）：");
    const char* color_items[] = {"白色", "蓝色(K)", "亮蓝色(C,激活隐藏)", "金色(Z)",
                                 "暗蓝色(H,未激活隐藏)"};
    const char color_letters[] = {0, 'K', 'C', 'Z', 'H'};
    if (ImGui::BeginTable("grows", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        // 列顺序与"道具修改"一致: 选择 / 属性名 / 属性颜色 / 类型 / 属性值
        ImGui::TableSetupColumn("选择", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("属性名", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("属性颜色", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("属性值", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableHeadersRow();
        // 属性名下拉(显示同"道具修改"如 "29：普通攻击"); 用纯名别名映射匹配 G_Info 名
        static std::vector<std::string> s_disp;
        static std::vector<std::string> s_bare;
        if (s_disp.empty()) {
            for (auto& d : ItemData::AttrDisplayList()) {
                s_disp.push_back(d);
                s_bare.push_back(AttrBareNameU8(d));
            }
            // 下拉框只列"道具修改"表里的属性名 —— 不再额外补 GAM 专有名字:
            //   异常状态时间减少/失明攻击几率/失明时间/失明抗性 是映射结果名, 列出来是重复项;
            //   召唤伙伴个数/驯兽最大数量 映射不到属性 id, 写盘时会被从词条数组里丢掉(选中也没用)。
            // 存档里遇到的这类名字, 下拉框会用"原名置顶"的方式显示出来, 读取不受影响。
        }
        static const std::vector<std::string>& pct_list = ItemData::PercentDisplayListRef();
        static std::vector<const char*> pct_c;
        if (pct_c.empty())
            for (auto& t : pct_list) pct_c.push_back(t.c_str());
        for (int i = 0; i < (int)app.gam_grows.size(); ++i) {
            auto& r = app.gam_grows[(size_t)i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            if (ImGui::TableSetColumnIndex(0)) {
                bool bsel = (i < (int)app.gam_grow_sel.size()) && app.gam_grow_sel[(size_t)i];
                if (ImGui::Checkbox("##sel", &bsel) && i < (int)app.gam_grow_sel.size())
                    app.gam_grow_sel[(size_t)i] = bsel ? 1 : 0;
            }
            if (ImGui::TableSetColumnIndex(1)) {
                std::string name_u = GbkToUtf8Str(r.name);
                int cur = -1;
                for (size_t k = 0; k < s_bare.size(); ++k)
                    if (AttrGInfoAliasU8(s_bare[k]) == name_u) {
                        cur = (int)k;
                        break;
                    }
                std::vector<const char*> nc;
                std::string custom;
                int base = 0;
                if (cur < 0) {  // 未匹配: 原名置顶, 不显示为空
                    custom = name_u.empty() ? "（空）" : name_u;
                    nc.push_back(custom.c_str());
                    base = 1;
                    cur = 0;
                }
                for (auto& d : s_disp) nc.push_back(d.c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##nm", &cur, nc.data(), (int)nc.size())) {
                    int si = cur - base;
                    if (si >= 0 && si < (int)s_bare.size())
                        // 写 G_Info 名字(如 普通攻击力), 不是道具修改的名字(普通攻击)
                        r.name = Utf8ToGbkStr(AttrGInfoAliasU8(s_bare[(size_t)si]));
                }
            }
            if (ImGui::TableSetColumnIndex(2)) {
                int cc = 0;
                for (int x = 0; x < 5; ++x)
                    if (color_letters[x] == r.color) {
                        cc = x;
                        break;
                    }
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##col", &cc, color_items, 5)) r.color = color_letters[cc];
            }
            if (ImGui::TableSetColumnIndex(3)) {
                int pc = r.is_percent ? 1 : 0;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##pct", &pc, pct_c.data(), (int)pct_c.size()))
                    r.is_percent = (pc == 1);
            }
            if (ImGui::TableSetColumnIndex(4)) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                int v = (int)r.vmin;
                if (ImGui::InputInt("##v", &v)) r.vmin = v;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("添加属性")) {
        poq::GamItem::GInfoRow r{};
        r.color = 0;
        auto lst = ItemData::AttrDisplayList();  // 默认取第一个属性名(写 G_Info 名字形式, GBK)
        r.name =
            lst.empty() ? std::string() : Utf8ToGbkStr(AttrGInfoAliasU8(AttrBareNameU8(lst[0])));
        r.vmin = 0;
        r.has_max = false;
        r.is_percent = false;
        app.gam_grows.push_back(r);
        app.gam_grow_sel.push_back(0);
    }
    ImGui::SameLine();
    if (ImGui::Button("删除选中")) {  // 删除勾选的行
        for (int i = (int)app.gam_grows.size() - 1; i >= 0; --i) {
            if (i >= (int)app.gam_grow_sel.size() || !app.gam_grow_sel[(size_t)i]) continue;
            app.gam_grows.erase(app.gam_grows.begin() + i);
            app.gam_grow_sel.erase(app.gam_grow_sel.begin() + i);
        }
    }
    ImGui::NewLine();  // 提示另起一行
    ImGui::TextDisabled(
        "改完后点\"保存文件\"即装配写回；模板头已隐藏、保存时自动补回。数值/"
        "上限可任意改（变长会自动重排）。");
}

void RenderGamTab(App& app) {
    // 按钮行: 自动宽度 + 超宽自动换行(避免末尾按钮被挤出屏幕)
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    const float line_max_x = ImGui::GetContentRegionAvail().x;
    auto put_btn = [&](const char* text, auto fn) {
        float bw = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetCursorPosX() + bw + 12.0f > line_max_x) ImGui::NewLine();
        if (ImGui::Button(text)) fn();
        ImGui::SameLine(0, 8.0f);
    };
    put_btn("打开gam", [&] { GamOnOpen(app); });
    put_btn("保存文件", [&] { GamOnSave(app); });
    put_btn("复制物品", [&] { GamOnCopyItem(app); });
    put_btn("粘贴物品", [&] { GamOnPasteItem(app); });
    put_btn("复制属性", [&] { GamOnCopyAttr(app); });
    put_btn("粘贴属性", [&] { GamOnPasteAttr(app); });
    ImGui::PopStyleVar();
    ImGui::NewLine();  // 结束按钮行, 状态回下一行
    // 状态(独立一行, 不挤按钮)
    if (app.gam_loaded)
        ImGui::TextDisabled("%s  |  物品 %d  容器 %d", app.gam_path.c_str(),
                            (int)app.gam_file.items.size(), (int)app.gam_file.containers.size());

    // 创建新武器(目录): 类别定宽(固定), 具体物品占剩余宽度, 合上后悬停也显示属性
    ImGui::Separator();
    float row_x = ImGui::GetContentRegionAvail().x;
    const float lbl_w = ImGui::CalcTextSize("创建新武器：").x;
    const char* cat_preview =
        app.gam_cat_loaded ? app.gam_catalog.cats[(size_t)app.gam_sel_cat].c_str() : "(目录未加载)";
    const float cat_w = 96.0f;  // 类别下拉固定宽度(不随内容/选中项变化)
    const float btn_w =
        ImGui::CalcTextSize("创建物品").x + ImGui::GetStyle().FramePadding.x * 2.0f + 10.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float item_w = row_x - lbl_w - cat_w - btn_w - spacing * 4;
    if (item_w < 120.0f) item_w = 120.0f;  // 窗口过窄时物品框至少 120px
    ImGui::TextUnformatted("创建新武器：");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(cat_w);
    if (ImGui::BeginCombo("##cat", cat_preview)) {
        if (!app.gam_cat_loaded) ImGui::TextDisabled("缺少 gam_catalog.bin");
        for (int i = 0; i < (int)app.gam_catalog.cats.size(); ++i)
            if (ImGui::Selectable(app.gam_catalog.cats[(size_t)i].c_str(), i == app.gam_sel_cat))
                app.gam_sel_cat = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const auto& ents = (app.gam_cat_loaded && app.gam_sel_cat < (int)app.gam_catalog.cats.size())
                           ? app.gam_catalog.items[(size_t)app.gam_sel_cat]
                           : std::vector<poq::GamCatalog::Entry>();
    static std::string preview_buf;
    if (!ents.empty() && app.gam_sel_cat_item < (int)ents.size()) {
        std::string dm = GamItemDispName(app, ents[(size_t)app.gam_sel_cat_item].name);
        preview_buf = "Lv" + std::to_string(ents[(size_t)app.gam_sel_cat_item].level) + " " + dm;
    } else {
        preview_buf = "选择物品";
    }
    ImGui::SetNextItemWidth(item_w);
    bool item_combo_open =
        ImGui::BeginCombo("##item", preview_buf.c_str(), ImGuiComboFlags_HeightLarge);
    static int tip_i = -1;
    static std::string tip;
    if (item_combo_open) {
        for (int i = 0; i < (int)ents.size(); ++i) {
            std::string lab = "Lv" + std::to_string(ents[(size_t)i].level) + " " +
                              GamItemDispName(app, ents[(size_t)i].name);
            lab += "##ci" + std::to_string(i);  // 重复物品名(如多个东海石)不冲突
            bool sel = (i == app.gam_sel_cat_item);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Selectable(lab.c_str(), sel)) app.gam_sel_cat_item = i;
            if (ImGui::IsItemHovered()) {
                if (tip_i != i) {
                    tip = GamFormatItemTip(app, ents[(size_t)i].rec);
                    tip_i = i;
                }
                if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
            }
        }
        ImGui::EndCombo();
    } else {
        // 合上后悬停当前选中物品也显示属性
        if (ImGui::IsItemHovered() && !ents.empty() && app.gam_sel_cat_item >= 0 &&
            app.gam_sel_cat_item < (int)ents.size()) {
            if (tip_i != app.gam_sel_cat_item) {
                tip = GamFormatItemTip(app, ents[(size_t)app.gam_sel_cat_item].rec);
                tip_i = app.gam_sel_cat_item;
            }
            if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("创建物品")) GamOnCreate(app);
    // 提示独立一行, 避免把按钮挤出去
    ImGui::TextDisabled("（先选中左侧地图，再把目录物品新建到该地图；选中具体物品后可悬停看属性）");

    ImGui::Separator();

    if (!app.gam_loaded) {
        ImGui::TextDisabled("尚未打开 gam。点\"打开gam\"选择存档(默认定位到游戏目录)。");
        return;
    }

    // 左树 + 右编辑
    float avail = ImGui::GetContentRegionAvail().x;
    float w = avail * 0.42f;
    if (w < 260.0f) w = 260.0f;
    ImGui::BeginChild("gamleft", ImVec2(w, ImGui::GetContentRegionAvail().y),
                      ImGuiChildFlags_Borders);
    RenderGamTree(app);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("gamright", ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y),
                      ImGuiChildFlags_Borders);
    RenderGamEdit(app);
    ImGui::EndChild();
}

}  // namespace poq
