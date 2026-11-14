#pragma once
// 秦殇物品修改器 —— App 结构与跨文件共享声明(#18 自 src/main.cpp 拆分)。
// 各 UI 文件(TabChar/TabSummon/TabGm/TabGam/TabH2/Help/MainWindow/UiText)共用本头。

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "UI/AppCommon.h"

namespace poq {

struct AppGamState;
struct AppCharState;
struct AppCfgState;
struct AppItemState;
struct AppProcessState;
class GameModder;

struct App {
  std::unique_ptr<AppCfgState> cfg_;
  AppCfgState& cfg() noexcept;
  const AppCfgState& cfg() const noexcept;

  std::unique_ptr<GameModder> modder_;
  GameModder& modder() noexcept;
  const GameModder& modder() const noexcept;

  std::unique_ptr<AppItemState> item_;
  AppItemState& item() noexcept;
  const AppItemState& item() const noexcept;

  std::unique_ptr<AppProcessState> proc_;
  AppProcessState& proc() noexcept;
  const AppProcessState& proc() const noexcept;

  // ---- 使用说明弹窗 ----
  // cur_tab:当前主 tab 下标(0=人物 1=道具 2=召唤 3=GM 4=H2,5/6=调试 tab);
  // help_jump/help_tab:点"使用说明"瞬间要跳到的 tab(仅弹窗打开后第一帧生效)。
  int cur_tab = 0;
  int help_tab = 0;
  bool help_jump = false;

  // ---- 道具/内存查看: AppItemState; 进程栏: AppProcessState ----

  // ---- 偏移/模式: 见 AppCfgState —— App.h 不 include OffsetsConfig.h。----

  // ---- 属性修改(角色): 见 AppCharState —— App.h 不 include Characters.h。----
  std::unique_ptr<AppCharState> char_;
  AppCharState& char_state() noexcept;
  const AppCharState& char_state() const noexcept;

  // ---- 召唤修改(task7)----
  bool summon_active = false;     // 激活态(激活后按钮变"取消",刷新可点)
  std::string summon_log;         // 召唤工具输出(F12 调试模式显示)
  std::string summon_mode_cache;  // 上次渲染的版本,检测切换并重置激活态
  int sm_group = 0;               // 召唤归属: 0=队友 1=盟友(v1.30 另含 2=敌方)
  int sm_count = 0;               // v1.30: 一次召唤最多造几只(0=不干预, 用技能原始数量=5)
  float summon_hp_accum = 0.0f;   // 激活后补满召唤血槽(0.5s 间隔)
  int sm_kind = 0;                // 单选: 0=野兽 1=刺客 2=秦兵 3=其它(v1.30)
  int sm_mon = 0;                 // 野兽 mon_type(0-9, 两版工具均支持)
  bool sm_beast_wild = false;     // v1.30: 朱厌/善翼从类型表 field[0] 选模板
  int sm_beast_sub = 0;           // --beast_sub_type 索引
  std::vector<std::string> sm_beast_wild_items;  // 类型表扫描结果(下拉文案)
  int sm_misc = 0;                               // sm_kind==3 时下拉索引
  char sm_misc_search[64] = "";                  // 召唤其它: 模糊搜索(名称/别名/rec)
  std::vector<int> sm_misc_recs;                 // 与 sm_misc_items 一一对应 type_table rec
  std::vector<std::string> sm_misc_items;
  int sm_assassin = 0;            // 刺客(0=弓箭 1=带刀)
  int sm_soldier = 0;             // 秦兵(0=随机 1-6=六大类)
  std::vector<int> sm_sub_items;  // 当前秦兵大类的子类(原索引; Steam 过滤旧重剑)
  int sm_sub = -1;                // 选中子类(原索引)

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
  // 连发执行(间隔 100ms): 由 tick_gm 驱动, 不 Sleep 卡 UI。
  bool gm_exec_pending = false;
  std::string gm_exec_cmd;
  int gm_exec_remaining = 0;  // 首轮已在 on_gm_exec 发送, 此处为剩余次数
  int gm_exec_total = 0;
  float gm_exec_gap_accum = 0.0f;

  // ---- H2解密(task10)----
  std::wstring h2_path;              // 待转换文件(已有备份时等待用户选择)
  bool h2_decrypt = true;            // 待转换方向: true=解密 false=加密
  bool h2_wait_confirm = false;      // 已有备份,等待覆盖/跳过/取消选择
  std::wstring h2_dir;               // 待批量转换目录(等待确认)
  bool h2_dir_decrypt = true;        // 目录转换方向: true=解密 false=加密
  bool h2_wait_dir_confirm = false;  // 目录转换确认弹窗待定

  // ---- GAM修改(task13): GamFile 等见 AppGamState —— App.h 不 include GamFile.h;
  // 改词条/存档 Core 时仅 GamTab* + App.cpp 重编, char/GM/道具等 tab 不受影响。----
  std::unique_ptr<AppGamState> gam_;
  AppGamState& gam() noexcept;
  const AppGamState& gam() const noexcept;

  // ---- 状态 ----
  bool hidden = true;        // F12 前隐藏调试 tab
  bool show_welcome = true;  // 启动免责声明弹窗(每次打开应用显示一次)
  std::string status;

  App();   // 定义见 App.cpp
  ~App();  // pimpl 状态完整类型在 App.cpp
  App(const App&) = delete;
  App& operator=(const App&) = delete;
  App(App&&) = default;
  App& operator=(App&&) = default;

  // 首次打开 gam tab 再加载 data\*.bin, 缩短冷启动(约 900KB 解析)。
  void ensure_gam_data_loaded();  // 定义见 App.cpp
  void ensure_process_list();     // 定义见 App.cpp

  // 重建基础属性控件(配置变化时)
  void rebuild_basic();  // 定义见 App.cpp

  // 刷新进程列表,只列 POQ.EXE(自动选中第一个,避免全系统进程干扰选择)
  void refresh_processes();   // 定义见 App.cpp
  void reload_cfg_buffers();  // 定义见 App.cpp

  // 当前"类型"字段值(enum 取 enum_val,否则 intval)
  std::optional<int> current_type_value() const;  // 定义见 App.cpp

  void set_status(const std::string& msg);  // 定义见 App.cpp
};
}  // namespace poq

#include "UI/ApiChar.h"    // 人物/道具 tab 跨文件声明
#include "UI/ApiGam.h"     // GAM 存档 tab
#include "UI/ApiGm.h"      // GM 命令 tab
#include "UI/ApiH2.h"      // H2 加解密 tab
#include "UI/ApiMain.h"    // 主窗口 / 帮助
#include "UI/ApiSummon.h"  // 召唤 tab
