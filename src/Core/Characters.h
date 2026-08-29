#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Native/ProcessMemory.h"

namespace poq {

// ---------------------------------------------------------------------------
// 队伍角色(5 人)属性结构体定义与读写(对应 Python poqeditor_py/characters.py)。
//
// 字段偏移(所有成员一致,已实机验证 5 人):
//   +0x00 姓名指针    +0x04 别名指针   +0x10 性别(0男/1女)  +0x0C 等级
//   +0x14 年龄        +0x1C 经验       +0x20 下一级          +0x28 声望(word)
//   +0x30/+0x34 生命当前/上限  +0x38/+0x3C 精气当前/上限
//   +0x48 力量 +0x4C 体质 +0x50 敏捷 +0x58 悟性 +0x5C 魅力 +0x60 智慧
//   +0x64/+0x68 普通攻击 min/max
//   +0x6C..+0x90 金木水火土攻击(min/max 各 2 个)
//   +0x94 普通防御  +0x98..+0xA8 金木水火土防御
//   +0xAC 闪避率  +0xB0 命中率
//   +0x274 剩余属性点(word)  +0x278 剩余技能点(word)
//   +0x2C 队长私有钱(全局金钱)
//   +0x398 职业(1游侠/2力士/3刺客/4术士/5巫师)
// ---------------------------------------------------------------------------

// 字段类型(决定读写与 UI 控件)。
enum class CharKind {
    Dword,      // 4 字节无符号
    Word,       // 2 字节无符号
    StringPtr,  // [base+offset] 指向 GBK 字符串
    Enum,       // 4 字节,值->显示名映射
    Pair,       // 两个 4 字节(下限/上限),offset_lo=下限 offset_hi=上限
};

// 一个角色字段。label 用于 UI;kind 决定读写;pair 时 offset_lo/offset_hi 为 (下限,上限)。
struct CharFieldCfg {
    std::string label;
    uint32_t offset_lo = 0;  // pair=下限偏移;其余=字段偏移
    uint32_t offset_hi = 0;  // 仅 pair=上限偏移
    CharKind kind = CharKind::Dword;
    std::map<int, std::string> enum_map;  // enum: 值->显示名
    std::optional<int> min;
    std::optional<int> max;
    std::optional<std::string> hint;
};

// 全部可选队友白名单(名字 -> 职业显示名,共 16 人;定位时槽内名字必须命中)。
// 下拉显示格式 = "职业-名字"(如 刺客-荆无忌)。
const std::map<std::string, std::string>& KnownMembers();

// 全局字段(5 人共享;金额位置待确认)。
const std::vector<CharFieldCfg>& GlobalFields();

// 每个角色的字段(全部成员共用同一偏移表)。
const std::vector<CharFieldCfg>& CharFields();

// 各版本引擎模块 -> (成员名 -> 偏移)。成员 EDI = 模块基址 + 偏移。
const std::map<std::string, std::map<std::string, uint32_t>>& MemberOffsets();

// 用固定偏移定位队伍成员(成员 EDI = 模块基址 + 偏移)。
//
// module 决定用哪个版本的偏移表(steam=EasyRPG.dll / v1.30=EasyRPG116.dll)。
// 固定偏移指向队伍槽位(槽 1-5),不是固定角色--队伍成员会随存档/换人变化,
// 因此动态读每个槽位的实际名字作为标识,只保留有效角色(名字可读、非空、
// 等级 1-99、生命/精气合理)。返回 [(姓名, 结构体地址), ...] 按槽位顺序,
// 结构体 = EDI + 0x28。用 vector 而非 map 以保留槽位顺序(对齐 Python dict)。
std::vector<std::pair<std::string, uintptr_t>> LocateByOffsets(ProcessMemory& mem, unsigned int pid,
                                                  const std::string& module,
                                                  bool strict_filter = true);

// 调试辅助:不放过滤地按槽位顺序读每个槽的名字(读失败记为占位符)。
// 用于 F12 调试验证"哪个槽扫不出来、哪个名字被白名单挡掉"(带 ⚠ 标记)。
std::vector<std::string> ProbeSlotNames(ProcessMemory& mem, unsigned int pid,
                                        const std::string& module);

// 字段值(读写共用)。pair 用 (lo,hi);string_ptr 用 str;其余用 lo。
struct CharValue {
    bool is_pair = false;
    int lo = 0;
    int hi = 0;
    std::string str;  // string_ptr(UTF-8)
};

// 读一个字段。string_ptr 返回 UTF-8 字符串;enum 返回 int;pair 返回 (lo,hi)。
// 失败(内存不可读等)返回 nullopt。
std::optional<CharValue> ReadCharField(ProcessMemory& mem, uintptr_t base, const CharFieldCfg& f);

// 写一个字段。失败返回 false。
bool WriteCharField(ProcessMemory& mem, uintptr_t base, const CharFieldCfg& f, const CharValue& v);

// 校验字段值是否合法。合法返回 nullopt,否则返回错误描述。
std::optional<std::string> ValidateValue(const CharFieldCfg& f, const CharValue& v);

// 读全部角色字段,返回 {label: 值}。
std::map<std::string, CharValue> ReadAllChar(ProcessMemory& mem, uintptr_t base);

// 读全局字段(按指定结构体读取)。
std::map<std::string, CharValue> ReadGlobalChar(ProcessMemory& mem, uintptr_t base);

}  // namespace poq
