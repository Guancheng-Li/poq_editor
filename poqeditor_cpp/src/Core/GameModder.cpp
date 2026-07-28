#include "Core/GameModder.h"

#include <cstdio>
#include <stdexcept>
#include <string>

#include <QString>

namespace poq {

namespace {

std::string AddrHex8(uintptr_t addr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08llX", static_cast<unsigned long long>(addr));
    return buf;
}

}  // namespace

uintptr_t GameModder::Initialize(unsigned int pid) {
    if (!mem_.Open(pid, config_.ProcessAccessValue()))
        throw std::runtime_error("OpenProcess 失败（请以管理员身份运行）");
    // Verify the anchor is readable.
    unsigned int p = mem_.ReadUInt32(static_cast<uintptr_t>(config_.AnchorAddrValue()));
    if (p == 0)
        throw std::runtime_error("读取手持指针为 0，请先在游戏中拾起一件物品");
    return static_cast<uintptr_t>(p);
}

uintptr_t GameModder::GetHeldItemPointer() {
    return static_cast<uintptr_t>(mem_.ReadUInt32(static_cast<uintptr_t>(config_.AnchorAddrValue())));
}

uintptr_t GameModder::GetItemBase() {
    uintptr_t p = GetHeldItemPointer();
    return p + static_cast<uintptr_t>(static_cast<long long>(config_.item_base_adjust));
}

std::unordered_map<std::string, unsigned int> GameModder::ReadBasicFields(uintptr_t item_base) {
    std::unordered_map<std::string, unsigned int> dict;
    for (const auto& f : config_.basic_fields) {
        dict[f.label] = mem_.ReadUInt32(item_base + static_cast<uintptr_t>(f.offset));
    }
    return dict;
}

void GameModder::WriteBasicFields(uintptr_t item_base,
                                  const std::unordered_map<std::string, unsigned int>& values) {
    for (const auto& f : config_.basic_fields) {
        auto it = values.find(f.label);
        if (it != values.end()) {
            mem_.WriteUInt32(item_base + static_cast<uintptr_t>(f.offset), it->second);
        }
    }
}

std::vector<ItemProperty> GameModder::ReadProperties(uintptr_t item_base) {
    std::vector<ItemProperty> list;
    const PropertyListCfg& pl = config_.property_list;
    if (!pl.scan_array) return list;
    uintptr_t base_addr = item_base + static_cast<uintptr_t>(pl.base_offset_from_item);
    for (int i = 0; i < pl.count; ++i) {
        uintptr_t slot = base_addr + static_cast<uintptr_t>(static_cast<long long>(i) * pl.stride);
        try {
            int idx = mem_.ReadInt32(slot + static_cast<uintptr_t>(pl.index_offset));
            int num = mem_.ReadInt32(slot + static_cast<uintptr_t>(pl.num_offset));
            int color = mem_.ReadInt32(slot + static_cast<uintptr_t>(pl.color_offset));
            int prec = mem_.ReadInt32(slot + static_cast<uintptr_t>(pl.precent_offset));
            list.emplace_back(AddrHex8(slot), idx, num, color, prec);
        } catch (...) {
            // Out of range or unreadable: stop scanning, mirroring the C# try/catch break.
            break;
        }
    }
    return list;
}

void GameModder::WriteProperties(const std::vector<ItemProperty>& props) {
    const PropertyListCfg& pl = config_.property_list;
    for (const ItemProperty& p : props) {
        if (p.address.empty()) continue;
        uintptr_t addr = ProcessMemory::ParseAddress(p.address);
        mem_.WriteUInt32(addr + static_cast<uintptr_t>(pl.index_offset),
                         static_cast<unsigned int>(p.index));
        mem_.WriteUInt32(addr + static_cast<uintptr_t>(pl.num_offset),
                         static_cast<unsigned int>(p.num));
        mem_.WriteUInt32(addr + static_cast<uintptr_t>(pl.color_offset),
                         static_cast<unsigned int>(p.color));
        mem_.WriteUInt32(addr + static_cast<uintptr_t>(pl.precent_offset),
                         static_cast<unsigned int>(p.precent));
    }
}

}  // namespace poq
