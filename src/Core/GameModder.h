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
///   `mov eax,[esi+eax*4+disp8]; pop esi`(5 字节 `8B 44 86 ?? 5E`),当
///   [esi+weight_offset](重量) <= weight_max 时把 esi(物品指针)写入 cur_item 槽。
/// - 本工具自注入,不依赖任何 CT 文件:初始化时若未注入则自行分配 code cave、
///   写入与 CT 等价的 cave 代码并 patch 5 字节 jmp;若发现已注入(0xE9,例如 CT 或
///   上次残留)则解析现有 cave 复用其 cur_item 槽,不重复注入。
/// - 物品基址 = cur_item 槽的值 + item_base_adjust(默认 0,无 -0xF0 调整)。
/// - 基础属性:物品基址 + 偏移。
/// - 非固定属性:链表,头指针在 [物品基址 + attr_list.head_offset],节点字段见
///   AttrListCfg(next/id/color/precent/value 等)。新增节点用静态节点池。
class GameModder {
public:
    explicit GameModder(OffsetsConfig config) : config_(std::move(config)) {}

    const OffsetsConfig& Config() const { return config_; }
    OffsetsConfig& Config() { return config_; }
    ProcessMemory& Mem() { return mem_; }

    /// 是否已注入(cur_item 槽地址非 0)。
    bool IsInjected() const { return cur_item_addr_ != 0; }
    /// cur_item 槽地址(0 = 未注入)。
    uintptr_t CurItemSlotAddr() const { return cur_item_addr_; }
    /// hook 指令地址。
    uintptr_t HookAddr() const { return hook_addr_; }

    /// 打开进程,完成/复用 AOB 注入以捕获 cur_item。成功返回当前物品指针(可能为 0)。
    ///
    /// 不依赖 CT 文件:未注入时自行注入(与 CT 等价的 code cave + jmp patch);
    /// 已注入(0xE9,例如 CT 或上次残留)时解析现有 cave 复用其 cur_item 槽。
    ///
    /// Throws std::runtime_error: 打开失败、找不到模块、hook 字节异常等。
    uintptr_t Initialize(unsigned int pid);

    /// 读取目标进程 hook 处实际字节,判断是否已被注入(0xE9 = jmp)。
    bool IsHookInjected(unsigned int pid);

    /// 关闭进程句柄。若是本工具自注入,则还原原始字节并释放 cave;复用外部注入则不还原。
    void Close();

    /// 读取 cur_item 槽 = 当前物品指针(0 表示当前无物品)。
    uintptr_t GetItemPointer();

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
    /// 只读字段忽略。校验:clamp 到 [min,max];耐久度 <= 耐久度上限;改上限且
    /// 新上限>原耐久度时写入耐久度=新上限;弓箭数量仅类型∈{13,14}时写入;
    /// 类型∈{13,14}(箭/矢)时:孔数=0、耐久度=-1、耐久度上限=-1(强制值,bypass clamp)。
    void WriteBasicFields(uintptr_t item_base, const BasicState& values);

    /// 遍历链表读取所有非固定属性。
    std::vector<ItemProperty> ReadAttributes(uintptr_t item_base);

    /// 把属性写回链表。返回 (写入条数, 跳过条数)。
    ///
    /// 安全策略(对齐原版修改器):
    /// - 现有节点:只改字段值 + 自愈(哨兵/标志/前驱指针)。
    /// - 删除(行数 < 节点数):真正从链表移除(新末尾 next=0;空列表则 head=0),
    ///   被移除的静态池节点清零以便复用。
    /// - 新增(行数 > 节点数):从静态节点池(attr_list.static_*,游戏静态零缓冲)
    ///   找全 0 的 static_slot 槽,按节点布局写入并挂到链表尾。找不到空闲槽则跳过该行。
    /// - 仅当原链表读全(尾部 next==0 或空链表)才追加,避免覆盖未读到的 next。
    std::pair<int, int> WriteAttributes(uintptr_t item_base,
                                        const std::vector<ItemProperty>& props);

    /// 内存查看:读取 addr 处 size 字节。
    std::vector<unsigned char> ReadMemoryDump(uintptr_t addr, int size);

private:
    void ResetState();
    void EnsureInjection();
    static bool IsHookOriginal(const std::vector<unsigned char>& orig);
    void Inject();
    static std::vector<unsigned char> BuildCave(uintptr_t cur_item_addr, uintptr_t cave_addr,
                                                uintptr_t return_addr, uint8_t disp);
    uintptr_t FindCurItemSlot(uintptr_t hook_addr);

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
    uintptr_t alloc_base_ = 0;     // 自注入分配的基址(用于释放)
    std::vector<unsigned char> orig_bytes_;  // 自注入前 hook 处原始 5 字节(用于还原)
};

}  // namespace poq
