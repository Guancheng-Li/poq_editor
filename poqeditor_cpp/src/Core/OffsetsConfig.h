#pragma once

#include <string>
#include <vector>

#include <QJsonObject>
#include <QJsonDocument>
#include <QString>

namespace poq {

/// A single basic (non-template) item field: a label and its offset relative to
/// the item base address. Mirrors the C# `BasicField` class.
struct BasicField {
    std::string label;
    int offset = 0;

    BasicField() = default;
    BasicField(std::string label_value, int offset_value)
        : label(std::move(label_value)), offset(offset_value) {}

    QJsonObject ToJson() const;
    static BasicField FromJson(const QJsonObject& obj);
};

/// Layout of the 46-entry property array. Mirrors `PropertyListCfg`.
struct PropertyListCfg {
    int base_offset_from_item = 0x404;
    int count = 46;
    int stride = 0x24;
    int index_offset = 0x00;
    int num_offset = 0x04;
    int color_offset = 0x14;
    int precent_offset = 0x20;
    bool scan_array = true;

    QJsonObject ToJson() const;
    static PropertyListCfg FromJson(const QJsonObject& obj);
};

/// All configurable offsets/addresses. Mirrors `OffsetsConfig`. Every address and
/// offset is adjustable here so the binary need not be rebuilt when calibrating
/// against the real POQ.EXE v1.30.
class OffsetsConfig {
public:
    std::string anchor_address = "0x901FF0";
    int item_base_adjust = -240;
    std::string process_access = "0x1F0FFF";
    std::vector<BasicField> basic_fields;
    PropertyListCfg property_list;
    std::string template_ini = "EquMod.ini";

    /// Parse a numeric string that may be decimal or "0x"-prefixed hex, returning
    /// the value as a signed 64-bit integer (matches C# `ParseLong`).
    static long long ParseLong(const std::string& s);

    /// Parsed anchor address (the fixed location holding the held-item pointer).
    long long AnchorAddrValue() const { return ParseLong(anchor_address); }

    /// Parsed process access mask (e.g. 0x1F0FFF = PROCESS_ALL_ACCESS).
    unsigned int ProcessAccessValue() const {
        return static_cast<unsigned int>(ParseLong(process_access));
    }

    /// Load from a JSON file (UTF-8). If the file is missing, returns Default().
    static OffsetsConfig Load(const QString& path);

    /// Save to a JSON file (UTF-8, indented).
    void Save(const QString& path) const;

    QJsonObject ToJson() const;
    static OffsetsConfig FromJson(const QJsonObject& obj);

    /// Default offsets reverse-engineered from the original poq_hack.exe. These
    /// may need calibration against the actual POQ.EXE v1.30.
    static OffsetsConfig Default();
};

}  // namespace poq
