#include "Core/IniTemplate.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace poq {

namespace {

// Trim ASCII whitespace from both ends.
std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Parse an int, returning 0 on failure (mirrors C# int.TryParse semantics).
int ParseIntOrZero(const std::string& s) {
    if (s.empty()) return 0;
    try {
        return std::stoi(s);
    } catch (...) {
        return 0;
    }
}

// Case-insensitive "starts with".
bool StartsWithI(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

}  // namespace

std::vector<ItemProperty> IniTemplate::Load(const QString& path) {
    std::vector<ItemProperty> result;
    if (!QFileInfo::exists(path)) return result;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return result;
    // UTF-8 (the "Text" flag also enables universal-newline conversion; the
    // explicit codec keeps things consistent on all platforms).
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);

    // Case-insensitive key->value map (keys stored lowercased).
    std::map<std::string, std::string> dict;
    while (!in.atEnd()) {
        std::string line = in.readLine().toStdString();
        std::string t = Trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '[') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(t.substr(0, eq));
        std::string val = Trim(t.substr(eq + 1));
        dict[ToLower(key)] = val;
    }
    file.close();

    int count = 0;
    auto it = dict.find("count");
    if (it != dict.end()) count = ParseIntOrZero(it->second);

    // Collect every index N that appears in any IndexN/NumN/ColorN/PrecentN key.
    std::set<int> nums;
    static const std::vector<std::string> kPrefixes = {"index", "num", "color", "precent"};
    for (const auto& kv : dict) {
        const std::string& k = kv.first;
        for (const std::string& prefix : kPrefixes) {
            if (StartsWithI(k, prefix) && k.size() > prefix.size()) {
                std::string suffix = k.substr(prefix.size());
                int idx = ParseIntOrZero(suffix);
                if (idx != 0 || suffix.find_first_not_of("0123456789") == std::string::npos) {
                    nums.insert(idx);
                }
            }
        }
    }

    std::vector<int> ordered(nums.begin(), nums.end());
    if (ordered.empty() && count > 0) {
        ordered.reserve(count);
        for (int i = 1; i <= count; ++i) ordered.push_back(i);
    }

    for (int i : ordered) {
        auto get = [&](const std::string& prefix) -> int {
            auto it2 = dict.find(ToLower(prefix + std::to_string(i)));
            return it2 == dict.end() ? 0 : ParseIntOrZero(it2->second);
        };
        result.emplace_back(/*addr=*/"", get("Index"), get("Num"), get("Color"), get("Precent"));
    }
    return result;
}

void IniTemplate::Save(const QString& path, const std::vector<ItemProperty>& props) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out.setGenerateByteOrderMark(false);  // UTF-8 without BOM.

    // CRLF line endings, matching the original VB6 ini parser's expectations.
    auto line = [&](const QString& s) { out << s << "\r\n"; };

    line(";如不要擅自改动，会造成程序无法正常运行");
    line(";EquCount 模板共有属性数");
    line(";EquIndex 属性的代码");
    line(";EquNum1 属性的数值");
    line(";EquColor1 属性的颜色");
    line(";EquPrecent 属性是否是百分比");
    line("[Mod]");
    line(QString("Count=%1").arg(props.size()));
    for (size_t i = 0; i < props.size(); ++i) {
        const ItemProperty& p = props[i];
        int n = static_cast<int>(i) + 1;
        line(QString("Index%1=%2").arg(n).arg(p.index));
        line(QString("Num%1=%2").arg(n).arg(p.num));
        line(QString("Color%1=%2").arg(n).arg(p.color));
        line(QString("Precent%1=%2").arg(n).arg(p.precent));
    }
    file.close();
}

}  // namespace poq
