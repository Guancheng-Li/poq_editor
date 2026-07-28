#pragma once

#include <string>
#include <vector>

#include <QString>

#include "Core/ItemProperty.h"

namespace poq {

/// Reader/writer for the EquMod.ini template (UTF-8, keys IndexN/NumN/ColorN/
/// PrecentN plus Count). Mirrors the C# `IniTemplate` static class.
class IniTemplate {
public:
    /// Load the template from a UTF-8 file. Returns an empty vector if the file
    /// is missing or contains no entries.
    static std::vector<ItemProperty> Load(const QString& path);

    /// Write the template to a UTF-8 file (no BOM, CRLF line endings) in the
    /// format expected by the original VB6 tool.
    static void Save(const QString& path, const std::vector<ItemProperty>& props);
};

}  // namespace poq
