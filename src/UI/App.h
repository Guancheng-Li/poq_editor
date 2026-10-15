#pragma once
// 秦殇物品修改器 —— App 结构与跨文件共享声明(#18 自 src/main.cpp 拆分)。
// 各 UI 文件(TabChar/TabSummon/TabGm/TabGam/TabH2/Help/MainWindow/UiText)共用本头。

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "UI/UiText.h"

#include "Core/Characters.h"
#include "Core/GamCatalog.h"
#include "Core/GamFile.h"
#include "Core/GameModder.h"
#include "Core/ItemData.h"
#include "Core/ItemProperty.h"
#include "Core/OffsetsConfig.h"
#include "Native/ProcessMemory.h"

namespace poq {

// ---------------------------------------------------------------------------
// 打开时创建的备份登记: 退出时判定"这个文件打开后有没有被改过", 没改过就删掉它的备份,
// 避免反复打开堆一堆没用的 .bak; 改过的备份保留(那种是有用的)。
// 判定分两级(为了快):
//   1) 先比"大小 + 修改时间"(打开备份时记下的) —— 没变就说明没动过, 直接删, 不读文件
//      (NTFS 的修改时间是 100ns 精度, 同尺寸改写也必然改动它)
//   2) 变了才把两边算 MD5 做最终判定(改过但内容恰好一样的情况, 比如游戏重存了同内容)
// ---------------------------------------------------------------------------
struct OpenBackup {
    std::wstring src;    // 原文件
    std::wstring bak;    // 打开时生成的备份
    uint64_t size = 0;   // 备份时原文件的大小
    uint64_t mtime = 0;  // 备份时原文件的修改时间(FILETIME 原值)
};

// ---- 共享全局(定义在 UiText.cpp) ----
extern std::vector<OpenBackup> g_open_backups;
extern bool g_exit_asked;        // 本次关闭是否已经问过
extern bool g_exit_ask_pending;  // 主循环需要弹出"退出前是否清理备份"的询问
extern std::vector<MessageBoxMsg> g_msgbox_queue;

// 角色字段控件状态(当前选中角色一份;切换角色时从内存重填)。
struct CharWidget {
    int intval = 0;          // dword/word/enum
    int pair_lo = 0;         // pair 下限
    int pair_hi = 0;         // pair 上限
    char strbuf[128] = {0};  // string_ptr(UTF-8,足够 50 个汉字)
    bool loaded = false;     // 读取成功过; false = 该字段读失败, 禁止保存(否则误写 0)
    bool dirty = false;      // 用户改动过(为将来 dirty-save 预留)
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

    // ---- 捕获后冻结 ----
    // 「读取属性」按下后:解冻->清槽->等一次捕获(手持物品被游戏读取)->读取->冻结。
    // 识别只发生一次;冻结后不再被悬停/场景读取顶掉。

    // ---- 上一件物品属性缓存 ----
    // 每次读取(或保存)成功时快照一份;"复制上一件物品"把它写到当前物品上。
    std::vector<ItemProperty> last_attrs;
    std::string last_name, last_alias;  // 上一件物品的名字/别名(复制时按需带过来)
    int last_type = -1;                 // 上一件物品的类型(同上)
    uintptr_t last_item_base = 0;

    // ---- 使用说明弹窗 ----
    // cur_tab:当前主 tab 下标(0=人物 1=道具 2=召唤 3=GM 4=H2,5/6=调试 tab);
    // help_jump/help_tab:点"使用说明"瞬间要跳到的 tab(仅弹窗打开后第一帧生效)。
    int cur_tab = 0;
    int help_tab = 0;
    bool help_jump = false;

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

    // ---- 属性修改(角色)----
    std::map<std::string, uintptr_t> char_addrs;  // 姓名 -> 结构体地址
    std::vector<std::string> char_names;          // 下拉顺序(角色x-名字)
    int char_selected = 0;
    std::vector<CharWidget> char_widgets;  // 平行 CharFields,当前选中角色
    char money_buf[32] = {0};              // 全局金钱输入
    bool ck_invincible = false;            // 无敌模式
    bool ck_kill = false;                  // 一击必杀
    // 锁原值缓存:姓名 -> {label: 原值}(勾选时缓存,取消时恢复)
    std::map<std::string, std::map<std::string, CharValue>> def_cache;  // 防御/闪避
    std::map<std::string, std::map<std::string, CharValue>> hit_cache;  // 攻击/命中率
    float lock_accum = 0.0f;                   // 锁定时器累加(500ms 写回一次)
    std::string char_status;                   // 角色 tab 状态文本
    std::vector<std::string> char_slot_probe;  // F12 调试:槽位名字探测(不过滤)
    bool char_filter = true;                   // 角色属性校验过滤(白名单+等级),默认开启
    bool char_filter_warn = false;             // 关闭过滤时的警告弹窗待显示

    // ---- 召唤修改(task7)----
    bool summon_active = false;     // 激活态(激活后按钮变"取消",刷新可点)
    std::string summon_log;         // 召唤工具输出(F12 调试模式显示)
    std::string summon_mode_cache;  // 上次渲染的版本,检测切换并重置激活态
    int sm_group = 0;               // 召唤归属: 0=队友可控(默认) 1=盟友不可控
    int sm_kind = 0;                // steam 三选一: 0=野兽 1=刺客 2=秦兵 (-1=全不选)
    int sm_mon = 0;                 // steam 野兽(0-9)
    int sm_assassin = 0;            // steam 刺客(0=弓箭 1=带刀)
    int sm_soldier = 0;             // steam 秦兵(0=随机 1-6=六大类)
    std::vector<int> sm_sub_items;  // 当前秦兵大类的子类(原索引列表,过滤旧重剑)
    int sm_sub = -1;                // 选中子类(原索引)
    int sm130_kind = 0;             // 1.30 二选一: 0=野兽 1=随机秦兵 (-1=全不选)
    int sm130_mon = 0;              // 1.30 野兽(0-7,朱厌/善翼不显示)

    // ---- GM命令(task8)----
    bool gm_active = false;       // 当前 GM 是否已激活(is_gm_ready 决定)
    std::string gm_log;           // 工具日志(F12 调试模式显示)
    std::string gm_mode_cache;    // 上次渲染的版本,检测切换并重置激活态
    int gm_method = 0;            // 激活方式: 0=内存修改 1=R9注入(默认内存修改)
    char gm_cmd_buf[256] = {0};   // 命令输入框(UTF-8)
    bool ck_count = false;        // 执行次数
    char gm_count_buf[8] = {0};   // 执行次数输入(默认 "1")
    float gm_timer_accum = 0.0f;  // 2s 周期刷新 GM 状态(仅 GM tab 可见时跑)
    bool gm_visible = false;      // GM tab 当前是否显示
    std::vector<std::pair<std::string, std::string>> gm_cmd_list;  // 常用命令 (命令, 显示名)
    int gm_cmdlist_sel = 0;                                        // 常用命令下拉当前选中项
    // 命令历史(上下箭头切换):最新在末尾,最多 30 条;idx=-1 表示未在浏览。
    std::vector<std::string> gm_history;
    int gm_hist_idx = -1;
    std::string gm_hist_draft;        // 开始浏览瞬间保存的输入框原文(下翻回最末时还原)
    bool gm_cmdlist_focused = false;  // 常用命令列表当前是否持有焦点(持有时箭头留给列表)

    // ---- H2解密(task10)----
    std::wstring h2_path;              // 待转换文件(已有备份时等待用户选择)
    bool h2_decrypt = true;            // 待转换方向: true=解密 false=加密
    bool h2_wait_confirm = false;      // 已有备份,等待覆盖/跳过/取消选择
    std::wstring h2_dir;               // 待批量转换目录(等待确认)
    bool h2_dir_decrypt = true;        // 目录转换方向: true=解密 false=加密
    bool h2_wait_dir_confirm = false;  // 目录转换确认弹窗待定

    // ---- GAM修改(task13)----
    poq::GamFile gam_file;  // 已加载存档
    std::string gam_path;
    bool gam_loaded = false;
    int gam_sel_container = -1;            // 容器下标
    int gam_sel_item = -1;                 // 选中物品下标(gam_file.items)
    int gam_tree_open_cont = -1;           // treenode: 选中变化后待自动展开的容器(一次性)
    int gam_tree_seen_sel = -2;            // treenode: 上次做展开联动时的选中项
    int gam_edit_loaded_item = -1;         // 词条编辑缓冲已加载的物品下标
    std::vector<uint8_t> gam_clip_record;  // 复制物品(整条记录字节)
    bool gam_has_record = false;
    std::map<std::string, int64_t> gam_clip_fields;  // 复制属性(固定字段快照)
    std::string gam_clip_ginfo;                      // 复制属性(G_Info GBK 文本,空=无)
    std::string gam_clip_name, gam_clip_alias;       // 复制属性(名字 UTF-8 / 别名)
    bool gam_has_attrs = false;
    poq::GamCatalog gam_catalog;  // 无中生有数据源(全装备目录)
    bool gam_cat_loaded = false;
    int gam_sel_cat = 0;                                          // 创建物品: 类别
    int gam_sel_cat_item = 0;                                     // 创建物品: 条目
    std::vector<std::pair<int, std::vector<uint8_t>>> gam_added;  // 保存时插入(容器,记录)
    std::string gam_last_dir;                                     // 上次打开 gam 的目录(记忆)
    poq::SceneCatalog gam_scene_cat;  // 场景全名目录(容器别名->完整场景路径)
    bool gam_scene_loaded = false;
    // GAM 树分级缓存(P1): 结构变化时重建, 不再每帧构建
    struct GamRoom {
        std::string label;
        int cont;
        std::string label_low;
    };
    struct GamRegion {
        std::string disp;
        std::string disp_low;
        std::vector<GamRoom> rooms;
        std::vector<GamRoom> objs;
    };
    std::vector<GamRegion> gam_regions;
    int gam_regions_items = -1;
    poq::ItemNames gam_item_names;  // 物品中文名(别名->中文)
    bool gam_item_names_loaded = false;
    char gam_ginfo_buf[32768] = {0};                // 词条文本编辑缓冲(UTF-8)
    bool gam_ginfo_pending = false;                 // 词条已改待保存
    std::vector<poq::GamItem::GInfoRow> gam_grows;  // 当前物品词条(编辑用, 隐藏头行)
    std::vector<uint8_t> gam_grow_sel;              // 与 gam_grows 平行的勾选状态
    std::string gam_gheader;                        // 当前物品词条头行(GBK, 保存时装配回)
    std::string gam_gtext_loaded;                   // 装载时装配出的 G_Info(判断用户是否真的改过)
    char gam_alias_buf[64] = {0};                   // 物品别名(拼音)编辑缓冲
    char gam_name_buf[128] = {0};                   // 物品中文名编辑缓冲(UTF-8)

    // ---- 状态 / 静态选项 ----
    bool hidden = true;        // F12 前隐藏调试 tab
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
        // 无中生有物品目录: exe 目录 / data 子目录 / 开发目录 ../../data(全装备解析产物)
        if (gam_catalog.Load({WideToUtf8(AppDirW() + L"\\gam_catalog.bin"),
                              WideToUtf8(AppDirW() + L"\\data\\gam_catalog.bin"),
                              WideToUtf8(AppDirW() + L"\\..\\..\\data\\gam_catalog.bin")},
                             gam_catalog)) {
            gam_cat_loaded = true;
        }
        if (gam_scene_cat.Load({WideToUtf8(AppDirW() + L"\\scene_catalog.bin"),
                                WideToUtf8(AppDirW() + L"\\data\\scene_catalog.bin"),
                                WideToUtf8(AppDirW() + L"\\..\\..\\data\\scene_catalog.bin")},
                               gam_scene_cat)) {
            gam_scene_loaded = true;
        }
        if (gam_item_names.Load({WideToUtf8(AppDirW() + L"\\item_names.bin"),
                                 WideToUtf8(AppDirW() + L"\\data\\item_names.bin"),
                                 WideToUtf8(AppDirW() + L"\\..\\..\\data\\item_names.bin")},
                                gam_item_names)) {
            gam_item_names_loaded = true;
        }
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
        if (processes.empty()) msg += "未找到 POQ.EXE,请先启动游戏后点\"刷新进程\"。";
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
        std::snprintf(cfg_weight_offset, sizeof(cfg_weight_offset), "%s",
                      HexStr(hk.weight_offset).c_str());
        std::snprintf(cfg_weight_max, sizeof(cfg_weight_max), "%u", hk.weight_max);
        std::snprintf(cfg_item_adjust, sizeof(cfg_item_adjust), "%d", m.item_base_adjust);
        std::snprintf(cfg_head, sizeof(cfg_head), "%s", HexStr(al.head_offset).c_str());
        std::snprintf(cfg_next, sizeof(cfg_next), "%s", HexStr(al.next_offset).c_str());
        std::snprintf(cfg_id, sizeof(cfg_id), "%s", HexStr(al.id_offset).c_str());
        std::snprintf(cfg_color, sizeof(cfg_color), "%s", HexStr(al.color_offset).c_str());
        std::snprintf(cfg_precent, sizeof(cfg_precent), "%s", HexStr(al.precent_offset).c_str());
        std::snprintf(cfg_value, sizeof(cfg_value), "%s", HexStr(al.value_offset).c_str());
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

// ---------------------------------------------------------------------------
// 跨文件使用的函数声明(实现在各 .cpp)
// ---------------------------------------------------------------------------

// ---- TabChar.cpp: 进程/初始化/道具逻辑/角色逻辑/道具 UI ----
std::optional<unsigned int> TryGetSelectedPid(const App& app);
std::optional<unsigned int> FindPoqPid(const App& app);
void OnInit(App& app);
void OnRawRead(App& app);
void OnRawWrite(App& app);
void OnSaveCfg(App& app);
std::string FieldValueDisplay(const App& app, const char* label, int v);
void TickLock(App& app, float dt);
void RenderBasicFields(App& app);
void RenderAttrTable(App& app, float height);
void RenderCharTab(App& app);

// ---- TabSummon.cpp: 召唤修改 ----
void RenderSummonTab(App& app);

// ---- TabGm.cpp: GM 命令 ----
int LoadGmCmdList(App& app, bool keep_previous_on_error);
void TickGm(App& app, float dt);
void RenderGmTab(App& app);

// ---- TabGam.cpp: GAM 存档修改 ----
std::wstring TimestampedBakPath(const std::wstring& path);
std::string GamLoadPrefs();
void RenderGamTab(App& app);

// ---- TabH2.cpp: H2 加解密 ----
void RenderH2Tab(App& app);

// ---- MainWindow.cpp: 主窗口 ----
void ApplyBlueTheme();
void RenderMainWindow(App& app);

// ---- Help.cpp: 版本/欢迎弹窗/消息队列/使用说明 ----
void RenderWelcomeDialog(App& app);
void RenderMsgQueue();
void RenderHelpPopup(App& app);

}  // namespace poq
