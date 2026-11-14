#pragma once
// 偏移/模式配置与相关 UI 缓冲(从 App 拆出): 含 OffsetsConfig。
// 仅 TabChar、MainWindow*、item/gam tab、App.cpp 等需要访问处 include; App.h 不拉 OffsetsConfig.h。

#include <cstdio>
#include <string>
#include <vector>
#include "Core/OffsetsConfig.h"
#include "UI/AppCommon.h"

namespace poq {

struct AppCfgState {
  OffsetsConfig cfg = OffsetsConfig::default_config();

  /// 当前模式下的道具基础字段(与 cfg.current().basic_fields 同步; 道具 tab 用)。
  std::vector<BasicFieldCfg> basic_fields;
  /// GAM 编辑 tab: 与当前模式 basic_fields 平行的 enum 下拉缓存。
  std::vector<EnumComboOptions> gam_enum_options;

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
};

}  // namespace poq
