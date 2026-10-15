#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Core/ItemProperty.h"
#include "Core/JsonTemplate.h"
#include "Core/OffsetsConfig.h"
#include "Native/ProcessMemory.h"

namespace poq {

/// 秦殇物品内存修改核心(对应 Python 的 game_modder.py)。
///
/// 内存模型来自对齐的 Cheat Engine 表:
/// - cur_item 由 AOB 注入捕获:在 hook.module+hook.offset 处 hook 原指令
///   `mov eax,[esi+eax*4+disp8]; pop esi`(5 字节 `8B 44 86 ?? 5E`),命中条件时
///   把 esi(物品指针)写入 cur_item 槽。
/// - 初始化(点"初始化"按钮)自动注入/复用:已注入(0xE9,如上次残留)则解析现有 cave
///   复用其 cur_item 槽;未注入则自行分配 code cave 并 patch 5 字节 jmp。
/// - 物品基址 = cur_item 槽的值 + item_base_adjust(默认 0,无 -0xF0 调整)。
/// - 基础属性:物品基址 + 偏移。
/// - 非固定属性:链表,头指针在 [物品基址 + attr_list.head_offset],节点字段见
///   AttrListCfg(next/id/color/precent/value 等)。新增节点从编辑器自管理的动态节点池
///   (VirtualAllocEx)里按 slot_size 分配槽位,游戏不认识这块内存、不会 free。
class GameModder {
public:
    explicit GameModder(OffsetsConfig config) : config_(std::move(config)) {}

    const OffsetsConfig& Config() const { return config_; }
    OffsetsConfig& Config() { return config_; }
    ProcessMemory& Mem() { return mem_; }
    /// 当前打开的进程 PID(0 = 未打开;角色定位等需要按 pid 查模块基址)。
    unsigned int Pid() const { return pid_; }

    /// 是否已注入(cur_item 槽地址非 0)。
    bool IsInjected() const { return cur_item_addr_ != 0; }
    /// cur_item 槽地址(0 = 未注入)。
    uintptr_t CurItemSlotAddr() const { return cur_item_addr_; }
    /// hook 指令地址。
    uintptr_t HookAddr() const { return hook_addr_; }

    /// 打开进程并完成/复用 AOB 注入以捕获 cur_item。成功返回当前物品指针(可能为 0)。
    ///
    /// Throws std::runtime_error: 打开失败、找不到模块、hook 字节异常等。
    uintptr_t Initialize(unsigned int pid);

    /// 读取目标进程 hook 处实际字节,判断是否已被注入(0xE9 = jmp)。
    bool IsHookInjected(unsigned int pid);

    /// 关闭进程句柄。若是本工具自注入,则还原原始字节并释放 cave;复用外部注入则不还原。
    // Close() 结果: hook 是否已还原、cave/节点池是否已释放。
    // 不变量(计划 P0): 只要 hook 仍指向 cave, cave 绝不能被释放。
    struct CloseResult {
        bool hook_restored = true;   // hook 字节已写回并读回校验通过(非自注入时为 true)
        bool cave_freed = true;      // code cave 已释放
        bool pool_freed = true;      // 词条节点池已释放
        bool process_exited = false; // 目标进程已退出(远程内存随进程释放, 无需再管)
        std::string error;           // 失败原因(有内容表示需要提示用户)
    };
    /// 关闭并还原注入。正常路径会抛异常报告失败(除非进程已退出);
    /// 析构/兜底请用 CloseNoThrow()。
    CloseResult Close();
    /// 不抛异常的关闭(析构兜底): 失败只记录到 last_close_error()。
    void CloseNoThrow();
    /// 同 CloseNoThrow, 但把结果交回调用方(退出路径要记录失败原因)。
    CloseResult CloseNoThrowResult();
    /// 最近一次关闭失败的原因(CloseNoThrow/Close 失败时填充)。
    const std::string& last_close_error() const { return last_close_error_; }

    /// 读取 cur_item 槽 = 当前物品指针(0 表示当前无物品)。
    uintptr_t GetItemPointer();

    /// 捕获后冻结支持(自注入的 cave 带冻结闸门):成功后读取可冻结防被覆盖。
    bool CaptureFreezeSupported() const { return freeze_addr_ != 0; }
    /// 冻结/解冻捕获:冻结后钩子不再写 cur_item 槽(防场景/背包读取顶掉手持物品)。
    void SetCaptureFrozen(bool on);
    /// 背包拿起边缘门已启用(steam 的 POQ.exe+0x261F84):只在 flag 0<->1 变化瞬间捕获,
    /// 悬停/场景读取(flag 平直)一律跳过。启用时读取直接用闩锁即可,无需再等捕获。
    bool BagFlagActive() const { return bag_flag_active_; }

    /// 物品基址 = 物品指针 + item_base_adjust(默认 0)。
    ///
    /// Throws std::runtime_error: 当前无物品(指针 0/0xFFFFFFFF)。
    uintptr_t GetItemBase();

    /// 读取所有基础属性,返回 BasicState。
    ///
    /// dword/word/enum 返回 int;string_ptr 返回 UTF-8 字符串或 "未知物品"。
    /// 弓箭数量在类型非箭/矢时返回 -1;类型为箭/矢(13/14)时:孔数=0、耐久度=-1、
    /// 耐久度上限=-1(force_when)。
    BasicState ReadBasicFields(uintptr_t item_base);

    /// 写入基础属性。BasicState 键为 label,值为 int(或 string_ptr 的 UTF-8 字符串)。
    /// 只读字段忽略。校验:clamp 到 [min,max];耐久度与上限联动——上限为 0/-1 时
    /// 耐久度强制一致(0 无法装备 / -1 永不损坏),否则耐久度 <= 上限且改上限、
    /// 新上限>原耐久度时写入耐久度=新上限;弓箭数量仅类型∈{13,14}时写入;
    /// 类型∈{13,14}(箭/矢)时:孔数=0、耐久度=-1、耐久度上限=-1(强制值,bypass clamp)。
    void WriteBasicFields(uintptr_t item_base, const BasicState& values);

    /// 遍历链表读取所有非固定属性。
    std::vector<ItemProperty> ReadAttributes(uintptr_t item_base);

    /// 把属性写回链表。返回 (写入条数, 跳过条数)。
    ///
    /// 安全策略(对齐原版修改器):
    /// - 现有节点:只改字段值 + 自愈(哨兵/标志);prev 不写(原生链表 prev 全 0)。
    /// - 删除(行数 < 节点数):真正从链表移除(新末尾 next=0;空列表则 head=0),
    ///   被移除的池节点清零以便复用。
    /// - 新增(行数 > 节点数):从节点池(动态 VirtualAllocEx,槽 0x40 对齐 web.ct)
    ///   找全 0 的空闲槽,按节点布局写入并挂到链表尾。找不到空闲槽则跳过该行。
    /// - 仅当原链表读全(尾部 next==0 或空链表)才追加,避免覆盖未读到的 next。
    std::pair<int, int> WriteAttributes(uintptr_t item_base,
                                        const std::vector<ItemProperty>& props);

    /// 清零动态节点池(所有槽回到空闲)。丢弃武器/读档后旧链作废时调用回收槽位。
    /// 返回 false = 池不存在或清零失败。
    bool ResetPool();

    /// 内存查看:读取 addr 处 size 字节。
    std::vector<unsigned char> ReadMemoryDump(uintptr_t addr, int size);

private:
    void ResetState();
    bool last_close_failed_ = false;      // 上次关闭未能完全还原
    std::string last_close_error_;        // 上次关闭失败原因
    void EnsureInjection();
    static bool IsHookOriginal(const std::vector<unsigned char>& orig);
    void Inject();
    static std::vector<unsigned char> BuildCave(uintptr_t cur_item_addr, uintptr_t cave_addr,
                                                uintptr_t return_addr, uint8_t disp,
                                                const CaptureCfg& capture,
                                                uintptr_t freeze_addr = 0,
                                                uintptr_t bag_flag_addr = 0,
                                                uint32_t bag_flag_value = 1,
                                                uintptr_t flag_last_addr = 0,
                                                uintptr_t flag_arm_addr = 0);
    uintptr_t FindCurItemSlot(uintptr_t hook_addr);

    // 捕获条件注册表:capture.type -> 生成"判断 + 捕获"段字节函数
    // (新增条件类型只需在此注册 + 配置 capture.type,无 if-else)。
    using CaptureBuilder = std::vector<unsigned char> (*)(uintptr_t cur_item_addr,
                                                          const CaptureCfg& cap);
    static std::map<std::string, CaptureBuilder> CaptureBuilders();
    static std::vector<unsigned char> BuildCapWeight(uintptr_t cur_item_addr,
                                                     const CaptureCfg& cap);
    static std::vector<unsigned char> BuildCapItemType(uintptr_t cur_item_addr,
                                                       const CaptureCfg& cap);

    // 节点池:动态分配池(VirtualAllocEx,惰性分配一次,槽位按 attr_list.slot_size)
    std::pair<uintptr_t, uint32_t> PoolCfg();
    bool IsPoolNode(uintptr_t addr);

    std::string ReadStringField(uintptr_t item_base, const BasicFieldCfg& f);
    void WriteStringField(uintptr_t item_base, const BasicFieldCfg& f, const std::string& text);

    std::vector<uintptr_t> ReadNodeAddrs(uintptr_t item_base);
    void WriteNode(uintptr_t addr, const ItemProperty& p);
    void NormalizeNode(uintptr_t addr);
    uintptr_t FindFreeStaticNode(const std::set<uintptr_t>& used);

    static int Clamp(const BasicFieldCfg& f, int v) { return f.Clamp(v); }
    void WriteField(uintptr_t item_base, const BasicFieldCfg& f, int v);

    OffsetsConfig config_;
    ProcessMemory mem_;
    unsigned int pid_ = 0;
    uintptr_t hook_addr_ = 0;
    uintptr_t cur_item_addr_ = 0;  // cur_item 槽地址
    uintptr_t freeze_addr_ = 0;    // 冻结闸门地址(自注入 cave 内;0=不支持)
    uintptr_t flag_last_addr_ = 0; // 背包拿起flag的"上次值"持久化dword(cave 边缘判断用)
    uintptr_t flag_arm_addr_ = 0;  // 边缘后的"捕获臂"计数(0=未武装;>0=下一拍捕获一次)
    bool bag_flag_active_ = false; // 背包拿起边缘门已启用
    uintptr_t alloc_base_ = 0;     // 自注入分配的基址(用于释放)
    uintptr_t pool_base_ = 0;      // 动态节点池基址
    uint32_t pool_size_ = 0;       // 动态节点池大小
    std::vector<unsigned char> orig_bytes_;  // 自注入前 hook 处原始 5 字节(用于还原)
};

}  // namespace poq
