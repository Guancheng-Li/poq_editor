#include "GamCatalog.h"

#include <cstdio>
#include <cstring>

namespace poq {

static uint16_t Rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t Rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool GamCatalog::LoadFile(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 16) {
        fclose(fp);
        return false;
    }
    std::vector<uint8_t> d((size_t)sz);
    if (fread(d.data(), 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    if (memcmp(d.data(), "GAMCAT1", 8) != 0) return false;
    size_t pos = 8;
    uint32_t count = Rd32(&d[pos]);
    pos += 4;
    cats.clear();
    items.clear();
    for (uint32_t i = 0; i < count && pos + 8 <= d.size(); ++i) {
        uint16_t clen = Rd16(&d[pos]);
        pos += 2;
        if (pos + clen + 2 > d.size()) return false;
        std::string cat((const char*)&d[pos], clen);
        pos += clen;
        uint16_t nlen = Rd16(&d[pos]);
        pos += 2;
        if (pos + nlen + 8 > d.size()) return false;
        std::string name((const char*)&d[pos], nlen);
        pos += nlen;
        uint32_t level = Rd32(&d[pos]);
        pos += 4;
        uint32_t rlen = Rd32(&d[pos]);
        pos += 4;
        if (pos + rlen > d.size()) return false;
        Entry e;
        e.name = name;
        e.level = level;
        e.rec.assign(&d[pos], &d[pos] + rlen);
        pos += rlen;
        int ci = -1;
        for (size_t k = 0; k < cats.size(); ++k)
            if (cats[k] == cat) {
                ci = (int)k;
                break;
            }
        if (ci < 0) {
            cats.push_back(cat);
            items.push_back({});
            ci = (int)cats.size() - 1;
        }
        items[(size_t)ci].push_back(std::move(e));
    }
    return !cats.empty();
}

bool GamCatalog::Load(const std::vector<std::string>& candidates, GamCatalog& out) {
    for (const auto& p : candidates) {
        if (out.LoadFile(p)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// SceneCatalog
// ---------------------------------------------------------------------------
bool SceneCatalog::LoadFile(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 16) {
        fclose(fp);
        return false;
    }
    std::vector<uint8_t> d((size_t)sz);
    if (fread(d.data(), 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    if (memcmp(d.data(), "SCNCAT1\0", 8) != 0) return false;
    size_t pos = 8;
    uint32_t count = Rd32(&d[pos]);
    pos += 4;
    scenes.clear();
    for (uint32_t i = 0; i < count && pos + 6 <= d.size(); ++i) {
        uint16_t pl = Rd16(&d[pos]);
        pos += 2;
        if (pos + pl + 4 > d.size()) return false;
        std::string p((const char*)&d[pos], pl);
        pos += pl;
        uint16_t rl = Rd16(&d[pos]);
        pos += 2;
        if (pos + rl + 2 > d.size()) return false;
        std::string r((const char*)&d[pos], rl);
        pos += rl;
        uint16_t ml = Rd16(&d[pos]);
        pos += 2;
        if (pos + ml > d.size()) return false;
        std::string m((const char*)&d[pos], ml);
        pos += ml;
        scenes.push_back({std::move(p), std::move(r), std::move(m)});
    }
    return !scenes.empty();
}

bool SceneCatalog::Load(const std::vector<std::string>& candidates, SceneCatalog& out) {
    for (const auto& p : candidates) {
        if (out.LoadFile(p)) return true;
    }
    return false;
}

namespace {
bool StartsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool EndsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
}  // namespace

std::string SceneCatalog::MatchPath(const std::string& alias) const {
    std::string best;
    int best_score = 0;
    for (const auto& s : scenes) {
        const std::string& r = s.region_py;
        const std::string& m = s.room_py;
        if (m.empty() && r.empty()) continue;
        int sc = 0;
        if (!m.empty() && alias == m)
            sc = 100;
        else if (!r.empty() && alias == r)
            sc = 90;
        else if (!m.empty() && !r.empty() && (alias == r + m || alias == m + r))
            sc = 85;
        else if (m.size() >= 2 && StartsWith(alias, m))
            sc = 80;  // zahuopulaoban 匹配杂货铺
        else if (m.size() >= 2 && EndsWith(alias, m))
            sc = 70;  // hd_wuqipu 匹配武器铺
        else if (m.size() >= 2 && m.size() > alias.size() && StartsWith(m, alias))
            sc = 65;  // 被乱草->被乱草掩盖的山洞
        else if (r.size() >= 2 && r.size() > alias.size() && StartsWith(r, alias))
            sc = 60;  // 衡山->衡山城,三川->三川郡
        else if (r.size() >= 2 && r.size() <= alias.size() && alias.find(r) != std::string::npos)
            sc = 55;
        else if (m.size() >= 2 && m.size() <= alias.size() && alias.find(m) != std::string::npos)
            sc = 50;
        else if (r.size() >= 2 && StartsWith(alias, r))
            sc = 40;
        else if (r.size() >= 2 && EndsWith(alias, r))
            sc = 35;
        if (sc > best_score) {
            best_score = sc;
            best = s.path;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// ItemNames
// ---------------------------------------------------------------------------
bool ItemNames::LoadFile(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 16) {
        fclose(fp);
        return false;
    }
    std::vector<uint8_t> d((size_t)sz);
    if (fread(d.data(), 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    if (memcmp(d.data(), "ITEMNM1\0", 8) != 0) return false;
    size_t pos = 8;
    uint32_t count = Rd32(&d[pos]);
    pos += 4;
    map.clear();
    for (uint32_t i = 0; i < count && pos + 4 <= d.size(); ++i) {
        uint16_t al = Rd16(&d[pos]);
        pos += 2;
        if (pos + al + 2 > d.size()) return false;
        std::string a((const char*)&d[pos], al);
        pos += al;
        uint16_t cl = Rd16(&d[pos]);
        pos += 2;
        if (pos + cl > d.size()) return false;
        std::string c((const char*)&d[pos], cl);
        pos += cl;
        map[a] = c;
    }
    return !map.empty();
}

bool ItemNames::Load(const std::vector<std::string>& candidates, ItemNames& out) {
    for (const auto& p : candidates) {
        if (out.LoadFile(p)) return true;
    }
    return false;
}

std::string ItemNames::Cn(const std::string& alias) const {
    auto it = map.find(alias);
    return it == map.end() ? std::string() : it->second;
}

}  // namespace poq