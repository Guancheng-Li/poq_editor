#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace poq {

/// GAM 存档物品记录(对应 python gamkit.Gam._parse_leaf)
/// 文件帧: [4B len][内容][4B count=0][4B total=len+12]，内容=0x59+i 整块。
/// 固定字段: 相对 别名尾(aend) 的偏移(内容内 dword ^0xAAABACAD, 空=-1)。
struct GamItem {
    size_t pos = 0;              // 记录 [len] 的文件偏移
    uint32_t len = 0;            // 内容长度
    std::vector<uint8_t> content;  // 解码后内容(内部字段仍 ^KEY 编码)
    size_t aend = 0;             // 别名尾偏移
    size_t rec_end = 0;          // 记录尾(=pos+4+len+8)
    std::string alias;           // 拼音别名
    bool dirty = false;          // 已编辑(保存时重写)
    bool is_new = false;         // 新增记录(保存时插入目标容器)

    int32_t GetField(const char* name, bool* ok) const;
    bool SetField(const char* name, int64_t v);  // 就地改(同长度)
    std::string GetGInfo() const;                // 词条 GBK 文本
    bool SetGInfo(const std::string& gbktext);   // 原位改词条文本(仅当编码长度≤当前占用)
    /// 解析 G_Info 词条为结构化行(颜色组/属性名GBK/值/上限/百分比).
    struct GInfoRow {
        char color = 0;        // 'K'蓝 'C'亮蓝(激活隐藏) 'Z'金 'H'暗蓝(未激活隐藏), 0=白
        std::string name;      // 属性名(GBK 字节)
        long vmin = 0;
        long vmax = 0;
        bool has_max = false;
        bool is_percent = false;  // 值后带 \x0f(百分比标记)
    };
    /// 返回结构化词条列表; 失败(无G_Info/解析不动)返回 false。
    bool ParseGInfoRows(std::vector<GInfoRow>& rows) const;
    /// G_Info 头行("&模板编号    Lv. N" / "Lv. N"), 无则空(含尾随换行)。
    std::string GetGInfoHeader() const;
    /// 装配: 头行 + 词条行 -> G_Info 文本(GBK)。颜色组按连续同色分组 [X..]。
    static bool BuildGInfoText(const std::string& header,
                               const std::vector<GInfoRow>& rows, std::string& out);
    /// 把某行词条的值改写回 G_Info(原位, 仅当新值等长, 否则失败)。
    /// idx 对应 ParseGInfoRows 的顺序; 按 "名 值[ ~ 值]" 行原位重写。
    bool SetGInfoRowValue(size_t idx, long vmin, long vmax);
    /// 从记录字节解析(用于粘贴/创建的临时物品)。
    static bool ParseRecord(const std::vector<uint8_t>& rec, GamItem& out);
    /// 由内容重编出整条记录字节 [len][content][0][len+12]。
    static bool BuildRecord(const GamItem& it, std::vector<uint8_t>& out);
};

/// 容器(场景/物体组): [len][string含场景名][count][子树...]
struct GamContainer {
    size_t start = 0;
    size_t end = 0;              // 下一容器起点(自身体 != end)
    std::string alias;
    std::vector<int> item_idx;   // 归属本容器的物品下标(在 file.items 中)
    // 插入锚点: 直接拥有最后一件物品的组的 count 字段偏移 + 插入位置(该物品记录尾)
    size_t insert_cnt_off = 0;
    size_t insert_pos = 0;
    bool first = false;          // 是否当前(首个/数据区开头)容器
    bool last = false;           // 是否末尾容器
};

class GamFile {
public:
    std::string path;
    std::vector<uint8_t> buf;
    uint32_t rec = 0;
    size_t dataoff = 0;
    uint32_t cont_len = 0;       // 头 dword[2] = 首容器长度
    std::vector<GamContainer> containers;
    std::vector<GamItem> items;

    /// 解析 .gam。失败返回 false(附带 err)。
    static bool Load(const std::string& path, GamFile& out, std::string& err);
    /// 保存: applied 已修改记录 + 新增记录插入到目标容器, 重算长度字段。
    /// added: {容器下标, 记录字节}; 就地编辑在 items 中。
    static bool Save(GamFile& f, const std::string& out_path,
                     const std::vector<std::pair<int, std::vector<uint8_t>>>& added,
                     std::string& err);

    /// 静态字节工具(供 UI 直接算值)
    static uint32_t DwAt(const std::vector<uint8_t>& c, size_t off);   // ^KEY
    static bool DecStr(const std::vector<uint8_t>& c, size_t off, std::string& out, size_t& used);
    static uint32_t X59Dw(const std::vector<uint8_t>& b, size_t pos, uint8_t k0 = 0x59);

    /// 可编辑固定字段列表 (name, 中文标签) — 供界面渲染。
    struct FieldDesc { const char* name; const char* label; };
    static const std::vector<FieldDesc>& FieldList();

private:
    void ParseBody();
};

}  // namespace poq