#pragma once

#include <string>
#include <vector>

namespace poq {

struct App;

extern const char* kSummonMonNames[10];
extern const char* kSummonAssassinNames[2];

struct SummonCatUi {
  const char* name;
  std::vector<const char*> subs;
};

const std::vector<SummonCatUi>& summon_qin_cats();
std::vector<std::string> make_mon_labels(const char* const* names, int count);
std::string summon_current_selection(const App& app);

}  // namespace poq
