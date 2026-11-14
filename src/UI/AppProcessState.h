#pragma once
// 进程列表与选中 PID(从 App 拆出): 含 ProcessInfo。
// TopBar / TabChar / try_get_selected_pid 等 include; App.h 不拉 ProcessMemory.h。

#include <string>
#include <vector>
#include "Native/ProcessMemory.h"

namespace poq {

struct AppProcessState {
  std::vector<ProcessInfo> processes;
  int proc_selected = -1;
  bool list_ready = false;
};

}  // namespace poq
