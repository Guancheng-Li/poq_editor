// GmCmd —— 屏幕输出捕获(steam 扫串 / 1.30 重组 / objlist)。
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <utility>
#include "Core/GmCmdInternal.h"

namespace poq::gm_cmd::detail {

// ---------------------------------------------------------------------------
// 屏幕输出捕获(steam 连续 GBK 串)
// ---------------------------------------------------------------------------
struct StrHit {
  uintptr_t addr = 0;
  std::string text;
};

// 全内存扫描所有包含 kw(GBK 字节)的 NUL 结尾 GBK 字符串(对齐 scan_gbk_strings)。
std::vector<StrHit> scan_gbk_strings(ProcessMemory& mem, const std::string& kw) {
  std::vector<StrHit> out;
  for (const auto& r : mem.enumerate_regions()) {
    std::vector<unsigned char> data;
    try {
      data = mem.read(r.base, static_cast<int>(std::min<size_t>(r.size, 0x4000000)));
    } catch (...) {
      continue;
    }
    size_t off = 0;
    while (true) {
      size_t i = find_bytes(data, kw, off);
      if (i == std::string::npos) break;
      // 对齐 python:rfind 窗口 [max(0,i-0x2000), i),未找到 -> s=0(从 region 头起)
      size_t win = (i > 0x2000) ? (i - 0x2000) : 0;
      size_t nul = r_find_nul(data, win, i);
      size_t s = (nul == win - 1) ? 0 : nul + 1;
      size_t j = find_nul_from(data, i);
      std::string text = gbk_to_utf8(reinterpret_cast<const char*>(data.data()) + s, j - s);
      if (!text.empty()) out.push_back({r.base + s, text});
      off = i + 1;
    }
  }
  return out;
}

// look 统计的"完整"判定(对齐 _look_stats[0]):对象头含 (N) 且其后对象行数 >= N。
bool look_stats_complete(const std::string& text) {
  std::smatch m;
  if (!std::regex_search(text, m, re_look_count())) return false;
  int want = std::stoi(m[1].str());
  std::string tail = text.substr(m.position(0) + m.length(0));
  int got = 0;
  for (const auto& line : split_lines(tail)) {
    std::string s = trim(line);
    if (!s.empty() && std::regex_match(s, re_look_obj())) ++got;
  }
  return got >= want && want > 0;
}

// ---------------------------------------------------------------------------
// 1.30 屏幕输出重组(散行 -> 完整块,对齐 capture_130_text)
// ---------------------------------------------------------------------------
struct Class130 {
  std::string kind;  // scene / exhead / objhead / exit / obj / sep / junk
  std::string text;
  int count = 0;
};

Class130 classify130(const std::string& line) {
  std::string s = trim(line);
  std::smatch m;
  if (std::regex_match(s, m, re_scene())) return {"scene", s, 0};
  if (std::regex_search(s, m, re_obj_head()) && s.find("%d") == std::string::npos)
    return {"objhead", s, std::stoi(m[1].str())};
  if (s.find("这里的出口有") != std::string::npos) return {"exhead", s, 0};
  if (s.find("这里的人和物品有") != std::string::npos) return {"junk", s, 0};
  if (std::regex_match(s, m, re_exit())) return {"exit", s, 0};
  if (std::regex_match(s, m, re_obj()) && s.find("out :") == std::string::npos &&
      s.find("场景") == std::string::npos && s.find("%s") == std::string::npos &&
      s.find("%d") == std::string::npos && s.find("/ROOM") == std::string::npos)
    return {"obj", s, 0};
  if (s.find("--------") != std::string::npos) return {"sep", s, 0};
  return {"junk", s, 0};
}

// 归一化出口行:去掉尾部 " <N>" 元数据,避免同出口重复。
std::string normalize_exit(const std::string& val) {
  std::string t = trim(val);
  std::smatch m;
  if (std::regex_match(t, m, re_exit()))
    return m[1].str() + " : " + m[2].str() + "<" + m[3].str() + ">";
  return val;
}

// 所有含 kb 的 region 内全部 NUL 结尾短行(addr, text),按地址升序(对齐 _gather_130_region_lines)。
std::vector<std::pair<uintptr_t, std::string>> gather130_lines(ProcessMemory& mem,
                                                               const std::string& kb) {
  std::vector<std::pair<uintptr_t, std::string>> out;
  for (const auto& r : mem.enumerate_regions()) {
    std::vector<unsigned char> data;
    try {
      data = mem.read(r.base, static_cast<int>(std::min<size_t>(r.size, 0x4000000)));
    } catch (...) {
      continue;
    }
    if (find_bytes(data, kb, 0) == std::string::npos) continue;
    size_t i = 0;
    while (i < data.size()) {
      size_t j = find_nul_from(data, i);
      size_t seg_len = j - i;
      if (seg_len >= 3) {
        std::string t = gbk_to_utf8(reinterpret_cast<const char*>(data.data()) + i, seg_len);
        if (t.size() >= 2 && t.size() <= 150) out.push_back({r.base + i, t});
      }
      i = j + 1;
    }
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return out;
}

std::optional<std::string> capture130_text(Ctx& ctx, bool want_objs, double timeout_sec) {
  std::string mark = want_objs ? "这里的人和物品有" : "这里的出口有";
  std::string kb = utf8_to_gbk(mark);
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(static_cast<int>(timeout_sec * 1000.0));
  while (std::chrono::steady_clock::now() < deadline) {
    auto lines = gather130_lines(ctx.mem, kb);
    const char* head_kind = want_objs ? "objhead" : "exhead";
    std::vector<std::pair<uintptr_t, std::string>> heads;
    for (const auto& [a, t] : lines)
      if (classify130(t).kind == head_kind) heads.push_back({a, t});
    if (heads.empty()) {
      ::Sleep(300);
      continue;
    }
    // 地址最大的 head = 最近一次写入的块
    uintptr_t head_addr = heads.back().first;
    std::string scene;
    bool scene_set = false;
    std::vector<std::string> exits, objs;
    std::set<std::string> seen_e, seen_o;
    int objcount = 0;
    bool have_count = false;
    for (const auto& [a, t] : lines) {
      if (std::llabs(static_cast<long long>(a) - static_cast<long long>(head_addr)) >= 0x8000)
        continue;
      Class130 c = classify130(t);
      if (c.kind == "scene") {
        if (!scene_set) {
          scene = c.text;
          scene_set = true;
        }
      } else if (c.kind == "objhead") {
        if (!have_count) {
          objcount = c.count;
          have_count = true;
        }
      } else if (c.kind == "exit") {
        std::string core = normalize_exit(c.text);
        if (seen_e.insert(core).second) exits.push_back(core);
      } else if (c.kind == "obj") {
        if (seen_o.insert(c.text).second) objs.push_back(c.text);
      }
    }
    if (want_objs) {
      if (scene_set && have_count && static_cast<int>(objs.size()) >= objcount) {
        std::string out = scene + "\n--------\n这里的出口有: ";
        for (const auto& e : exits) out += "\n" + e;
        out += "\n这里的人和物品有: (" + std::to_string(objcount) + ")";
        for (int i = 0; i < objcount && i < static_cast<int>(objs.size()); ++i)
          out += "\n" + objs[static_cast<size_t>(i)];
        return out;
      }
    } else {
      if (scene_set && !exits.empty()) {
        std::string out = scene + "\n--------\n这里的出口有: ";
        for (const auto& e : exits) out += "\n" + e;
        return out;
      }
    }
    ::Sleep(300);
  }
  return std::nullopt;
}

// objlist:直调后全内存扫条目行,按 ID 排序输出(对齐 gm_tab 的 build_objdb 用法)。
std::string capture_obj_list(Ctx& ctx) {
  silent_prepare(ctx);
  DispatchResult res = send_command(ctx, "objlist", "");
  if (res.value != 1) return "";
  ::Sleep(800);
  struct ObjEntry {
    std::string name, alias, path;
    int64_t id = 0;
  };
  std::map<int64_t, ObjEntry> entries;
  for (const auto& hit : scan_gbk_strings(ctx.mem, kObjIdPrefix)) {
    auto begin = std::sregex_iterator(hit.text.begin(), hit.text.end(), re_obj_entry());
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
      const std::smatch& m = *it;
      std::string name = trim(m[1].str());
      std::string alias = trim(m[2].str());
      std::string path = trim(m[3].str());
      int64_t oid = std::stoll(m[4].str());
      if (name.empty() || alias.empty()) continue;
      entries[oid] = {name, alias, path, oid};
    }
  }
  if (entries.empty()) return "";
  std::string out = "物体个数 : " + std::to_string(entries.size());
  for (const auto& kv : entries)
    out += "\n\t" + kv.second.name + "(" + kv.second.alias + ") : " + kv.second.path + "(" +
           std::to_string(kv.second.id) + ")";
  return out;
}

// 通用捕获路径(对齐 capture_cmd_text,steam 扫连续串)。
std::string capture_steam_text(Ctx& ctx, bool is_look) {
  std::string kw = is_look ? kMarkObjs : kMarkExits;
  std::string kw_gbk = utf8_to_gbk(kw);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
  std::string best;
  while (std::chrono::steady_clock::now() < deadline) {
    for (const auto& hit : scan_gbk_strings(ctx.mem, kw_gbk)) {
      if (is_look) {
        if (hit.text.size() > best.size()) best = hit.text;
        if (look_stats_complete(hit.text)) return hit.text;
      } else {
        return hit.text;  // dir 无校验:取第一个命中
      }
    }
    ::Sleep(300);
  }
  return best;  // look 兜底:最长命中(可能为空)
}
}  // namespace poq::gm_cmd::detail
