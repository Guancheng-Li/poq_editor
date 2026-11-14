#pragma once
// 秦殇物品修改器 —— UI 共享类型(App 之外的通用小类型与全局)。
// 从 App.h 拆出(阶段二 §4): 让只需要"App 状态"的文件不必拉起整个 App.h。

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "UI/UiText.h"  // MessageBoxMsg

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

// 枚举下拉项只在配置变化时生成; Combo 的 getter 直接读取缓存字符串。
struct EnumComboOptions {
  std::vector<int> values;
  std::vector<std::string> labels;

  void build(const std::map<int, std::string>& names, bool show_value) {
    values.clear();
    labels.clear();
    values.reserve(names.size());
    labels.reserve(names.size());
    for (const auto& kv : names) {
      values.push_back(kv.first);
      labels.push_back(show_value ? std::to_string(kv.first) + "：" + kv.second : kv.second);
    }
  }

  static const char* label_at(void* data, int idx) {
    const auto& options = *static_cast<const EnumComboOptions*>(data);
    if (idx < 0 || static_cast<size_t>(idx) >= options.labels.size()) return nullptr;
    return options.labels[static_cast<size_t>(idx)].c_str();
  }

  int index_of(int value) const {
    for (size_t i = 0; i < values.size(); ++i)
      if (values[i] == value) return static_cast<int>(i);
    return -1;
  }
};

}  // namespace poq
