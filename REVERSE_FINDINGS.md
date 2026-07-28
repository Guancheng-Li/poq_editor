# poq_hack.exe 逆向分析结果

## 程序身份
- **语言**: VB6 (Visual Basic 6) 原生编译，用 PECompact2 加壳
- **运行时**: 依赖 msvbvm60.dll + MSFLXGRD.OCX (Microsoft FlexGrid 控件)
- **尺寸**: 26KB，加壳后 .text 被 PECompact2 压缩

## 64 位下失效的原因
1. VB6 程序是 32 位，64 位 Windows 需通过 WoW64 运行（可行），但：
2. **MSFLXGRD.OCX 未注册** —— 这是 GUI 无法加载的最常见原因（需用 `C:\Windows\SysWOW64\regsvr32` 注册 32 位 OCX）。
3. MSFlexGrid 在新版 Windows + 高 DPI 下渲染异常（用户反馈的 GUI 问题）。
4. 加壳的 VB6 程序在新系统上反作弊/兼容性更脆弱。

## 已脱壳
用 unicorn 模拟 PECompact2 解压 stub（含 SEH 控制流），成功还原 .text（0x401000, 0x14000 字节）。
- 解压后确认包含：`MSVBVM60`, `VB5!`（VB6 工程标志）, `ReadProcessMemory`, `WriteProcessMemory`, `OpenProcess`。

## 内存修改机制（核心）
通过反汇编 VB6 原生代码（调用点经 DllFunctionCall thunk）确认：

### 使用的 Win32 API（VB6 Declare）
- `OpenProcess(0x1F0FFF=PROCESS_ALL_ACCESS, FALSE, pid)` —— 打开游戏进程
- `ReadProcessMemory(hProcess, lpBase, lpBuf, nSize, &bytesRead)`
- `WriteProcessMemory(hProcess, lpBase, lpBuf, nSize, &bytesWritten)`
- `CloseHandle`, `CreateToolhelp32Snapshot`, `Process32First/Next`（枚举进程）, `GetVersion`
- `GetPrivateProfileStringA` / `WritePrivateProfileStringA`（读写 EquMod.ini）

### 锚点地址（关键）
- **固定地址 `0x00901FF0`**：游戏中"当前手持物品"的指针。玩家用鼠标拾起物品时，游戏把该物品对象指针写入 `[0x901FF0]`。
- 每个操作（初始化/读取/保存）都会重新：`OpenProcess` → `ReadProcessMemory(0x901FF0, 4字节)` → 得到指针 P。

### 物品基址
- `item_base = P - 0xF0`（手持指针指向物品对象中部，-0xF0 回到对象起始）。

### 物品字段偏移（相对 item_base）
从 cmdRead 读取 / cmdSave 写入的调用点提取（每个 4 字节 DWORD）：
- 基础属性：`0x0, 0x4, 0x8, 0xC, 0x10, 0x14, 0x20`
- 其他字段：`0xA0, 0xCC, 0xF8, 0x110, 0x268, 0x404`
- 对应 UI：TxtDur(耐久)/txtWeight(重量)/txtHole(孔数)/txtNeed(需求) 等

### 46 属性模板（EquMod.ini）
- ini 定义 46 条属性：每条 = {Index(属性代码), Num(数值), Color(颜色), Precent(是否百分比)}
- cmdRead 读取物品当前属性显示到 MSFlexGrid；cmdSave 把 46 条属性写回物品。
- 属性数组通过 `field_0x54 + i*4` 形式访问（循环读取，每元素 4 字节）。
- **注意**：46 属性数组的完整结构（每槽字段布局/stride）需在实现时进一步确认；建议把所有偏移做成可配置（ini），便于针对 POQ.EXE v1.30 校准。

## UI 结构（Form1）
- `cbProcess` 进程选择下拉框
- `cmdInit` 初始化、`cmdRead` 读取属性、`cmdSave` 保存属性
- `cmdAdd/cmdDel/cmdEdit/cmdmod/cmdRef`、`mnuRead/mnuWrite/mnuMod` 菜单
- `MSFlexGrid` 属性网格、`lbSkCount` 属性计数
- `txtSkill/cbSkill/ckPrecent` 属性编辑、`TxtDur/txtWeight/txtHole/txtNeed` 基础属性
- 配套模块：`Module1`、`modXP`（XP 样式）、`ClassIniFile`（INI 读写类）

## 工作流程（来自使用说明 + 反汇编）
1. 运行秦殇(POQ.EXE)，用 cbProcess 选择进程
2. cmdInit：OpenProcess + 读 0x901FF0 验证（成功则提示）
3. 游戏中用鼠标拾起要改的物品（游戏更新 [0x901FF0]）
4. cmdRead：读 [0x901FF0]-0xF0 为物品基址，读各属性显示到网格
5. 从模板(EquMod.ini)载入 46 条属性
6. cmdSave：把属性写回物品内存
7. 游戏中把物品扔地上再捡起，使修改生效
