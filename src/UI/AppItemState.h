#pragma once
// 道具 tab 状态(从 App 拆出): 含 ItemProperty、基础字段控件、原始内存查看缓冲等。
// ItemTab* / TabChar(读写) / MainWindowAuxTabs 等 include; App.h 不拉 ItemProperty.h。

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "Core/ItemProperty.h"
#include "UI/AppCommon.h"

namespace poq {

struct AppItemState {
  std::vector<EnumComboOptions> basic_enum_options;  // 与 AppCfgState::basic_fields 平行

  struct BasicWidget {
    bool is_enum = false;
    int intval = 0;
    int enum_val = 0;
    char strbuf[256] = {0};
  };
  std::vector<BasicWidget> basic;

  std::vector<ItemProperty> attrs;
  std::vector<bool> attr_sel;

  bool item_read_pending = false;
  float item_read_accum = 0.0f;

  std::vector<ItemProperty> last_attrs;
  std::string last_name;
  std::string last_alias;
  int last_type = -1;
  uintptr_t last_item_base = 0;

  char raw_addr[64] = {0};
  char raw_size[16] = {0};
  char raw_hex[131072] = {0};

  std::vector<uint32_t> attr_ids;
  std::vector<uint32_t> color_ids;
  std::vector<uint32_t> percent_ids;
};

}  // namespace poq
