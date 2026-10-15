# PoqEditor ImGui 优化与修复计划

日期：2026-09-29  
范围：`poqeditor_imgui`  
目标：优先修复可能导致游戏崩溃、远程内存破坏、hook 残留和错误写入的问题；随后补齐错误处理、资源生命周期和可观测性；最后按 Google C++ Style Guide 逐步统一代码风格并拆分过大的模块。

---

## 1. 总体原则

本轮优化按“先保证目标进程安全，再保证数据正确，再改善工程质量”的顺序执行。

优先级定义：

- **P0**：可能直接导致游戏崩溃、执行流跳转到无效内存、明显破坏目标进程状态。
- **P1**：可能导致错误写入、注入状态泄漏、远程资源未清理、部分失败后状态不一致。
- **P2**：错误处理、可观测性、初始化健壮性、非原子保存等问题。
- **P3**：Google C++ Style、文件组织、可维护性、编译告警等工程质量问题。

建议实施顺序：

1. 修复角色“视野”误写。
2. 修复 hook 恢复失败后仍释放 code cave。
3. 修复读取失败字段被保存为 0。
4. 修复 PID 切换和注入失败时的状态管理。
5. 修复 `ProcessMemory::Write()` 的页面保护和完整写检查。
6. 调整远程线程 timeout 行为。
7. 修复字符串原地写的容量模型。
8. 改善保存失败报告与事务性。
9. 加固 D3D/Win32/ImGui 初始化。
10. 分批落地 Google C++ Style 和模块拆分。

---

## 2. P0：角色“视野”字段必须真正只读

### 2.1 问题

`Characters.cpp` 已明确注明 `+0x198` 的“视野”字段直接通过 `WriteProcessMemory` 修改会导致游戏崩溃，并要求该字段只读。

当前实现仍有两层漏洞：

- `CharFieldCfg` 没有字段级的 `writable/read_only` 元数据。
- `OnCharSave()` 最终遍历全部 `CharFields()`，因此即使 UI 把某字段禁用，保存逻辑仍可能写它。

这意味着“UI 只读”和“后端只读”没有绑定在一起。

### 2.2 建议设计

在 `CharFieldCfg` 中增加字段级写权限：

```cpp
struct CharFieldCfg {
  std::string label;
  uint32_t offset_lo = 0;
  uint32_t offset_hi = 0;
  CharKind kind = CharKind::Dword;
  std::map<int, std::string> enum_map;
  std::optional<int> min;
  std::optional<int> max;
  std::optional<std::string> hint;
  bool writable = true;
};
```

“视野”定义显式设置为不可写：

```cpp
CharFieldCfg{
    "视野",
    0x198,
    0,
    CharKind::Dword,
    {},
    0,
    99999,
    std::string("运行中只读，直接写入可能导致游戏崩溃"),
    false,
},
```

### 2.3 UI 层

不要让 `IsCharFieldLocked()` 同时承担字段固有权限和模式锁定。建议改成：

```cpp
bool IsCharFieldEditable(const App& app, const CharFieldCfg& field) {
  if (!field.writable) {
    return false;
  }

  if (app.ck_invincible) {
    // 根据 field.label 判断无敌模式锁定。
  }

  if (app.ck_kill) {
    // 根据 field.label 判断一击必杀模式锁定。
  }

  return true;
}
```

这样 UI disabled 状态直接来自同一份字段定义。

### 2.4 保存层

保存时必须再次检查：

```cpp
for (const auto& field : CharFields()) {
  if (!field.writable) {
    continue;
  }
  ...
}
```

同时建议 `WriteCharField()` 自身也增加最后一道防线：

```cpp
if (!field.writable) {
  return false;
}
```

这样即使以后新增别的调用路径绕过 UI，也不会误写只读字段。

### 2.5 验证

- 正常读取角色属性，确认“视野”可显示。
- 确认“视野”控件始终 disabled。
- 修改其他字段后保存。
- 调试日志确认没有对 `base + 0x198` 发生写入。
- 连续保存多次，确认不因该字段写入导致游戏崩溃。

---

## 3. P0：hook 恢复失败时禁止释放 code cave

### 3.1 问题

当前 `GameModder::Close()` 大致按以下步骤：

1. 尝试写回原始 hook 字节。
2. 捕获异常并忽略。
3. 尝试释放 `alloc_base_`。
4. 清空本地状态并关闭进程句柄。

如果 hook 恢复失败，游戏中的 `E9` 仍然指向 code cave；而 cave 一旦被成功释放，目标进程下一次执行到 hook 点就可能跳入已释放内存。

### 3.2 修复原则

核心不变量：

> 只要 hook 仍指向 cave，cave 就绝不能释放。

建议流程：

```text
if 没有本工具注入:
    普通清理
else:
    尝试恢复 hook
    读回验证 hook
    如果恢复失败:
        保留 cave
        保留必要状态
        返回/记录错误
    如果恢复成功:
        释放 cave
```

### 3.3 建议返回结果

不要让 `Close()` 完全吞掉关键异常。

可以定义：

```cpp
struct CloseResult {
  bool hook_restored = true;
  bool cave_freed = true;
  bool pool_freed = true;
  std::string error;
};
```

或者分成：

```cpp
void Close();        // 正常路径，可以报告失败
void CloseNoThrow(); // 析构兜底路径
```

### 3.4 恢复后的读回验证

建议：

```cpp
mem_.Write(hook_addr_, orig_bytes_);
const auto verify = mem_.Read(hook_addr_, orig_bytes_.size());
if (verify != orig_bytes_) {
  throw std::runtime_error("hook 恢复后校验失败");
}
```

只有校验成功后才允许 `mem_.Free(alloc_base_)`。

### 3.5 恢复失败处理

如果恢复失败：

- 不释放 `alloc_base_`。
- 不立即清空 `hook_addr_`、`orig_bytes_` 等恢复所需状态。
- UI 提示 hook 清理失败。
- 后续修改功能应停止，避免进一步污染状态。
- 如果确认目标进程已经退出，则远程内存会随进程结束自动释放，可以只清本地状态。

---

## 4. P1：读取失败字段不能默认保存为 0

### 4.1 问题

`RebuildCharWidgets()` 会先创建全零 `CharWidget`：

```cpp
app.char_widgets.assign(CharFields().size(), CharWidget());
```

某字段读取失败时直接 `continue`，因此该 widget 仍保持默认 0。

之后 `OnCharSave()` 会收集全部 widget 并写回，可能把“读取失败”误解释为“值为 0”。

### 4.2 建议状态

```cpp
struct CharWidget {
  bool loaded = false;
  bool dirty = false;

  int intval = 0;
  int pair_lo = 0;
  int pair_hi = 0;
  char strbuf[128] = {0};
};
```

读取成功后才设置：

```cpp
widget.loaded = true;
```

### 4.3 UI 行为

读取失败字段：

- disabled。
- 显示“读取失败”提示。
- debug 模式记录字段名和地址。
- 不参与保存。

### 4.4 保存条件

建议最终只写满足以下条件的字段：

```text
field.writable == true
AND widget.loaded == true
AND widget.dirty == true
```

如果暂时不引入 `dirty`，至少要保证 `writable && loaded`。

---

## 5. P1：切换 PID 前必须关闭旧进程状态

### 5.1 问题

当前 `GameModder::Initialize(pid)` 在 PID 变化时直接：

```cpp
if (pid_ != pid) ResetState();
```

这会丢失旧进程的：

- hook 地址。
- 原始指令。
- cave 地址。
- pool 地址。
- 注入状态。

如果旧进程仍存活，这些远程修改会被遗留。

### 5.2 推荐流程

```cpp
if (pid_ != 0 && pid_ != pid) {
  Close();
}

OpenNewProcess(pid);
InitializeNewState();
```

如果旧进程仍活着但 hook 恢复失败，应先报告清理失败，不要静默丢掉旧状态后继续。

### 5.3 状态机

建议显式维护：

```cpp
enum class ModderState {
  kClosed,
  kProcessOpened,
  kInjected,
  kClosingFailed,
};
```

比根据多个地址是否为 0 推断状态更可靠。

---

## 6. P1：Inject 必须具备 rollback 语义

### 6.1 问题

当前 `Inject()` 会依次：

1. 分配远程 cave。
2. 初始化 slot。
3. 生成机器码。
4. 写入 cave。
5. patch hook。

中途任意一步抛异常，都可能留下已经分配或已经写入的远程资源。

### 6.2 两阶段提交

#### Prepare

- 保存原始 hook 字节。
- 分配 cave。
- 初始化 slot。
- 写 cave。
- 读回验证 cave。
- 此阶段不要修改执行流。

#### Commit

最后一步才 patch hook。

patch 成功后设置明确状态，例如：

```cpp
injection_committed_ = true;
```

### 6.3 rollback

commit 前失败：

- free cave。
- 清空相关地址。
- hook 保持原样。

commit 后出现异常：

- 先恢复 hook。
- 验证恢复成功。
- 再 free cave。

建议用 scope guard 或局部 RAII 对象实现。

---

## 7. P1：修复 ProcessMemory::Write()

### 7.1 当前问题

当前实现存在：

- `VirtualProtectEx` 返回值未检查。
- 页面被改成 `PAGE_EXECUTE_READWRITE` 后没有恢复。
- `WriteProcessMemory` 只检查 BOOL。
- 没有验证 `bytes_written == data.size()`。
- 写机器码后没有明确调用 `FlushInstructionCache`。

### 7.2 推荐流程

```text
1. 检查 handle 和参数
2. VirtualProtectEx -> PAGE_EXECUTE_READWRITE
3. 检查 VirtualProtectEx 是否成功
4. WriteProcessMemory
5. 检查 BOOL
6. 检查 bytes_written == requested
7. 无论成功失败都恢复 old_protect
8. 写代码时 FlushInstructionCache
```

### 7.3 异常安全

恢复 protection 必须放进 scope guard。

示意：

```cpp
DWORD old_protect = 0;
if (!::VirtualProtectEx(handle_, address, size,
                        PAGE_EXECUTE_READWRITE, &old_protect)) {
  throw ...;
}

auto restore = MakeScopeExit([&] {
  DWORD ignored = 0;
  ::VirtualProtectEx(handle_, address, size, old_protect, &ignored);
});
```

### 7.4 区分数据写和代码写

建议增加：

```cpp
void Write(...);
void WriteCode(...);
```

`WriteCode()` 在成功后调用：

```cpp
::FlushInstructionCache(handle_, address, size);
```

---

## 8. P1/P2：远程线程 timeout 不能简单处理

### 8.1 问题

`CallRemoteThread()` 超时后直接 `TerminateThread`。

线程可能正在：

- 持有游戏内部锁。
- malloc/free。
- 修改全局对象。
- 执行游戏 handler。
- 运行异常处理或 TLS 清理。

直接杀线程可能让目标进程进入损坏状态。

### 8.2 建议返回明确状态

```cpp
enum class RemoteCallStatus {
  kCompleted,
  kTimedOutStillRunning,
  kForceTerminated,
  kProcessExited,
  kFailed,
};
```

不要用单个 `std::nullopt` 同时表示 timeout、失败和未知状态。

### 8.3 timeout 后资源处理

如果线程是否仍在运行不确定：

- 不要立即 free 它可能仍访问的 stub / buffer。
- 标记当前远程调用状态未知。
- 禁止继续执行依赖一致性的修改。
- UI 提示重新初始化或重启游戏。

---

## 9. P1：GM 远程缓冲布局必须检查长度

### 9.1 问题

`GmCmd.cpp` 中远程缓冲固定布局：

```text
base + 0x000: token
base + 0x040: rest
base + 0x200: stub
base + 0x400: return slot
```

GBK 字符串写入前没有看到后端对这些边界做严格校验。

UI 的 256 字节输入框不能替代后端边界检查，因为 UTF-8 长度和 GBK 长度不是同一概念。

### 9.2 建议

编码后再检查：

```cpp
if (token_bytes.size() > 0x40) {
  throw std::runtime_error("GM 命令 token 过长");
}

if (rest_bytes.size() > 0x1C0) {
  throw std::runtime_error("GM 命令参数过长");
}
```

必须把终止 NUL 计入容量。

长期可以改为动态布局，而不是依赖固定偏移。

---

## 10. P1/P2：字符串原地写存在 buffer capacity 风险

### 10.1 问题

姓名/别名当前做法是：

1. 读取游戏中保存的字符串指针。
2. 将新值转 GBK。
3. 最多截断到 100 字节。
4. 直接写到旧指针。

“最多写 100 字节”不代表目标 buffer 有 100 字节容量。

如果旧字符串只分配了很小空间，新字符串可能覆盖相邻堆对象。

### 10.2 推荐方案顺序

#### 方案 A：确认游戏真实字符串结构

优先查找：

- 固定数组。
- capacity 字段。
- 游戏自己的 string object。
- setter。

#### 方案 B：调用游戏自己的 allocator / setter

如果能定位，这是长期最佳方案。

#### 方案 C：临时保守限制

如果无法确认容量：

- 读取当前字符串真实字节长度。
- 新 GBK 数据不得超过可证明的安全长度。

虽然功能受限，但比假设 100 字节安全可靠。

### 10.3 统一验证和编码

当前 `ValidateValue()` 用 UTF-8 的 `std::string::size()`，写入却按 code point 转 GBK。

建议封装：

```cpp
EncodedString EncodeGameString(std::string_view utf8);
```

校验和最终写入都基于同一份编码结果。

---

## 11. P2：角色保存结果需要准确报告

### 11.1 当前问题

保存阶段是逐字段写入。

可能出现：

- 前几个字段成功。
- 中间某字段失败。
- 后续继续。
- 最终仍显示“保存完成”。

### 11.2 最低要求

维护：

```cpp
std::vector<std::string> saved_fields;
std::vector<std::string> failed_fields;
```

结果应展示：

```text
成功写入：12
失败：2
失败字段：姓名、生命
```

### 11.3 进一步改进

对数值字段保存前缓存旧值，如果中途失败，可以尝试 rollback。

长期还是建议 dirty save，只写用户实际修改过的字段。

---

## 12. P2：OnMoneyApply 等单字段路径必须检查结果

所有调用 `WriteCharField()` 的地方都应该检查结果，不能忽略失败后仍提示成功。

推荐：

```cpp
if (!WriteCharField(...)) {
  app.SetStatus("写入失败");
  return;
}

app.SetStatus("修改成功");
```

此外应逐步减少 `WriteCharField()` 内部的空 `catch (...)`，让调用方可以获得具体异常原因。

---

## 13. P2：关键路径减少 catch (...)

### 13.1 可以静默失败的场景

- 非关键 debug 探测。
- 可选模块扫描。
- 纯展示信息读取。

### 13.2 不应静默失败的场景

- hook 恢复。
- remote allocation 释放。
- 用户主动保存。
- 注入和 rollback。
- 初始化主路径。
- 文件写入。
- 权限/保护修改。

至少优先捕获：

```cpp
catch (const std::exception& ex) {
  ...
}
```

并在 debug 日志中记录 `ex.what()`。

---

## 14. P2：D3D11 RenderTarget 错误处理

### 14.1 问题

当前 `CreateRenderTarget()`：

- `pBackBuffer` 未初始化。
- 不检查 `GetBuffer()`。
- 不检查 `CreateRenderTargetView()`。
- 最后无条件 `Release()`。

### 14.2 建议

让函数返回 bool：

```cpp
bool CreateRenderTarget();
```

并使用 COM RAII，例如 WRL `ComPtr`：

```cpp
Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;

HRESULT hr =
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
if (FAILED(hr)) {
  return false;
}
```

创建 RTV 失败时：

- 保持 `g_mainRenderTargetView == nullptr`。
- 记录 HRESULT。
- 当前帧不要继续使用无效 RTV。

---

## 15. P2：Win32 / ImGui 初始化返回值检查

至少检查：

- `RegisterClassExW()`
- `CreateWindowW()`
- `CreateDeviceD3D()`
- `ImGui_ImplWin32_Init()`
- `ImGui_ImplDX11_Init()`

初始化失败后按“已经成功的阶段”逆序清理。

长期建议将窗口、D3D、ImGui backend 生命周期拆成独立 RAII 组件。

---

## 16. P2：资源生命周期统一 RAII

优先包装：

- `HANDLE`
- 远程 allocation
- hook transaction
- COM 指针
- ImGui backend lifecycle

示例：

```cpp
class RemoteAllocation {
 public:
  RemoteAllocation(ProcessMemory& mem, size_t size,
                   uintptr_t near_address);
  ~RemoteAllocation();

  uintptr_t address() const;
  void ReleaseOwnership();

 private:
  ProcessMemory* mem_ = nullptr;
  uintptr_t address_ = 0;
};
```

这样 `Inject()` 和 GM 的异常路径不需要大量重复的 try/catch/free。

---

## 17. Google C++ Style 落地计划

不建议把严重 bug 修复和全项目格式化放在同一个 commit，否则 review 时功能修改会被格式噪声淹没。

### 17.1 增加 .clang-format

建议项目根目录加入：

```yaml
BasedOnStyle: Google
Language: Cpp
ColumnLimit: 80
DerivePointerAlignment: false
PointerAlignment: Left
```

### 17.2 缩进

Google C++ Style 默认使用 2 spaces。

当前项目大量使用 4 spaces，建议按文件/模块分批格式化。

### 17.3 文件命名

当前例如：

```text
GameModder.cpp
ProcessMemory.cpp
Characters.cpp
SummonSteam.cpp
```

建议逐步改为小写下划线：

```text
game_modder.cpp
process_memory.cpp
characters.cpp
summon_steam.cpp
```

如果希望严格贴近 Google 常见习惯，也可以使用 `.cc`；但这会扩大 CMake 和 include diff，不需要和安全修复一起做。

### 17.4 命名规则

逐步统一：

- 类型：`PascalCase`
- 变量：`snake_case`
- 成员：`snake_case_`
- 常量：`kPascalCase`

### 17.5 C-style cast

将：

```cpp
(int)value
(size_t)i
(const float*)ptr
```

替换为：

```cpp
static_cast<int>(value)
static_cast<size_t>(i)
reinterpret_cast<const float*>(ptr)
```

### 17.6 80 列

交给 clang-format 处理，避免人工折行后又被工具重新格式化。

---

## 18. main.cpp 拆分计划

当前 `main.cpp` 已接近 5000 行，混合了：

- UI 状态。
- 角色逻辑。
- GM。
- 物品。
- summon。
- Win32。
- D3D。
- ImGui 初始化。
- 偏好设置。
- 多种业务行为。

建议目录：

```text
src/
  app/
    app_state.h
    app_state.cpp
    app_main.h
    app_main.cpp

  ui/
    character_tab.h
    character_tab.cpp
    gm_tab.h
    gm_tab.cpp
    item_tab.h
    item_tab.cpp
    summon_tab.h
    summon_tab.cpp
    settings_tab.h
    settings_tab.cpp

  platform/
    win32_window.h
    win32_window.cpp
    d3d11_renderer.h
    d3d11_renderer.cpp
```

### 18.1 第一批优先拆角色模块

因为本次多个高风险问题集中在角色编辑逻辑，建议第一批迁出：

- `CharWidget`
- `RebuildCharWidgets()`
- `OnCharRead()`
- `OnCharSave()`
- `OnMoneyApply()`
- 角色属性渲染

目标不是一次拆完，而是先建立可维护边界。

---

## 19. 编译告警与静态检查

### 19.1 MSVC

CMake 建议加入：

```cmake
if (MSVC)
  target_compile_options(PoqEditorImgui PRIVATE
    /W4
    /permissive-
  )
endif()
```

暂时不要马上启用 `/WX`。

步骤：

1. 开 `/W4`。
2. 清理现有 warning。
3. 新代码保持 warning-free。
4. warning 基本归零后再考虑 CI 中 `/WX`。

### 19.2 clang-format

先用于新拆出来和本次改动的文件，最后再做全仓库格式化。

### 19.3 clang-tidy

后续可以从低噪声规则开始：

- `bugprone-*`
- `performance-*`
- `modernize-use-nullptr`
- `modernize-use-override`
- 部分 `readability-*`

不要一次性全开。

---

## 20. 推荐实施批次

### Batch 1：角色保存安全

内容：

- 视野字段真正只读。
- unread 字段禁止保存。
- 保存只写 writable + loaded。
- 推荐同时引入 dirty save。

目标：普通“保存角色属性”不再具有明显崩溃和错误清零风险。

### Batch 2：ProcessMemory 写入安全

内容：

- 检查 `VirtualProtectEx`。
- 恢复原 protection。
- 检查 `bytes_written`。
- 增加 `WriteCode()`。
- 代码写入后 flush instruction cache。

### Batch 3：hook 生命周期

内容：

- hook 恢复成功后才能 free cave。
- 恢复后读回验证。
- PID 切换前清理旧进程。
- `Inject()` rollback。
- 明确注入状态机。

### Batch 4：GM / remote thread

内容：

- timeout 明确状态。
- 避免 timeout 后立刻释放仍可能被使用的 remote buffer。
- token/rest 做 GBK 编码后边界检查。
- 评估是否保留 `TerminateThread`。

### Batch 5：角色字符串

内容：

- 统一 UTF-8/GBK 校验。
- 调研真实字符串容量。
- 在容量未知时采取保守写入策略。

### Batch 6：UI / D3D

内容：

- HRESULT 检查。
- Win32 初始化检查。
- ImGui backend 初始化检查。
- COM RAII。
- 初始化失败统一清理。

### Batch 7：工程重构

内容：

- Google-based `.clang-format`。
- `/W4 /permissive-`。
- C++ casts。
- 拆分 `main.cpp`。
- 文件命名统一。
- 分批格式化。

---

## 21. 验证矩阵

### 21.1 角色编辑

覆盖：

- 正常读取全部字段。
- 人为制造一个字段读取失败。
- 只修改一个字段。
- 连续保存两次。
- 无敌模式开启时保存。
- 一击必杀开启时保存。
- 确认视野永不写入。
- 姓名/别名边界长度。

### 21.2 hook

覆盖：

- 首次注入。
- 同 PID 重复初始化。
- 切换 PID。
- 游戏退出后重新初始化。
- hook patch 失败。
- hook restore 失败。
- cave free 失败。
- 注入中途失败。

核心断言：

```text
只要 hook 仍指向 cave，cave 就绝不能被释放。
```

### 21.3 ProcessMemory

覆盖：

- 正常读写。
- 不可写页面。
- 写跨页。
- partial write。
- protection 恢复。
- 写机器码后执行。

### 21.4 GM

覆盖：

- 正常短命令。
- token 边界。
- rest 边界。
- 超长参数。
- 正常返回。
- timeout。
- 调用期间目标进程退出。

### 21.5 D3D

覆盖：

- 正常启动。
- resize。
- 最小化/恢复。
- render target 创建失败。
- device lost 相关路径。

---

## 22. 建议的 commit 拆分

建议不要做一个超大 commit。

```text
fix(character): make readonly and unread fields unsavable

fix(memory): restore page protection after remote writes

fix(hook): make cave lifetime depend on hook restoration

fix(hook): rollback failed injection and close old pid safely

fix(gm): harden remote-call timeout and argument buffers

fix(character): make game-string writes capacity aware

fix(renderer): validate D3D and ImGui initialization

build: enable higher MSVC warnings

style: add Google-based clang-format config

refactor(ui): extract character tab from main
```

这样每个阶段都容易 review、验证和回退。

---

## 23. 最终验收标准

完成本轮优化后，至少满足：

- “保存角色属性”不会写任何标记为只读的字段。
- 任意字段读取失败不会被默认写成 0。
- 只要 hook 仍指向 cave，cave 就不会被释放。
- PID 切换不会静默遗留旧进程注入。
- `Inject()` 中途失败会 rollback 已申请资源。
- `ProcessMemory::Write()` 恢复页面 protection。
- `ProcessMemory::Write()` 验证实际写入长度。
- 写机器码后正确 flush instruction cache。
- 远程线程 timeout 有明确状态。
- timeout 后不会立即释放仍可能被线程访问的内存。
- GM 固定缓冲区有编码后的长度边界检查。
- 字符串写入具有可证明的容量边界。
- 保存失败会明确指出失败字段。
- D3D/Win32/ImGui 关键初始化返回值都被检查。
- 关键修改路径不再使用空 `catch (...)` 隐藏失败。
- 项目加入 Google-based `.clang-format`。
- 新代码按 Google C++ Style 编写。
- MSVC 至少启用 `/W4`。
- `main.cpp` 开始按功能拆分，角色模块优先。

本计划的核心原则是：**先消除会破坏目标进程状态的风险，再提高数据一致性，最后做结构和风格统一。**
