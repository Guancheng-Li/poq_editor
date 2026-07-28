# 秦殇内存修改器 (PoqEditor) - Python/PySide6 版

秦殇（Prince of Qin, POQ.EXE, 32 位）游戏内存修改器。本工具是原 VB6 `poq_hack.exe`
的 64 位重制版，用 Python + PySide6 实现，可在 64 位 Windows 上运行，通过读写 32 位
游戏进程内存修改手持物品的属性。

## 运行环境

- **Windows**（必须）。内存读写通过 Win32 API（`ctypes.windll.kernel32`）实现，
  非 Windows 平台无法实际操作内存。
- **Python 3.9+**（推荐 3.10/3.11）。
- **PySide6**。
- **以管理员身份运行**（打开游戏进程需要 `PROCESS_ALL_ACCESS` 权限）。

## 安装

```bat
pip install -r requirements.txt
```

## 运行

以管理员身份打开命令行，进入本目录后执行：

```bat
python main.py
```

## 使用流程

1. 启动秦殇（`POQ.EXE`）。
2. 以管理员身份运行本工具。
3. 在顶部"进程"下拉框选择 `POQ.EXE`（若已自动选中可跳过），点击"初始化"。
4. 在游戏中用鼠标拾起要修改的物品（游戏会把该物品指针写入锚点地址 `0x901FF0`）。
5. 切到"物品修改"页，点击"读取属性"读取当前物品基础属性与属性列表。
6. 如需套用模板，点击"从模板读取"（读取 `EquMod.ini` 中的 46 条属性）。
7. 编辑表格与基础属性文本框，点击"保存属性"写回游戏内存。
8. 在游戏中把物品扔到地上再捡起，使修改生效。

## 内存机制

（逆向自原版 `poq_hack.exe`，详见上层目录 `REVERSE_FINDINGS.md`）

- `OpenProcess(PROCESS_ALL_ACCESS = 0x1F0FFF, FALSE, pid)` 打开游戏进程。
- 读 DWORD @ `0x00901FF0` 得到"当前手持物品"指针 `P`。
- 物品基址 `= P - 0xF0`（`ItemBaseAdjust = -240`）。
- 基础属性：在 `item_base + offset` 处读写 4 字节 DWORD，偏移来自 `offsets.json`
  的 `BasicFields`（0x00/0x04/0x08/0x0C/0x10/0x14/0x20/0xA0/0xCC/0xF8/0x110/0x268/0x404）。
- 46 条属性槽：起点 `item_base + 0x404`，每槽步长 `0x24` 字节，槽内字段
  `Index@0x00, Num@0x04, Color@0x14, Precent@0x20`。
- 所有偏移均可在"偏移配置"页调整，保存后写入 `offsets.json` 并即时生效。

## 文件说明

| 文件 | 作用 |
|------|------|
| `main.py` | 程序入口，创建 `QApplication` 与主窗口 |
| `main_window.py` | 主窗口 UI 与逻辑（对应 `MainWindow.xaml(.cs)`） |
| `game_modder.py` | 内存修改核心逻辑（对应 `Core/GameModder.cs`） |
| `process_memory.py` | Win32 进程内存读写封装（`Native/*.cs`） |
| `offsets_config.py` | 偏移配置模型与 JSON 读写（`Core/OffsetsConfig.cs`） |
| `ini_template.py` | `EquMod.ini` 模板读写（`Core/IniTemplate.cs`） |
| `models.py` | `ItemProperty` 数据类（`Core/ItemProperty.cs`） |
| `offsets.json` | 偏移配置（UTF-8） |
| `EquMod.ini` | 46 条属性模板（UTF-8） |
| `requirements.txt` | 依赖（仅 `PySide6`） |
