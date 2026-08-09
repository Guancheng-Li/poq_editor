# 秦殇物品修改器 (PoqEditor) - imgui 单文件版

秦殇(POQ)游戏内存修改器,基于 **Dear ImGui(docking 分支)+ Win32 + DirectX11** 实现。
是 `../poqeditor_cpp`(C++/Qt)的 **100% 功能复刻**,但:
- **单个 exe,零 DLL 依赖**(MSVC /MT 静态运行时,连 VCRUNTIME 都不需要);
- **无控制台窗口**、**无 imgui 自绘标题栏**(用系统标准标题栏,标题与主界面一致,不重复);
- **灰白主题**、**中文字体**(运行时加载系统微软雅黑,无需打包字体);
- 体积 ~1-2MB,拷贝即跑。

## 运行

1. 启动游戏(POQ.EXE)。
2. 双击 `PoqEditor.exe`(内嵌管理员清单,自动请求 UAC)。
3. 顶部"选择进程"下拉框选 POQ.EXE(自动选中),点"初始化"(未注入则自行 AOB 注入)。
4. 游戏中用鼠标拾起要修改的物品(保持在光标上)。
5. "物品修改"页点"读取属性"载入基础属性与属性列表。
6. 修改基础属性;属性列表可"添加/删除/清空"或"从模板读取"载入 JSON、"另存为"导出。
7. 点"保存属性"写回内存;游戏中把物品扔到地上再捡起使修改生效。

> 按 **F12** 显示/隐藏"内存查看"与"偏移设置"调试页(默认隐藏)。

## 构建(本机)

需要:VS 18 2026(MSVC)+ Windows SDK + CMake 3.16+ + imgui(默认 `C:/env/imgui-1.92.1`)。

```bat
build.bat
```

或手动:

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DIMGUI_DIR=C:/env/imgui-1.92.1
cmake --build build --config Release
```

产出:`build\Release\PoqEditor.exe`(单文件,可直接拷给别人)。

- imgui 源码默认来自 `C:\env\imgui-1.92.1`(普通版,本工程未用 docking/viewport 特性),
  可用 `-DIMGUI_DIR=...` 覆盖(如 `C:\env\imgui-docking`)。

## 功能与 cpp 版对齐

- **进程**:列表/手动 PID 或进程名、刷新、初始化(游戏重启后自动按名重选)。
- **基础属性**:武器名字(字符串)、武器等级、是否鉴定、耐久度/上限、基准价格、重量、
  类型、孔数、等级/力量/敏捷/体质需求、五行、弓箭数量;条件/强制逻辑(箭/矢类型时
  孔数=0、耐久度=-1、弓箭数量仅箭/矢可编辑)一致。
- **属性列表**:99 个属性名 + 5 种颜色 + 数值/百分比 + 属性值;读/存/加/删/清空/模板。
- **内存模型**:与 CT 对齐——AOB 自注入捕获 cur_item(复用已注入)、链表读写、
  静态节点池新增/删除(安全策略一致)。
- **F12 调试**:内存查看(hex 读/写)、偏移设置(会话内生效,不写文件)。
- 普通模式状态栏自动隐藏内存地址,仅 F12 调试模式显示。

## 项目结构

```text
poqeditor_imgui/
├── CMakeLists.txt          MSVC /MT + imgui(1.92.1)+ DX11 + WIN32 子系统
├── build.bat               一键构建 -> build\Release\PoqEditor.exe
├── app.manifest            requireAdministrator + UTF-8 代码页
├── app.rc                  内嵌 manifest + exe 图标
├── poq_multi_3sizes.ico    图标源文件(仅编译时需要)
├── README.md
└── src/
    ├── main.cpp            Win32+DX11+imgui 主程序与全部 UI(无边框窗口,自绘标题栏)
    ├── Core/
    │   ├── ItemProperty.h       一条非固定属性
    │   ├── ItemData.h/.cpp      属性 ID/颜色/百分比 查表与互转(UTF-8)
    │   ├── OffsetsConfig.h/.cpp 内置默认偏移(不读配置文件,单文件版)
    │   ├── Json.h/.cpp          轻量 JSON DOM(UTF-8),替代 QJson
    │   ├── JsonTemplate.h/.cpp  JSON 模板加载(校验)与导出
    │   └── GameModder.h/.cpp    AOB 注入 + 属性读写核心
    └── Native/
        └── ProcessMemory.h/.cpp Win32 进程内存读写/分配封装
```

## 常见问题

- **初始化报"OpenProcess 失败"**:没以管理员运行,或游戏刚重启(PID 失效,会自动重找一次)。
- **报"未找到模块 EasyRPG116.dll"**:游戏没启动或版本不同——F12 偏移设置页改 `hook.module`。
- **中文不显示**:确认系统有微软雅黑/黑体/宋体(正常 Windows 都有);字体从系统目录加载。
- **界面缺字/乱码**:所有源文件均为 UTF-8,MSVC 用 `/utf-8` 编译,勿改编码。
