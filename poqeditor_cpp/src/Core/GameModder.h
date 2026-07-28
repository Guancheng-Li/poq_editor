#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/ItemProperty.h"
#include "Core/OffsetsConfig.h"
#include "Native/ProcessMemory.h"

namespace poq {

/// Core memory-editing logic. All addresses/offsets come from OffsetsConfig and
/// are adjustable. Algorithm (reverse-engineered from the original poq_hack.exe):
///   1. OpenProcess(PROCESS_ALL_ACCESS, pid)
///   2. Read [AnchorAddress] -> held-item pointer P
///   3. item_base = P + ItemBaseAdjust
///   4. Read/write item fields (basic fields + 46 property slots)
/// Mirrors the C# `GameModder` class.
class GameModder {
public:
    explicit GameModder(OffsetsConfig config) : config_(std::move(config)) {}

    const OffsetsConfig& Config() const { return config_; }
    OffsetsConfig& Config() { return config_; }
    ProcessMemory& Mem() { return mem_; }

    /// Initialize: open the process and verify the anchor is readable. Returns
    /// the held-item pointer P. Throws std::runtime_error on failure.
    uintptr_t Initialize(unsigned int pid);

    /// The current held-item pointer (the value stored at [AnchorAddress]).
    uintptr_t GetHeldItemPointer();

    /// item_base = held pointer + ItemBaseAdjust.
    uintptr_t GetItemBase();

    /// Read all basic fields, keyed by label.
    std::unordered_map<std::string, unsigned int> ReadBasicFields(uintptr_t item_base);

    /// Write the basic fields present in `values`.
    void WriteBasicFields(uintptr_t item_base,
                          const std::unordered_map<std::string, unsigned int>& values);

    /// Scan the property array layout, returning a list with addresses set.
    std::vector<ItemProperty> ReadProperties(uintptr_t item_base);

    /// Write each property back to its own `address` field.
    void WriteProperties(const std::vector<ItemProperty>& props);

private:
    OffsetsConfig config_;
    ProcessMemory mem_;
};

}  // namespace poq
