#include "drop_finder_model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <set>

#include "json_lite.hpp"

namespace esrwiki::drop {

namespace {

bool ReadStrings(const json::Value& v, std::vector<std::string>& out) {
    if (v.kind != json::Value::Kind::Array) return false;
    out.clear();
    for (const json::Value& e : v.items) {
        if (e.kind != json::Value::Kind::String) return false;
        out.push_back(e.text);
    }
    return true;
}

std::string Replace(std::string text, const std::string& what, const std::string& with) {
    const std::size_t at = text.find(what);
    if (at != std::string::npos) text.replace(at, what.size(), with);
    return text;
}

std::string Lookup(const std::map<std::string, std::string>& strings, const std::string& name, const char* fallback) {
    const auto found = strings.find(name);
    return found == strings.end() ? std::string(fallback) : found->second;
}

std::string At(const std::vector<std::string>& list, int index) {
    return index >= 0 && static_cast<std::size_t>(index) < list.size() ? list[static_cast<std::size_t>(index)] : "";
}

}  // namespace

bool LoadNames(const std::string& text, Names& out, std::string& error) {
    json::Value root;
    if (!json::Parse(text, root, error)) return false;
    Names n;
    if (!ReadStrings(root.Field("items"), n.items) || !ReadStrings(root.Field("uniques"), n.uniques) ||
        !ReadStrings(root.Field("sets"), n.sets) || !ReadStrings(root.Field("mons"), n.mons) ||
        !ReadStrings(root.Field("supers"), n.supers) || !ReadStrings(root.Field("zones"), n.zones) ||
        !ReadStrings(root.Field("types"), n.types) || !ReadStrings(root.Field("acts"), n.acts)) {
        error = "df-names.json: a name table is malformed";
        return false;
    }
    out = std::move(n);
    return true;
}

bool LoadStrings(const std::string& text, std::map<std::string, std::string>& out, std::string& error) {
    json::Value root;
    if (!json::Parse(text, root, error)) return false;
    if (root.kind != json::Value::Kind::Object) {
        error = "df-ui.json is not an object";
        return false;
    }
    std::map<std::string, std::string> s;
    for (std::size_t i = 0; i < root.keys.size(); ++i) {
        if (root.values[i].kind == json::Value::Kind::String) s[root.keys[i]] = root.values[i].text;
    }
    out = std::move(s);
    return true;
}

std::vector<MonsterEntry> MonsterList(const Data& data, const Names& names) {
    std::vector<MonsterEntry> out;
    for (std::size_t i = 0; i < data.mons.size(); ++i) {
        if (!data.mons[i].listed) continue;
        out.push_back({false, static_cast<int>(i), At(names.mons, static_cast<int>(i))});
    }
    for (std::size_t i = 0; i < data.supers.size(); ++i) {
        out.push_back({true, static_cast<int>(i), At(names.supers, static_cast<int>(i))});
    }
    return out;
}

std::vector<MonsterEntry> EveryMonster(const Data& data, const Names& names) {
    std::vector<MonsterEntry> out;
    for (std::size_t i = 0; i < data.mons.size(); ++i) {
        out.push_back({false, static_cast<int>(i), At(names.mons, static_cast<int>(i))});
    }
    for (std::size_t i = 0; i < data.supers.size(); ++i) {
        out.push_back({true, static_cast<int>(i), At(names.supers, static_cast<int>(i))});
    }
    return out;
}

std::string KeyName(const Names& names, const std::map<std::string, std::string>& strings, const std::string& key) {
    if (key.empty()) return "";
    const char kind = key[0];
    const int n = key.size() > 1 ? std::stoi(key.substr(1)) : 0;  // "g" (gold) has no number
    switch (kind) {
        case 'g': return Lookup(strings, "gold", "Gold");
        case 'i':
        case 'q': return At(names.items, n);
        case 'x': return Replace(Lookup(strings, "qUniqueBase", "Unique %s"), "%s", At(names.items, n));
        case 'u': return At(names.uniques, n);
        case 's': return At(names.sets, n);
        case 'r': return Replace(Lookup(strings, "qRare", "Rare %s"), "%s", At(names.types, n));
        case 'm': return Replace(Lookup(strings, "qMagic", "Magic %s"), "%s", At(names.types, n));
        case 'h': return Replace(Lookup(strings, "qSuperior", "Superior %s"), "%s", At(names.types, n));
        case 'n': return Replace(Lookup(strings, "qNormal", "Normal %s"), "%s", At(names.types, n));
        case 'l': return Replace(Lookup(strings, "qLow", "Low Quality %s"), "%s", At(names.types, n));
        default: return key;
    }
}

namespace {

// The engine's selection for a monster entry under a setup (superuniques ignore the zone, as the page does).
Selection SelectionFor(const MonsterEntry& m, const Setup& setup) {
    Selection sel;
    sel.superunique = m.superunique;
    sel.i = m.index;
    sel.d = setup.diff;
    sel.t = setup.type;
    sel.x = setup.terror;
    sel.z = m.superunique ? -1 : setup.zone;
    return sel;
}

}  // namespace

std::vector<DropRow> DropsOf(Engine& engine, const Names& names, const MonsterEntry& monster, const Settings& settings,
                             const Setup& setup) {
    engine.SetSettings(settings);
    const Resolved r = engine.Resolve(SelectionFor(monster, setup));
    if (r.tc < 0) return {};
    const std::map<std::string, std::string> none;
    const OutMap main = engine.TopMap(r.tc, r.c, r.pe);
    const OutMap quest = r.qtc >= 0 ? engine.TopMap(r.qtc, r.c, r.pe) : OutMap{};
    std::vector<DropRow> rows;
    for (const auto& kv : main) {
        if (kv.second <= 0) continue;
        DropRow row;
        row.key = kv.first;
        row.name = KeyName(names, none, kv.first);
        row.chance = kv.second;
        const auto q = quest.find(kv.first);
        row.quest = q == quest.end() ? 0 : q->second;
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const DropRow& a, const DropRow& b) {
        return a.chance != b.chance ? a.chance > b.chance : a.name < b.name;
    });
    return rows;
}

std::vector<WhoRow> WhoDrops(Engine& engine, const Data& data, const Names& names, const Settings& settings, int terror,
                             const std::string& key) {
    engine.SetSettings(settings);
    const Bases bases = engine.BasesOf(key);
    // Every monster a zone spawns, at any difficulty: the website's inZone set.
    std::set<int> inZone;
    for (const Zone& zone : data.zones) {
        for (const auto& group : zone.groups) {
            for (const auto& list : group) inZone.insert(list.begin(), list.end());
        }
    }
    struct Hit {
        int zone;
        double chance;
        double quest;
    };
    struct Group {
        MonsterEntry monster;
        int diff = 0;
        int type = 0;
        std::vector<Hit> hits;
    };
    std::map<std::array<int, 4>, Group> groups;  // superunique, monster index, type, difficulty
    const auto consider = [&](const MonsterEntry& m, int t, int d, int z) {
        Selection sel;
        sel.superunique = m.superunique;
        sel.i = m.index;
        sel.d = d;
        sel.t = t;
        sel.x = terror;
        sel.z = z;
        const Resolved r = engine.Resolve(sel);
        const double a = engine.Top1(r.tc, r.c, r.pe, key, bases);
        const double b = engine.Top1(r.qtc, r.c, r.pe, key, bases);
        if (a <= 0 && b <= 0) return;
        Group& g = groups[{m.superunique ? 1 : 0, m.index, t, d}];
        if (g.hits.empty()) {
            g.monster = m;
            g.diff = d;
            g.type = t;
        }
        g.hits.push_back({z, a, b});
    };
    for (int d = 0; d < 3; ++d) {
        // Zone monsters: group 0 is regular, group 1 champion and unique, group 2 minion.
        for (std::size_t z = 0; z < data.zones.size(); ++z) {
            std::set<std::pair<int, int>> seen;  // each monster and type once per zone and difficulty
            for (std::size_t g = 0; g < 3; ++g) {
                const std::vector<int> types = g == 0 ? std::vector<int>{0} : g == 1 ? std::vector<int>{1, 2} : std::vector<int>{3};
                for (int m : data.zones[z].groups[g][static_cast<std::size_t>(d)]) {
                    for (int t : types) {
                        if (!seen.insert({m, t}).second) continue;
                        consider(MonsterEntry{false, m, At(names.mons, m)}, t, d, static_cast<int>(z));
                    }
                }
            }
        }
        // Listed monsters no zone spawns drop with no zone, as regular monsters.
        for (std::size_t i = 0; i < data.mons.size(); ++i) {
            const int index = static_cast<int>(i);
            if (!data.mons[i].listed || inZone.count(index) != 0) continue;
            consider(MonsterEntry{false, index, At(names.mons, index)}, 0, d, -1);
        }
        for (std::size_t s = 0; s < data.supers.size(); ++s) {
            const int index = static_cast<int>(s);
            consider(MonsterEntry{true, index, At(names.supers, index)}, 0, d, -1);
        }
    }
    std::vector<WhoRow> rows;
    for (auto& entry : groups) {
        Group& g = entry.second;
        std::sort(g.hits.begin(), g.hits.end(), [](const Hit& x, const Hit& y) {
            return x.chance != y.chance ? x.chance > y.chance : x.quest > y.quest;
        });
        WhoRow row;
        row.monster = g.monster;
        row.diff = g.diff;
        row.type = g.type;
        row.chance = g.hits.front().chance;
        row.quest = g.hits.front().quest;
        row.zone = g.hits.front().zone;
        row.zoneCount = static_cast<int>(g.hits.size());
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const WhoRow& a, const WhoRow& b) {
        return a.chance != b.chance ? a.chance > b.chance : a.monster.name < b.monster.name;
    });
    return rows;
}

std::vector<ItemEntry> ItemList(const Data& data, const Names& names) {
    std::vector<ItemEntry> out;
    for (std::size_t b = 0; b < data.items.size(); ++b) {
        out.push_back({"i" + std::to_string(b), At(names.items, static_cast<int>(b))});
    }
    for (std::size_t u = 0; u < data.uniques.size(); ++u) {
        out.push_back({"u" + std::to_string(u), At(names.uniques, static_cast<int>(u))});
    }
    for (std::size_t s = 0; s < data.sets.size(); ++s) {
        out.push_back({"s" + std::to_string(s), At(names.sets, static_cast<int>(s))});
    }
    return out;
}

}  // namespace esrwiki::drop
