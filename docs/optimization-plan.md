# PoqEditor(imgui 版)代码优化说明

日期:2026-09-19 · 基于 commit `7ff5c45` 的代码审阅。
范围:`src/main.cpp`(4848 行)、`src/Core/GamFile.cpp`(1307)、`Core/GameModder.cpp`、
`Native/ProcessMemory.cpp` 等主要路径。

优先级:P0=正确性问题,必须修;P1=用户可感知的性能;P2/P3=次要性能;
P4=小项;P5=结构可维护性(本次不动,仅记录)。

---

## P0 · 词条删光时数组不同步(正确性)

### 问题
`GamFile.cpp`:
- `ParseGInfoRows()` 结尾 `return !rows.empty();` —— G_Info 文本解析出 **0 行**时返回 false;
- `SyncAffixesFromGInfo()` 开头 `if (!ParseGInfoRows(rows)) return false;` —— 于是**放弃重建**;
- `SetGInfo()` 调同步后**不看返回值**。

触发路径:把一件物品的词条**全部勾选删除** → 新 G_Info 只剩头行(`Lv. 8`)→ 解析 0 行 →
同步返回 false → **词条数组保持旧节点**。游戏读的是数组 → 删光了的游戏里照样全在。
"粘贴属性到本来没有词条行的物品"也会触发同样路径。

### 方案
1. `ParseGInfoRows` 语义改为"文本非空即成功"(0 行也是合法结果);
2. 同步时 0 行 → 写 0 节点数组(count=0);
3. **先实测**游戏对 count=0 数组的接受度(加载→看物品词条是否为空→游戏存档回读),
   若游戏不接受,退一步:保留 1 个零值占位节点(值 0 的节点游戏不渲染,已有先例);
4. `SetGInfo` 检查同步返回值,失败时弹错误而不是静默。

### 验证
- 工具里删光一件物品的词条 → 保存 → 重开 → 词条应为空;
- 游戏加载 → 该物品无词条 → 游戏存档 → 工具重开仍为空。

---

## P1 · 每帧重复构建(UI 跑在 60fps 循环里)

### 问题 A:`RenderGamTree` 的 regions 每帧重建
`main.cpp:3113` 附近:`RenderGamTree()` 函数体内构造 `std::vector<Region>` +
`std::map<std::string,int> ridx`。558 容器的世界档:每帧做 558 次路径切分 + map 插入,
上千次字符串堆分配。**每帧都发生**。

### 方案
regions 只依赖 `gam_file`(打开时)与容器别名 → 装进 `App`,在"打开 gam / 粘贴物品 /
删除物品"时重建一次;渲染只读缓存。过滤器命中与否每帧算没关系,但命中用的**小写串**
也要缓存(见问题 B)。

### 问题 B:过滤时每件物品每帧 2 次 ToLower
`item_matches`(`main.cpp:3159`)对每件物品做 `ToLower(alias)` +
`ToLower(显示名)`,各分配一个 string。7000 件的世界档 ≈ 每帧 1.4 万次分配。

### 方案
`GamItem` 增加两个缓存字段 `alias_low`、`disp_low`(装载时算一次);过滤器与
`lowf` 比对直接用缓存串。显示名拼接(`中文名(别名)`)也预拼缓存。

### 问题 C:下拉框选项列表每帧新建
- `RenderAttrTable`(`main.cpp:1754`):每帧 `ItemData::AttrDisplayList()` 返回
  ~99 个 string 的新 vector;
- GAM 词条表(`main.cpp:3434`)同款,另有 `PercentDisplayList()`。

`AttrNames()` 内部 map 是静态的,但返回 vector 每次现做 —— 每帧几百次分配 ×2 处。

### 方案
`ItemData` 增加 `static const std::vector<std::string>& AttrDisplayListRef()`(首次调用
构建一次);调用点改引用,`c_str()` 数组同样缓存。道具 tab 常驻,这是**无条件每帧**的浪费。

### 验证
- 打开世界档(多容器)开过滤器,任务管理器看 CPU/GC 压力对比;
- 功能回归:过滤命中、物品名显示、下拉内容不变。

---

## P2 · 读游戏内存:每 4 字节一次系统调用 + 一次堆分配

### 问题
`ProcessMemory::ReadUInt32`(`ProcessMemory.cpp:249`)→ `Read()` →
一次 `ReadProcessMemory` **系统调用** + 返回**按值 vector**(堆分配)。

热点:
- `GameModder::ReadAttributes`(`GameModder.cpp:723`):每节点 5 个字段 = 5 次调用;
  20 条词条的武器 ≈ 120 次调用(含链表遍历 ReadNodeAddrs);
- "读取属性"按钮一次点击全部发生,是道具修改路径上**可感知延迟**的来源。

### 方案
1. `ProcessMemory` 加 `bool ReadBuf(uintptr_t, void*, size_t)`(栈/调用方缓冲,零分配);
   `ReadUInt32` 改用它;
2. 节点字段改**整槽一次读**(槽 0x40 字节连续,id/color/flag/percent/value 偏移全在槽内);
3. `ReadNodeAddrs` 链表遍历保持逐节点(地址不连续),但每次只读 8 字节(next+prev)。

### 验证
- 读取属性功能不变(值逐一对比);
- "读取属性"按钮耗时对比(预期 5~10× 提速)。

---

## P3 · GetGInfo/SetGInfo 全内容逐字节扫 ×3

### 问题
`GamFile.cpp:229/245`:`GetGInfo`/`SetGInfo` 从 0 逐偏移试 `DecStr` 找 `"G_Info"` 字段。
一次 `SetGInfo` 实际发生 3 次全扫:SetGInfo 自身 + 同步里 `ParseGInfoRows` 与
`GetGInfoHeader` 各自又调 `GetGInfo`。保存几百件 dirty 物品时是上千次全扫。

### 方案
`GamItem` 加 `size_t ginfo_key_off = 0;`(装载时找到后缓存;0=未找)。
`content` 变长/变短时按已知 delta 平移该偏移(SetGInfo/SetName/SetAlias 内部已知 delta)。
三个调用点 O(1) 命中;失效(找不到)时回退全扫一次并更新缓存。

### 验证
- 语料 512 件往返(affix_rt)逐字节一致率不降;
- 大档保存耗时对比。

---

## P4 · 小项(顺手修,不单独立项)

| 位置 | 问题 | 修法 |
|---|---|---|
| `ParseAffixes`(`GamFile.cpp:366`) | 从前往后全扫找 `[0x2C][FFFFFFFF]` 链 | 数组在内容末尾:从尾部按 44 步长**倒着验证**,O(节点数) |
| `Md5Final`(`main.cpp:222`) | padding 逐字节喂(≤63 次调用) | 一次 memset 补到 56 |
| `walkSubtree` 失配重试 | 每字节一次完整解析尝试(含 X59Range 拷贝),14MB 档解析时间主要在这 | 先做轻量探测(L 边界 + cnt/total 校验)再进完整解析 |
| `Save::delta_at` | 每查询线性扫 dirty 列表 | 前缀和(当前规模收益小,可不动) |

---

## P5 · 结构性(本次不动,仅记录)

1. `main.cpp` 4848 行单文件:UI、App 状态、MD5、窗口管线混在一起 → 按 tab/模块拆;
2. 三段近似相同的"名字/别名/类型"询问框回调(JSON 导入 / 复制上一件 / GAM 粘贴)
   → 收敛成一个带参数 helper;`MsgBoxAsk` 已是 `MsgBoxAskEx` 特例,可删一个;
3. GBK↔UTF8 转换有 3 份实现(main.cpp、ItemData.cpp 的 U8ToGbk、tmp 工具)→ 共用一处。

---

## 实施顺序

1. **P0**(先实测 count=0,再改)
2. **P1-A/B/C**(每帧浪费,收益最直观)
3. **P2**(道具修改可感知延迟)
4. **P3、P4**(顺手)
5. P5 单独安排

每步:改 → 全量回归(语料 512 件 affix_rt 逐字节一致 + 现有样本 `_u01o.._u01v`)
→ 出包。P0 需要一轮游戏内实测(count=0 行为)。

---

# 实施记录(2026-09-19)

## 已完成

### P0 词条删光不同步 —— 已修 + 已验证
1. `ParseGInfoRows` 改为"文本非空即成功"(0 行合法);
2. `SetGInfo` 检查同步返回值,失败不再静默;
3. **连带发现并修掉两个真问题**:
   - `SetGInfo` 重排内容后**尾部长度字段没跟着更新** → 已加 `WriteContentLen`;
   - 删光后数组变成 0 节点,`ParseAffixes` 认不出来 → `has_affixes=false`
     → **之后再也加不回词条**。已加"0 节点数组"识别(`[count=0][长度]`,且在
     "确认无节点链之后"才判 —— 提前判会把正常物品误判,实测踩过)。
4. 实测:`删光 → 0 节点 → 再加 → 1 节点 → 再删光` 全链路正确,长度字段 OK。

### P1-A/B/C 每帧浪费 —— 已完成
- **A** `regions` 缓存进 `App::gam_regions`,只在 `items.size()` 变化时重建;
- **B** `GamItem` 加 `alias_low`/`disp_low`,装载时算一次,过滤器零分配;
- **C** `ItemData::AttrDisplayListRef()`/`PercentDisplayListRef()` 静态缓存,
  道具修改/GAM 词条表不再每帧新建 ~99 个 string。

### P2 内存读取零分配 —— 已完成(待在游戏内验证)
- `ProcessMemory::ReadBuf()`(栈/调用方缓冲,不抛异常);
- `ReadNodeAddrs` 用 ReadBuf 读 next;
- `ReadAttributes` 改为**整段一次读**(节点字段全在同一槽内),原来每节点 5 次
  `ReadProcessMemory` + 5 次 vector 分配 → 现在 1 次、零分配。

### P3 G_Info 偏移缓存 —— 已完成
`GamItem::ginfo_key_off_`,装载/首次查找时记住偏移;所有会改变内容长度的写入口
(`SetGInfo`/`SetName`/`SetAlias`/`SetAffixes`)调 `InvalidateGInfoCache()` 失效。
连续调用(解析词条→取文本→取头行)只全扫一次。

### P4 小项 —— 已完成两项
- `Md5Final` padding 一次喂完(原来逐字节最多 63 次);**已用 certutil 逐字节复验**;
- `ParseAffixes` 改为从尾部按 44 步长倒着验证(O(节点数)),保留前向全扫作回退。

### 未做
- P4 的 `walkSubtree` 轻量探测(改动面大、风险高,留待后续);
- P4 的 `delta_at` 前缀和(当前规模收益可忽略);
- P5 全部(结构性重构,单独安排)。

## 验证结果

```
语料 521 件:  完全一致 455  仅系数不同 0  结构不同 66  重建不自洽 0  长度字段错 0
```
(比改动前的 446 件更多 —— `ParseGInfoRows` 语义修正后,原先被当作"解析失败"跳过
的物品现在正确重建。)

## 待游戏内验证

**P2(内存读取)**:这块改的是与游戏进程交互的路径,离线测不到,需在游戏里确认:
1. 点"读取属性" → 词条列表与改动前一致(值/颜色/激活/类型);
2. 点"复制上一件物品" → 写入条数与提示一致;
3. 观察"读取属性"的响应速度(预期明显变快)。

