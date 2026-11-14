// AffixRt —— GAM 词条数组往返保真度离线回归工具(不进主 exe)。
//
// 用途: 改 GamFile / GamFileAffix 等词条相关代码后, 一键离线验证"重建词条数组"
//       与"原数组"是否一致, 防止回归。指标口径与 docs/260919_optimization_plan.md 一致。
//
// 用法:
//   AffixRt.exe <文件或目录> [...]        目录会递归收集 *.gam
//   AffixRt.exe --verbose <路径...>      额外打印每件物品的明细行
//
// 输出(汇总行):
//   语料 520 件:  完全一致 454  仅系数不同 0  结构不同 66  重建不自洽 0  长度字段错 0
//
// 说明: 仓库不含 .gam 语料, 由维护者本地提供路径; 基线数字见 docs/affix_regression.md。

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "Core/GamFile.h"

namespace {

struct Key {
  int id = 0;
  long value = 0;
  unsigned color = 0;
  unsigned active = 0;
  long a = 0;  // 系数(放大 1000 取整比较)
  long c = 0;
  bool operator<(const Key& o) const {
    if (id != o.id) return id < o.id;
    if (value != o.value) return value < o.value;
    if (color != o.color) return color < o.color;
    if (active != o.active) return active < o.active;
    if (a != o.a) return a < o.a;
    return c < o.c;
  }
  bool softEqual(const Key& o) const {  // 只比 id/值/颜色/激活
    return id == o.id && value == o.value && color == o.color && active == o.active;
  }
};

std::vector<Key> keys(const std::vector<poq::GamItem::AffixNode>& v) {
  std::vector<Key> r;
  for (const auto& n : v) {
    Key k;
    k.id = n.id;
    k.value = static_cast<long>(n.value < 0 ? n.value - 0.5f : n.value + 0.5f);
    k.color = n.color;
    k.active = n.active;
    k.a = static_cast<long>(n.a * 1000);
    k.c = static_cast<long>(n.c * 1000);
    r.push_back(k);
  }
  std::sort(r.begin(), r.end());
  return r;
}

// 递归收集目录下的 *.gam
void collect_gams(const std::string& dir, std::vector<std::string>& out) {
  WIN32_FIND_DATAA fd;
  HANDLE h = ::FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    std::string name = fd.cFileName;
    if (name == "." || name == "..") continue;
    std::string full = dir + "\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      collect_gams(full, out);
    } else {
      size_t dot = name.find_last_of('.');
      if (dot != std::string::npos) {
        std::string ext = name.substr(dot);
        for (char& ch : ext) ch = static_cast<char>(::tolower(ch));
        if (ext == ".gam") out.push_back(full);
      }
    }
  } while (::FindNextFileA(h, &fd));
  ::FindClose(h);
}

bool is_dir(const std::string& p) {
  DWORD attr = ::GetFileAttributesA(p.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

struct Tally {
  int items = 0;
  int noarr = 0;
  int same = 0;
  int soft = 0;
  int hard = 0;
  int cntBad = 0;
  int lenBad = 0;
  int loadFail = 0;
};

struct Baseline {
  int items = -1;
  int same = -1;
  int soft = -1;
  int hard = -1;
  int cntBad = -1;
  int lenBad = -1;
};

static bool parse_baseline_key(const std::string& line, const char* key, int& out) {
  const size_t klen = std::strlen(key);
  if (line.size() <= klen || line.compare(0, klen, key) != 0 || line[klen] != '=') return false;
  char* end = nullptr;
  long v = std::strtol(line.c_str() + klen + 1, &end, 10);
  if (end == line.c_str() + klen + 1) return false;
  out = static_cast<int>(v);
  return true;
}

static bool load_baseline(const std::string& path, Baseline& b) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    parse_baseline_key(line, "语料件数", b.items);
    parse_baseline_key(line, "完全一致", b.same);
    parse_baseline_key(line, "仅系数不同", b.soft);
    parse_baseline_key(line, "结构不同", b.hard);
    parse_baseline_key(line, "重建不自洽", b.cntBad);
    parse_baseline_key(line, "长度字段错", b.lenBad);
  }
  return b.same >= 0 && b.soft >= 0 && b.cntBad >= 0 && b.lenBad >= 0;
}

static int check_baseline(const Tally& t, const Baseline& b) {
  if (t.loadFail > 0) {
    std::printf("基线校验失败: 有 %d 个 .gam 加载失败\n", t.loadFail);
    return 2;
  }
  if (b.items >= 0 && t.items != b.items) {
    std::printf("基线校验失败: 语料件数 %d, 期望 %d (语料路径是否完整?)\n", t.items, b.items);
    return 2;
  }
  if (t.cntBad != b.cntBad) {
    std::printf("基线校验失败: 重建不自洽 %d, 期望 %d\n", t.cntBad, b.cntBad);
    return 2;
  }
  if (t.lenBad != b.lenBad) {
    std::printf("基线校验失败: 长度字段错 %d, 期望 %d\n", t.lenBad, b.lenBad);
    return 2;
  }
  if (t.soft != b.soft) {
    std::printf("基线校验失败: 仅系数不同 %d, 期望 %d\n", t.soft, b.soft);
    return 2;
  }
  if (t.same != b.same) {
    std::printf("基线校验失败: 完全一致 %d, 期望 %d\n", t.same, b.same);
    return 2;
  }
  std::printf("基线校验通过 (完全一致 %d, 结构不同 %d 为历史口径, 见 docs/affix_regression.md)\n",
              t.same, t.hard);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool verbose = false;
  bool checkBaseline = false;
  bool loadOnly = false;
  std::string baselinePath;
  std::vector<std::string> paths;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--verbose" || a == "-v") {
      verbose = true;
      continue;
    }
    if (a == "--load-only") {
      loadOnly = true;
      continue;
    }
    if (a == "--check-baseline") {
      checkBaseline = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') baselinePath = argv[++i];
      continue;
    }
    paths.push_back(a);
  }
  if (paths.empty()) {
    std::printf(
        "AffixRt —— 词条数组往返回归工具\n"
        "用法: AffixRt.exe <文件或目录> [...]   (目录递归收集 *.gam)\n"
        "      AffixRt.exe --verbose <路径...>  (额外打印每件物品明细)\n"
        "      AffixRt.exe --check-baseline [基线.txt] <路径...>  (对照 "
        "data/py/affix_rt_baseline.txt)\n"
        "      AffixRt.exe --load-only <路径...>  (仅解析 .gam, 不做词条比对)\n"
        "仓库不含语料, 请提供本地 .gam 路径; 指标口径见 docs/affix_regression.md\n");
    return 1;
  }
  if (checkBaseline && baselinePath.empty()) {
    std::printf("--check-baseline 需要基线文件路径 (例如 ..\\data\\py\\affix_rt_baseline.txt)\n");
    return 1;
  }
  Baseline baseline{};
  if (checkBaseline && !load_baseline(baselinePath, baseline)) {
    std::printf("无法读取基线文件: %s\n", baselinePath.c_str());
    return 1;
  }

  std::vector<std::string> gams;
  for (const std::string& p : paths) {
    if (is_dir(p)) {
      collect_gams(p, gams);
    } else {
      gams.push_back(p);
    }
  }
  if (gams.empty()) {
    std::printf("没找到 .gam 文件(检查路径)\n");
    return 1;
  }

  Tally t;
  std::vector<std::string> samples;      // 结构不同样例
  std::vector<std::string> coefSamples;  // 仅系数不同样例
  for (const std::string& g : gams) {
    poq::GamFile gf;
    std::string err;
    if (!poq::GamFile::load(g, gf, err)) {
      std::printf("load fail %s: %s\n", g.c_str(), err.c_str());
      ++t.loadFail;
      continue;
    }
    if (loadOnly) {
      t.items += static_cast<int>(gf.items.size());
      continue;
    }
    for (auto& it : gf.items) {
      ++t.items;
      if (!it.has_affixes) {
        ++t.noarr;
        continue;
      }
      auto k0 = keys(it.affixes);
      it.sync_affixes_from_g_info();
      auto k1 = keys(it.affixes);
      // 自洽性: 重建后再解析一遍, 节点数应一致
      poq::GamItem tmp;
      tmp.content = it.content;
      tmp.parse_affixes();
      if (!tmp.has_affixes || tmp.affixes.size() != it.affixes.size()) {
        ++t.cntBad;
      }
      if (poq::GamFile::dw_at(it.content, it.content.size() - 4) !=
          static_cast<uint32_t>(it.content.size())) {
        ++t.lenBad;
      }
      if (k0.size() == k1.size()) {
        bool eq = true, se = true;
        for (size_t i = 0; i < k0.size(); ++i) {
          if (!k0[i].softEqual(k1[i])) se = false;
          if (!(k0[i] < k1[i]) && !(k1[i] < k0[i])) continue;
          eq = false;
        }
        if (eq) {
          ++t.same;
        } else if (se) {
          ++t.soft;
          if (coefSamples.size() < 6) {
            char b[512];
            std::snprintf(b, sizeof(b), "%s %s", gf.path.c_str(), it.alias.c_str());
            std::string s = b;
            for (size_t i = 0; i < k0.size(); ++i) {
              if (k0[i].a == k1[i].a && k0[i].c == k1[i].c) continue;
              std::snprintf(b, sizeof(b), "\n    id%02X v%ld c%u a%u: a/c %ld/%ld -> %ld/%ld",
                            k0[i].id, k0[i].value, k0[i].color, k0[i].active, k0[i].a, k0[i].c,
                            k1[i].a, k1[i].c);
              s += b;
            }
            coefSamples.push_back(s);
          }
        } else {
          ++t.hard;
        }
        if (!eq && !se && samples.size() < 6) {
          char b[256];
          std::snprintf(b, sizeof(b), "%s %s 原%d 新%d", gf.path.c_str(), it.alias.c_str(),
                        static_cast<int>(k0.size()), static_cast<int>(k1.size()));
          samples.push_back(b);
        }
      } else {
        ++t.hard;
        if (samples.size() < 6) {
          char b[256];
          std::snprintf(b, sizeof(b), "%s %s 节点数 原%d 新%d", gf.path.c_str(), it.alias.c_str(),
                        static_cast<int>(k0.size()), static_cast<int>(k1.size()));
          samples.push_back(b);
        }
      }
      if (verbose) {
        std::printf("%s %-24s 节点 %zu -> %zu\n", gf.path.c_str(), it.alias.c_str(), k0.size(),
                    k1.size());
      }
    }
  }

  if (loadOnly) {
    std::printf("load-only: %d 个 .gam, %d 件物品, 加载失败 %d\n",
                static_cast<int>(gams.size()), t.items, t.loadFail);
    return t.loadFail ? 1 : 0;
  }
  std::printf(
      "语料 %d 件:  完全一致 %d  仅系数不同 %d  结构不同 %d  重建不自洽 %d  长度字段错 %d\n",
      t.items, t.same, t.soft, t.hard, t.cntBad, t.lenBad);
  if (t.noarr) std::printf("(其中 %d 件无词条数组, 不参与比对)\n", t.noarr);
  if (t.loadFail) std::printf("(加载失败 %d 个文件)\n", t.loadFail);
  if (!coefSamples.empty()) {
    std::printf("--- 仅系数不同的样例 ---\n");
    for (const auto& s : coefSamples) std::printf("%s\n", s.c_str());
  }
  if (!samples.empty()) {
    std::printf("--- 结构不同的样例 ---\n");
    for (const auto& s : samples) std::printf("%s\n", s.c_str());
  }
  if (checkBaseline) return check_baseline(t, baseline);
  return 0;
}
