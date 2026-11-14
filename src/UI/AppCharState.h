#pragma once
// 人物 tab 专用状态(从 App 拆出): 含 CharValue / 锁原值缓存等。
// 仅 CharTab*、CharacterTabLogic、App.cpp 等需要访问处 include; App.h 不拉 Characters.h。

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "Core/Characters.h"
#include "UI/AppCommon.h"

namespace poq {

struct AppCharState {
  std::map<std::string, uintptr_t> char_addrs;
  std::vector<std::string> char_names;
  int char_selected = 0;
  std::vector<CharWidget> char_widgets;
  char money_buf[32] = {0};
  bool ck_invincible = false;
  bool ck_kill = false;
  std::map<std::string, std::map<std::string, CharValue>> def_cache;
  std::map<std::string, std::map<std::string, CharValue>> hit_cache;
  float lock_accum = 0.0f;
  std::string char_status;
  std::vector<std::string> char_slot_probe;
  bool char_filter = true;
  bool char_filter_warn = false;
};

}  // namespace poq
