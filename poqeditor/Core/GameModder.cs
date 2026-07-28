using PoqEditor.Native;

namespace PoqEditor.Core;

/// <summary>
/// 秦殇物品内存修改核心逻辑。所有地址/偏移来自 <see cref="OffsetsConfig"/>，可调整。
/// 算法（逆向自原版 poq_hack.exe）：
///   1) OpenProcess(PROCESS_ALL_ACCESS, pid)
///   2) 读 [AnchorAddress] 得到手持物品指针 P
///   3) 物品基址 = P + ItemBaseAdjust
///   4) 读/写物品各字段（基础属性 + 46 条属性槽）
/// </summary>
public sealed class GameModder
{
    public OffsetsConfig Config { get; }
    public ProcessMemory Mem { get; } = new();

    public GameModder(OffsetsConfig config) { Config = config; }

    /// <summary>初始化：打开进程并验证锚点可读。成功返回手持指针。</summary>
    public IntPtr Initialize(uint pid)
    {
        if (!Mem.Open(pid, Config.ProcessAccessValue))
            throw new InvalidOperationException("OpenProcess 失败（请以管理员身份运行）");
        // 验证锚点可读
        var p = Mem.ReadUInt32(new IntPtr(Config.AnchorAddrValue));
        if (p == 0)
            throw new InvalidOperationException("读取手持指针为 0，请先在游戏中拾起一件物品");
        return new IntPtr(p);
    }

    /// <summary>当前手持物品指针 [AnchorAddress]。</summary>
    public IntPtr GetHeldItemPointer() => new(Mem.ReadUInt32(new IntPtr(Config.AnchorAddrValue)));

    /// <summary>物品基址 = 手持指针 + ItemBaseAdjust。</summary>
    public IntPtr GetItemBase()
    {
        long p = GetHeldItemPointer().ToInt64();
        return new IntPtr(p + Config.ItemBaseAdjust);
    }

    /// <summary>读取所有基础属性字段。</summary>
    public Dictionary<string, uint> ReadBasicFields(IntPtr itemBase)
    {
        var dict = new Dictionary<string, uint>();
        foreach (var f in Config.BasicFields)
            dict[f.Label] = Mem.ReadUInt32(new IntPtr(itemBase.ToInt64() + f.Offset));
        return dict;
    }

    /// <summary>写入基础属性字段。</summary>
    public void WriteBasicFields(IntPtr itemBase, Dictionary<string, uint> values)
    {
        foreach (var f in Config.BasicFields)
        {
            if (values.TryGetValue(f.Label, out var v))
                Mem.WriteUInt32(new IntPtr(itemBase.ToInt64() + f.Offset), v);
        }
    }

    /// <summary>按配置的属性数组布局扫描当前物品的属性槽，返回带地址的属性列表。</summary>
    public List<ItemProperty> ReadProperties(IntPtr itemBase)
    {
        var list = new List<ItemProperty>();
        var pl = Config.PropertyList;
        if (!pl.ScanArray) return list;
        long baseAddr = itemBase.ToInt64() + pl.BaseOffsetFromItem;
        for (int i = 0; i < pl.Count; i++)
        {
            long slot = baseAddr + i * pl.Stride;
            try
            {
                int idx = Mem.ReadInt32(new IntPtr(slot + pl.IndexOffset));
                int num = Mem.ReadInt32(new IntPtr(slot + pl.NumOffset));
                int color = Mem.ReadInt32(new IntPtr(slot + pl.ColorOffset));
                int prec = Mem.ReadInt32(new IntPtr(slot + pl.PrecentOffset));
                list.Add(new ItemProperty
                {
                    Address = slot.ToString("X8"),
                    Index = idx,
                    Num = num,
                    Color = color,
                    Precent = prec
                });
            }
            catch
            {
                // 越界或不可读则停止扫描
                break;
            }
        }
        return list;
    }

    /// <summary>把网格中的属性写回各自地址（地址取自每行的 Address 列）。</summary>
    public void WriteProperties(IEnumerable<ItemProperty> props)
    {
        foreach (var p in props)
        {
            if (string.IsNullOrWhiteSpace(p.Address)) continue;
            var addr = ProcessMemory.ParseAddress(p.Address);
            Mem.WriteUInt32(new IntPtr(addr.ToInt64() + Config.PropertyList.IndexOffset), (uint)p.Index);
            Mem.WriteUInt32(new IntPtr(addr.ToInt64() + Config.PropertyList.NumOffset), (uint)p.Num);
            Mem.WriteUInt32(new IntPtr(addr.ToInt64() + Config.PropertyList.ColorOffset), (uint)p.Color);
            Mem.WriteUInt32(new IntPtr(addr.ToInt64() + Config.PropertyList.PrecentOffset), (uint)p.Precent);
        }
    }
}
