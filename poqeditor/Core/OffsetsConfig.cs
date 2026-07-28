using System.Text.Json;
using System.Text.Json.Serialization;

namespace PoqEditor.Core;

/// <summary>偏移配置：全部内存地址/偏移都可在此文件调整，无需重新编译。</summary>
public class OffsetsConfig
{
    /// <summary>游戏内存中“当前手持物品指针”的固定地址（十进制或 0x 十六进制）。</summary>
    public string AnchorAddress { get; set; } = "0x901FF0";

    /// <summary>手持指针指向物品对象中部，需要减去该值得到物品基址（-0xF0 = -240）。</summary>
    public int ItemBaseAdjust { get; set; } = -240;

    /// <summary>打开进程的访问权限（默认 PROCESS_ALL_ACCESS = 0x1F0FFF）。</summary>
    public string ProcessAccess { get; set; } = "0x1F0FFF";

    /// <summary>物品基础属性（耐久/重量/孔数/需求等），相对物品基址的偏移。</summary>
    public List<BasicField> BasicFields { get; set; } = new();

    /// <summary>属性列表（46 条属性模板）的内存布局。</summary>
    public PropertyListCfg PropertyList { get; set; } = new();

    /// <summary>模板 ini 文件名（相对于程序目录）。</summary>
    public string TemplateIni { get; set; } = "EquMod.ini";

    [JsonIgnore] public long AnchorAddrValue => ParseLong(AnchorAddress);
    [JsonIgnore] public uint ProcessAccessValue => (uint)ParseLong(ProcessAccess);

    public static long ParseLong(string s)
    {
        s = s.Trim();
        if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) s = s[2..];
        return Convert.ToInt64(s, 16);
    }

    public static OffsetsConfig Load(string path)
    {
        if (!File.Exists(path)) return Default();
        var json = File.ReadAllText(path);
        var cfg = JsonSerializer.Deserialize<OffsetsConfig>(json, new JsonSerializerOptions
        { ReadCommentHandling = JsonCommentHandling.Skip, PropertyNameCaseInsensitive = true }) ?? Default();
        return cfg;
    }

    public void Save(string path) =>
        File.WriteAllText(path, JsonSerializer.Serialize(this, new JsonSerializerOptions
        { WriteIndented = true }));

    /// <summary>默认偏移（来自对原版 poq_hack.exe 的逆向，可能需要针对实际 POQ.EXE v1.30 校准）。</summary>
    public static OffsetsConfig Default() => new()
    {
        AnchorAddress = "0x901FF0",
        ItemBaseAdjust = -240,
        ProcessAccess = "0x1F0FFF",
        BasicFields = new List<BasicField>
        {
            new("字段_0x00", 0x00),
            new("字段_0x04", 0x04),
            new("字段_0x08", 0x08),
            new("字段_0x0C", 0x0C),
            new("字段_0x10", 0x10),
            new("字段_0x14", 0x14),
            new("字段_0x20", 0x20),
            new("字段_0xA0", 0xA0),
            new("字段_0xCC", 0xCC),
            new("字段_0xF8", 0xF8),
            new("字段_0x110", 0x110),
            new("字段_0x268", 0x268),
            new("字段_0x404", 0x404),
        },
        PropertyList = new PropertyListCfg
        {
            // 属性数组起点（相对物品基址）。0x404 是逆向中反复出现的偏移，作为起点猜测。
            BaseOffsetFromItem = 0x404,
            Count = 46,
            // 每个属性槽的步长（字节）。槽内字段最大偏移 0x20，故步长至少 0x24。
            Stride = 0x24,
            IndexOffset = 0x00,
            NumOffset = 0x04,
            ColorOffset = 0x14,
            PrecentOffset = 0x20,
            // 读取属性时是否按固定数组扫描；false 则仅依赖用户在网格里填的地址。
            ScanArray = true,
        },
        TemplateIni = "EquMod.ini",
    };
}

public class BasicField
{
    public string Label { get; set; } = "";
    public int Offset { get; set; }
    public BasicField() { }
    public BasicField(string label, int offset) { Label = label; Offset = offset; }
}

public class PropertyListCfg
{
    public int BaseOffsetFromItem { get; set; }
    public int Count { get; set; } = 46;
    public int Stride { get; set; } = 0x24;
    public int IndexOffset { get; set; } = 0x00;
    public int NumOffset { get; set; } = 0x04;
    public int ColorOffset { get; set; } = 0x14;
    public int PrecentOffset { get; set; } = 0x20;
    public bool ScanArray { get; set; } = true;
}
