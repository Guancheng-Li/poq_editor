using System.Text;

namespace PoqEditor.Core;

/// <summary>读取/写入 EquMod.ini 模板（UTF-8 编码，键 IndexN/NumN/ColorN/PrecentN）。</summary>
public static class IniTemplate
{
    public static List<ItemProperty> Load(string path)
    {
        var result = new List<ItemProperty>();
        if (!File.Exists(path)) return result;
        // 统一使用 UTF-8（兼容带 BOM 的文件）
        var lines = File.ReadAllLines(path, new UTF8Encoding(false));
        var dict = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var line in lines)
        {
            var t = line.Trim();
            if (t.Length == 0 || t.StartsWith(";") || t.StartsWith("[")) continue;
            var eq = t.IndexOf('=');
            if (eq < 0) continue;
            dict[t[..eq].Trim()] = t[(eq + 1)..].Trim();
        }
        int count = dict.TryGetValue("Count", out var c0) && int.TryParse(c0, out var c1) ? c1 : 0;
        var nums = new HashSet<int>();
        foreach (var k in dict.Keys)
        {
            foreach (var prefix in new[] { "Index", "Num", "Color", "Precent" })
            {
                if (k.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) &&
                    int.TryParse(k[prefix.Length..], out var idx))
                    nums.Add(idx);
            }
        }
        var ordered = nums.OrderBy(x => x).ToList();
        if (ordered.Count == 0 && count > 0)
            ordered = Enumerable.Range(1, count).ToList();
        foreach (var i in ordered)
        {
            int.TryParse(dict.GetOrDefault($"Index{i}"), out var idx);
            int.TryParse(dict.GetOrDefault($"Num{i}"), out var num);
            int.TryParse(dict.GetOrDefault($"Color{i}"), out var color);
            int.TryParse(dict.GetOrDefault($"Precent{i}"), out var prec);
            result.Add(new ItemProperty { Index = idx, Num = num, Color = color, Precent = prec });
        }
        return result;
    }

    public static void Save(string path, IEnumerable<ItemProperty> props)
    {
        var sb = new StringBuilder();
        sb.AppendLine(";如不要擅自改动，会造成程序无法正常运行");
        sb.AppendLine(";EquCount 模板共有属性数");
        sb.AppendLine(";EquIndex 属性的代码");
        sb.AppendLine(";EquNum1 属性的数值");
        sb.AppendLine(";EquColor1 属性的颜色");
        sb.AppendLine(";EquPrecent 属性是否是百分比");
        sb.AppendLine("[Mod]");
        var list = props.ToList();
        sb.AppendLine($"Count={list.Count}");
        for (int i = 0; i < list.Count; i++)
        {
            var p = list[i];
            int n = i + 1;
            sb.AppendLine($"Index{n}={p.Index}");
            sb.AppendLine($"Num{n}={p.Num}");
            sb.AppendLine($"Color{n}={p.Color}");
            sb.AppendLine($"Precent{n}={p.Precent}");
        }
        // UTF-8 不带 BOM，CRLF 行尾，兼容原版 ini 解析
        File.WriteAllText(path, sb.ToString().Replace("\n", "\r\n"), new UTF8Encoding(false));
    }
}

internal static class DictExt
{
    public static string? GetOrDefault(this Dictionary<string, string> d, string key) =>
        d.TryGetValue(key, out var v) ? v : null;
}
