#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace poq {

/// 全装备物品目录(无中生有数据源)。由 tools/build_gam_catalog.py 生成。
/// 两级: 类别(档名) -> 物品(别名=物品名), 类别内按等级升序、同级按拼音升序。
class GamCatalog {
public:
    struct Entry {
        std::string name;            // 物品名(拼音别名)
        uint32_t level = 0;          // 等级
        std::vector<uint8_t> rec;    // 完整记录字节 [len][content][cnt][echo]
    };
    std::vector<std::string> cats;                   // 类别(排序)
    std::vector<std::vector<Entry>> items;           // items[cat_idx]

    /// 从候选路径加载; 返回是否成功。
    static bool Load(const std::vector<std::string>& candidate_paths, GamCatalog& out);
    bool LoadFile(const std::string& path);
};

/// 场景全名目录(由 tools/build_scene_catalog.py 从世界档场景表生成)。
/// 把容器别名(拼音)匹配成完整场景路径显示, 如 "hd_wuqipu" -> "秦殇/邯郸/ROOM/武器铺"。
class SceneCatalog {
public:
    struct Scene {
        std::string path;      // UTF-8 全路径
        std::string region_py; // 区域拼音
        std::string room_py;   // 房间拼音
    };
    std::vector<Scene> scenes;

    /// 从候选路径加载一个可用的; 返回成功。
    static bool Load(const std::vector<std::string>& candidates, SceneCatalog& out);
    bool LoadFile(const std::string& path);
    /// 容器别名 -> 匹配的场景全路径; 无匹配返回空。
    std::string MatchPath(const std::string& alias) const;
};

/// 物品中文名映射(由 tools/build_item_names.py 从游戏 Data/item_XX.txt 生成)。
/// 别名(拼音)->中文名; 用于 无中生有 目录与存档物品树显示中文。
class ItemNames {
public:
    std::map<std::string, std::string> map;  // alias -> 中文(UTF-8)
    static bool Load(const std::vector<std::string>& candidates, ItemNames& out);
    bool LoadFile(const std::string& path);
    /// 别名查中文; 无则返回空串。
    std::string Cn(const std::string& alias) const;
};

}  // namespace poq