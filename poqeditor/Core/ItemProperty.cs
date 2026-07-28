using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace PoqEditor.Core;

/// <summary>一条属性修改项。Address 为该属性在游戏内存中的写入地址（可手填或由读取扫描得到）。</summary>
public class ItemProperty : INotifyPropertyChanged
{
    private string _address = "";
    private int _index;
    private int _num;
    private int _color;
    private int _precent;

    public string Address
    {
        get => _address;
        set { _address = value; OnPropertyChanged(); }
    }
    public int Index
    {
        get => _index;
        set { _index = value; OnPropertyChanged(); }
    }
    public int Num
    {
        get => _num;
        set { _num = value; OnPropertyChanged(); }
    }
    public int Color
    {
        get => _color;
        set { _color = value; OnPropertyChanged(); }
    }
    public int Precent
    {
        get => _precent;
        set { _precent = value; OnPropertyChanged(); }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? n = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
}
