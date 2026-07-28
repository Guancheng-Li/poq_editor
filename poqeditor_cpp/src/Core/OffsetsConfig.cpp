#include "Core/OffsetsConfig.h"

#include <cstdlib>
#include <string>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace poq {

// ----- BasicField -----

QJsonObject BasicField::ToJson() const {
    QJsonObject obj;
    obj["Label"] = QString::fromUtf8(label.c_str());
    obj["Offset"] = offset;
    return obj;
}

BasicField BasicField::FromJson(const QJsonObject& obj) {
    BasicField f;
    f.label = obj.value("Label").toString().toStdString();
    f.offset = obj.value("Offset").toInt(f.offset);
    return f;
}

// ----- PropertyListCfg -----

QJsonObject PropertyListCfg::ToJson() const {
    QJsonObject obj;
    obj["BaseOffsetFromItem"] = base_offset_from_item;
    obj["Count"] = count;
    obj["Stride"] = stride;
    obj["IndexOffset"] = index_offset;
    obj["NumOffset"] = num_offset;
    obj["ColorOffset"] = color_offset;
    obj["PrecentOffset"] = precent_offset;
    obj["ScanArray"] = scan_array;
    return obj;
}

PropertyListCfg PropertyListCfg::FromJson(const QJsonObject& obj) {
    PropertyListCfg p;
    p.base_offset_from_item = obj.value("BaseOffsetFromItem").toInt(p.base_offset_from_item);
    p.count = obj.value("Count").toInt(p.count);
    p.stride = obj.value("Stride").toInt(p.stride);
    p.index_offset = obj.value("IndexOffset").toInt(p.index_offset);
    p.num_offset = obj.value("NumOffset").toInt(p.num_offset);
    p.color_offset = obj.value("ColorOffset").toInt(p.color_offset);
    p.precent_offset = obj.value("PrecentOffset").toInt(p.precent_offset);
    p.scan_array = obj.value("ScanArray").toBool(p.scan_array);
    return p;
}

// ----- OffsetsConfig -----

long long OffsetsConfig::ParseLong(const std::string& s_in) {
    std::string s = s_in;
    // Trim whitespace.
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return 0;
    s = s.substr(a, b - a + 1);
    int base = 16;
    if (s.size() >= 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
        s = s.substr(2);
    }
    if (s.empty()) return 0;
    // strtoll handles an optional leading sign and the hex parse.
    return std::strtoll(s.c_str(), nullptr, base);
}

QJsonObject OffsetsConfig::ToJson() const {
    QJsonObject obj;
    obj["AnchorAddress"] = QString::fromUtf8(anchor_address.c_str());
    obj["ItemBaseAdjust"] = item_base_adjust;
    obj["ProcessAccess"] = QString::fromUtf8(process_access.c_str());
    obj["TemplateIni"] = QString::fromUtf8(template_ini.c_str());

    QJsonArray fields;
    for (const auto& f : basic_fields) fields.append(f.ToJson());
    obj["BasicFields"] = fields;

    obj["PropertyList"] = property_list.ToJson();
    return obj;
}

OffsetsConfig OffsetsConfig::FromJson(const QJsonObject& obj) {
    OffsetsConfig cfg;
    cfg.anchor_address = obj.value("AnchorAddress").toString(
        QString::fromUtf8(cfg.anchor_address.c_str())).toStdString();
    cfg.item_base_adjust = obj.value("ItemBaseAdjust").toInt(cfg.item_base_adjust);
    cfg.process_access = obj.value("ProcessAccess").toString(
        QString::fromUtf8(cfg.process_access.c_str())).toStdString();
    cfg.template_ini = obj.value("TemplateIni").toString(
        QString::fromUtf8(cfg.template_ini.c_str())).toStdString();

    cfg.basic_fields.clear();
    const QJsonArray fields = obj.value("BasicFields").toArray();
    for (const QJsonValue& v : fields) {
        if (v.isObject()) cfg.basic_fields.push_back(BasicField::FromJson(v.toObject()));
    }

    cfg.property_list = PropertyListCfg::FromJson(obj.value("PropertyList").toObject());
    return cfg;
}

OffsetsConfig OffsetsConfig::Load(const QString& path) {
    if (!QFileInfo::exists(path)) return Default();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return Default();
    QByteArray data = file.readAll();
    file.close();
    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return Default();
    return FromJson(doc.object());
}

void OffsetsConfig::Save(const QString& path) const {
    QJsonDocument doc(ToJson());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
}

OffsetsConfig OffsetsConfig::Default() {
    OffsetsConfig cfg;
    cfg.anchor_address = "0x901FF0";
    cfg.item_base_adjust = -240;
    cfg.process_access = "0x1F0FFF";
    cfg.template_ini = "EquMod.ini";
    cfg.basic_fields = {
        {"字段_0x00", 0x00},
        {"字段_0x04", 0x04},
        {"字段_0x08", 0x08},
        {"字段_0x0C", 0x0C},
        {"字段_0x10", 0x10},
        {"字段_0x14", 0x14},
        {"字段_0x20", 0x20},
        {"字段_0xA0", 0xA0},
        {"字段_0xCC", 0xCC},
        {"字段_0xF8", 0xF8},
        {"字段_0x110", 0x110},
        {"字段_0x268", 0x268},
        {"字段_0x404", 0x404},
    };
    cfg.property_list.base_offset_from_item = 0x404;
    cfg.property_list.count = 46;
    cfg.property_list.stride = 0x24;
    cfg.property_list.index_offset = 0x00;
    cfg.property_list.num_offset = 0x04;
    cfg.property_list.color_offset = 0x14;
    cfg.property_list.precent_offset = 0x20;
    cfg.property_list.scan_array = true;
    return cfg;
}

}  // namespace poq
