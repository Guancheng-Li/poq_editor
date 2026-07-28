using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using PoqEditor.Core;
using PoqEditor.Native;

namespace PoqEditor;

public partial class MainWindow : Window
{
    private OffsetsConfig _cfg = null!;
    private GameModder _modder = null!;
    private readonly ObservableCollection<ItemProperty> _props = new();
    private readonly Dictionary<string, TextBox> _basicBoxes = new();
    private readonly Dictionary<string, TextBox> _cfgBoxes = new();
    private List<ProcessInfo> _processes = new();

    private static string AppDir => AppDomain.CurrentDomain.BaseDirectory;
    private static string CfgPath => Path.Combine(AppDir, "offsets.json");

    public MainWindow()
    {
        InitializeComponent();
        PropGrid.DataContext = _props;
    }

    private void Window_Loaded(object sender, RoutedEventArgs e)
    {
        ReloadConfig();
        RefreshProcesses();
    }

    private void ReloadConfig()
    {
        _cfg = OffsetsConfig.Load(CfgPath);
        _modder = new GameModder(_cfg);
        BuildBasicFieldsPanel();
        BuildConfigPanel();
    }

    // ---------- 进程 ----------
    private void BtnRefresh_Click(object sender, RoutedEventArgs e) => RefreshProcesses();

    private void RefreshProcesses()
    {
        _processes = ProcessMemory.EnumerateProcesses();
        CbProcess.Items.Clear();
        ProcessInfo? poq = null;
        foreach (var p in _processes)
        {
            CbProcess.Items.Add(p);
            if (p.Name.Equals("POQ.exe", StringComparison.OrdinalIgnoreCase) ||
                p.Name.Equals("POQ.EXE", StringComparison.OrdinalIgnoreCase))
                poq = p;
        }
        if (poq != null) CbProcess.SelectedItem = poq;
        Status($"已列出 {_processes.Count} 个进程。{(poq != null ? "已自动选中 POQ.EXE。" : "未找到 POQ.EXE，请手动选择或启动游戏。")}");
    }

    private bool TryGetSelectedPid(out uint pid)
    {
        pid = 0;
        if (CbProcess.SelectedItem is ProcessInfo pi) { pid = pi.Pid; return true; }
        // 用户可能手输入了文本
        var txt = CbProcess.Text;
        if (uint.TryParse(txt.AsSpan(), out pid)) return true;
        // 按名字匹配
        var match = _processes.FirstOrDefault(p => p.Name.Equals(txt, StringComparison.OrdinalIgnoreCase));
        if (match != null) { pid = match.Pid; return true; }
        return false;
    }

    private void BtnInit_Click(object sender, RoutedEventArgs e)
    {
        if (!TryGetSelectedPid(out var pid)) { Status("请先选择一个进程。"); return; }
        try
        {
            var ptr = _modder.Initialize(pid);
            Status($"初始化成功。手持指针 = 0x{ptr.ToInt64():X8}（物品基址 = 0x{_modder.GetItemBase().ToInt64():X8}）。");
        }
        catch (Exception ex) { Status("初始化失败：" + ex.Message); }
    }

    // ---------- 基础属性面板 ----------
    private void BuildBasicFieldsPanel()
    {
        BasicFieldsPanel.Children.Clear();
        _basicBoxes.Clear();
        foreach (var f in _cfg.BasicFields)
        {
            var b = new Border { BorderBrush = System.Windows.Media.Brushes.LightGray, BorderThickness = new Thickness(0.5), Padding = new Thickness(6,3,6,3), Margin = new Thickness(3) };
            var sp = new StackPanel { Orientation = Orientation.Horizontal };
            var lbl = new TextBlock { Text = $"{f.Label} (+0x{f.Offset:X})", VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0,0,6,0) };
            var tb = new TextBox { Width = 90, Text = "0" };
            sp.Children.Add(lbl); sp.Children.Add(tb);
            b.Child = sp;
            BasicFieldsPanel.Children.Add(b);
            _basicBoxes[f.Label] = tb;
        }
    }

    // ---------- 属性按钮 ----------
    private void BtnRead_Click(object sender, RoutedEventArgs e)
    {
        if (!_modder.Mem.IsOpen) { Status("请先点击“初始化”。"); return; }
        try
        {
            var ib = _modder.GetItemBase();
            // 基础属性
            var basic = _modder.ReadBasicFields(ib);
            foreach (var (k, v) in basic)
                if (_basicBoxes.TryGetValue(k, out var tb)) tb.Text = v.ToString();
            // 属性列表
            _props.Clear();
            foreach (var p in _modder.ReadProperties(ib)) _props.Add(p);
            UpdateCount();
            Status($"读取完成。物品基址 = 0x{ib.ToInt64():X8}，属性 {_props.Count} 条。");
        }
        catch (Exception ex) { Status("读取失败：" + ex.Message); }
    }

    private void BtnLoadTpl_Click(object sender, RoutedEventArgs e)
    {
        var path = Path.Combine(AppDir, _cfg.TemplateIni);
        try
        {
            var tpl = IniTemplate.Load(path);
            if (tpl.Count == 0) { Status("模板为空或文件不存在：" + path); return; }
            // 如果网格已有地址（读取过），则按 Index 匹配模板数值；否则直接填入模板
            if (_props.Count > 0 && _props.Any(p => !string.IsNullOrWhiteSpace(p.Address)))
            {
                foreach (var row in _props)
                {
                    var m = tpl.FirstOrDefault(t => t.Index == row.Index);
                    if (m != null) { row.Num = m.Num; row.Color = m.Color; row.Precent = m.Precent; }
                }
                Status($"已按 Index 匹配模板数值到现有 {_props.Count} 条属性。");
            }
            else
            {
                _props.Clear();
                foreach (var p in tpl) _props.Add(p);
                Status($"已从模板载入 {_props.Count} 条属性（地址列为空，保存前请先“读取属性”或手填地址）。");
            }
            UpdateCount();
        }
        catch (Exception ex) { Status("载入模板失败：" + ex.Message); }
    }

    private void BtnSave_Click(object sender, RoutedEventArgs e)
    {
        if (!_modder.Mem.IsOpen) { Status("请先点击“初始化”。"); return; }
        if (_props.Count == 0 || _props.All(p => string.IsNullOrWhiteSpace(p.Address)))
        { Status("没有可写入的属性地址。请先“读取属性”或在地址列填入地址。"); return; }
        try
        {
            var ib = _modder.GetItemBase();
            // 基础属性
            var basic = new Dictionary<string, uint>();
            foreach (var (k, tb) in _basicBoxes)
                if (uint.TryParse(tb.Text, out var v)) basic[k] = v;
            _modder.WriteBasicFields(ib, basic);
            // 属性列表
            _modder.WriteProperties(_props);
            Status($"保存完成：基础属性 {basic.Count} 项，属性 {_props.Count(p => !string.IsNullOrWhiteSpace(p.Address))} 条已写入。请在游戏中把物品扔到地上再捡起使修改生效。");
        }
        catch (Exception ex) { Status("保存失败：" + ex.Message); }
    }

    private void BtnAdd_Click(object sender, RoutedEventArgs e)
    {
        _props.Add(new ItemProperty());
        UpdateCount();
    }
    private void BtnDel_Click(object sender, RoutedEventArgs e)
    {
        var sel = PropGrid.SelectedItems.Cast<ItemProperty>().ToList();
        if (sel.Count == 0) { Status("请先在表格里选中要删除的行。"); return; }
        foreach (var p in sel) _props.Remove(p);
        UpdateCount();
    }
    private void BtnClear_Click(object sender, RoutedEventArgs e)
    {
        _props.Clear();
        UpdateCount();
    }
    private void UpdateCount() => LbCount.Text = $"属性数：{_props.Count}";

    // ---------- 内存查看 ----------
    private void BtnRawRead_Click(object sender, RoutedEventArgs e)
    {
        if (!_modder.Mem.IsOpen) { Status("请先“初始化”。"); return; }
        try
        {
            var addr = ProcessMemory.ParseAddress(RawAddr.Text);
            int size = int.TryParse(RawSize.Text, out var s) ? s : 64;
            var data = _modder.Mem.Read(addr, size);
            RawHex.Text = FormatHexDump(addr.ToInt64(), data);
            Status($"已读取 0x{addr.ToInt64():X8} 共 {data.Length} 字节。");
        }
        catch (Exception ex) { Status("读取失败：" + ex.Message); }
    }

    private void BtnRawWrite_Click(object sender, RoutedEventArgs e)
    {
        if (!_modder.Mem.IsOpen) { Status("请先“初始化”。"); return; }
        try
        {
            var addr = ProcessMemory.ParseAddress(RawAddr.Text);
            var bytes = ParseHexDump(RawHex.Text);
            if (bytes.Count == 0) { Status("没有可写入的十六进制字节。"); return; }
            _modder.Mem.Write(addr, bytes.ToArray());
            Status($"已写入 0x{addr.ToInt64():X8} 共 {bytes.Count} 字节。");
        }
        catch (Exception ex) { Status("写入失败：" + ex.Message); }
    }

    private static string FormatHexDump(long baseAddr, byte[] data)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < data.Length; i += 16)
        {
            sb.Append((baseAddr + i).ToString("X8") + ": ");
            int n = Math.Min(16, data.Length - i);
            for (int j = 0; j < n; j++) sb.Append(data[i + j].ToString("X2") + " ");
            sb.AppendLine();
        }
        return sb.ToString();
    }

    private static List<byte> ParseHexDump(string text)
    {
        var bytes = new List<byte>();
        foreach (var line in text.Split('\n', '\r'))
        {
            var t = line.Trim();
            if (t.Length == 0) continue;
            int colon = t.IndexOf(':');
            if (colon >= 0) t = t[(colon + 1)..].Trim();
            foreach (var tok in t.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries))
            {
                if (tok.Length == 2 && byte.TryParse(tok, System.Globalization.NumberStyles.HexNumber, null, out var b))
                    bytes.Add(b);
            }
        }
        return bytes;
    }

    // ---------- 偏移配置 ----------
    private void BuildConfigPanel()
    {
        CfgPanel.Children.Clear();
        _cfgBoxes.Clear();
        void AddRow(string label, string value, string key)
        {
            var sp = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(2) };
            sp.Children.Add(new TextBlock { Text = label, Width = 260, VerticalAlignment = VerticalAlignment.Center });
            var tb = new TextBox { Width = 240, Text = value };
            sp.Children.Add(tb);
            CfgPanel.Children.Add(sp);
            _cfgBoxes[key] = tb;
        }
        AddRow("锚点地址 (AnchorAddress)", _cfg.AnchorAddress, "AnchorAddress");
        AddRow("物品基址调整 (ItemBaseAdjust, -0xF0=-240)", _cfg.ItemBaseAdjust.ToString(), "ItemBaseAdjust");
        AddRow("进程访问权限 (ProcessAccess)", _cfg.ProcessAccess, "ProcessAccess");
        AddRow("属性数组起点 (BaseOffsetFromItem)", _cfg.PropertyList.BaseOffsetFromItem.ToString(), "BaseOffsetFromItem");
        AddRow("属性数量 (Count)", _cfg.PropertyList.Count.ToString(), "Count");
        AddRow("属性槽步长 (Stride)", _cfg.PropertyList.Stride.ToString(), "Stride");
        AddRow("Index 偏移", _cfg.PropertyList.IndexOffset.ToString(), "IndexOffset");
        AddRow("Num 偏移", _cfg.PropertyList.NumOffset.ToString(), "NumOffset");
        AddRow("Color 偏移", _cfg.PropertyList.ColorOffset.ToString(), "ColorOffset");
        AddRow("Precent 偏移", _cfg.PropertyList.PrecentOffset.ToString(), "PrecentOffset");
        AddRow("读取时扫描数组 (ScanArray, true/false)", _cfg.PropertyList.ScanArray.ToString(), "ScanArray");

        var btnSave = new Button { Content = "保存配置并应用", Margin = new Thickness(2, 12, 2, 2), Padding = new Thickness(12, 6, 12, 6) };
        btnSave.Click += BtnSaveCfg_Click;
        CfgPanel.Children.Add(btnSave);

        var note = new TextBlock
        {
            Text = "说明：以上偏移来自对原版 poq_hack.exe 的逆向，默认值可能需要针对实际 POQ.EXE v1.30 校准。\n"
                 + "可用“内存查看”页读取物品内存来核对。修改后点击“保存配置并应用”即写入 offsets.json 并即时生效。",
            Margin = new Thickness(2, 8, 2, 2), Foreground = System.Windows.Media.Brushes.Gray, TextWrapping = TextWrapping.Wrap
        };
        CfgPanel.Children.Add(note);
    }

    private void BtnSaveCfg_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            _cfg.AnchorAddress = _cfgBoxes["AnchorAddress"].Text;
            _cfg.ItemBaseAdjust = int.Parse(_cfgBoxes["ItemBaseAdjust"].Text);
            _cfg.ProcessAccess = _cfgBoxes["ProcessAccess"].Text;
            _cfg.PropertyList.BaseOffsetFromItem = int.Parse(_cfgBoxes["BaseOffsetFromItem"].Text);
            _cfg.PropertyList.Count = int.Parse(_cfgBoxes["Count"].Text);
            _cfg.PropertyList.Stride = int.Parse(_cfgBoxes["Stride"].Text);
            _cfg.PropertyList.IndexOffset = int.Parse(_cfgBoxes["IndexOffset"].Text);
            _cfg.PropertyList.NumOffset = int.Parse(_cfgBoxes["NumOffset"].Text);
            _cfg.PropertyList.ColorOffset = int.Parse(_cfgBoxes["ColorOffset"].Text);
            _cfg.PropertyList.PrecentOffset = int.Parse(_cfgBoxes["PrecentOffset"].Text);
            _cfg.PropertyList.ScanArray = bool.Parse(_cfgBoxes["ScanArray"].Text);
            _cfg.Save(CfgPath);
            ReloadConfig();
            Status("配置已保存并应用。");
        }
        catch (Exception ex) { Status("保存配置失败：" + ex.Message); }
    }

    private void Status(string msg) => StatusText.Text = msg;
}
