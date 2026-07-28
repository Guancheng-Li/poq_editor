# 秦殇内存修改器 - 64 位重制版 (PoqEditor)

原版 `origin/poq_hack.exe` 是 VB6 + PECompact2 加壳的 32 位程序，依赖 `MSFLXGRD.OCX`，
在 64 位 Windows 上因 OCX 未注册 / FlexGrid 高 DPI 渲染异常而无法使用。
本项目用 **C# + WPF (.NET 8)** 重写，原生 64 位、现代 UI、**所有内存偏移可配置**。

## 功能（完整复刻原版）
- 选择/刷新进程（自动识别 POQ.EXE）
- **初始化**：OpenProcess + 读取手持物品指针验证
- **读取属性**：读取当前手持物品的基础属性与 46 条属性槽，显示到表格
- **从模板读取**：载入 `EquMod.ini` 的 46 条属性模板（按 Index 匹配现有槽，或直接填充）
- **保存属性**：把表格中的基础属性与 46 条属性写回游戏内存
- **内存查看**：按地址读/写任意内存（十六进制），用于校准偏移
- **偏移配置**：所有地址/偏移在 UI 里可改，保存到 `offsets.json` 即时生效

## 工作原理（逆向自原版）
1. `OpenProcess(PROCESS_ALL_ACCESS, pid)` 打开游戏
2. 读 `[0x901FF0]` 得到"当前手持物品"指针 P（玩家拾起物品时游戏写入该地址）
3. 物品基址 = `P - 0xF0`
4. 在物品基址 + 各偏移处读/写 4 字节 DWORD（基础属性 + 属性槽）

> 详见上层目录 `REVERSE_FINDINGS.md`。

## 构建
需要 **Windows + .NET 8 SDK**（WPF 仅支持 Windows）。
```bat
cd poqeditor
dotnet build -c Release
```
产物：`bin\Release\net8.0-windows\PoqEditor.exe`（首次运行会触发 UAC，因为需要管理员权限修改游戏内存）。

也可发布成单文件：
```bat
dotnet publish -c Release -r win-x64 --self-contained false /p:PublishSingleFile=true
```

## 使用步骤（与原版一致）
1. 以管理员身份运行 PoqEditor.exe
2. 启动秦殇(POQ.EXE)，在 PoqEditor 里点“刷新进程”，选择 POQ.EXE
3. 点“初始化”（成功会显示手持指针与物品基址）
4. 在游戏中用鼠标拾起要修改的物品（游戏会把物品指针写入 0x901FF0）
5. 点“读取属性”，显示物品当前属性
6. 点“从模板读取”，载入 46 条属性（EquMod.ini）
7. 在表格里按需编辑数值
8. 点“保存属性”
9. 回到游戏，把物品扔到地上再捡起，使修改生效

## 偏移校准（重要）
默认偏移来自对原版 exe 的逆向，可能与你实际的 POQ.EXE v1.30 有出入。
校准方法：
1. “内存查看”页，地址填 `0x901FF0`，长度 4，点“读取”——应得到一个非零指针。
2. 用该指针 - 0xF0 作为物品基址，再读物品内存，对照游戏里看到的物品属性定位各字段偏移。
3. 在“偏移配置”页修改对应偏移，点“保存配置并应用”。

## 文件说明
| 文件 | 作用 |
|---|---|
| `offsets.json` | 内存偏移配置（可改，即时生效） |
| `EquMod.ini` | 46 条属性模板（UTF-8 编码） |
| `app.manifest` | 请求管理员权限 + 高 DPI 支持 |

## 目录结构
```
poq/
  origin/        原版（保留不动）
  poqeditor/     本重制项目
  REVERSE_FINDINGS.md   逆向分析记录
```
