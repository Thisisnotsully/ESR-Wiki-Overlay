// The Drop Finder panel's model: names, the monster list, and the rows the panel shows. Platform-neutral; the Win32
// panel (overlay_drop.inc) only lays these rows out and draws them. Chances come from drop_finder.hpp.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "drop_finder.hpp"

namespace esrwiki::drop {

// The names that go with the data (df-names.json): index-aligned with the data's tables.
struct Names {
    std::vector<std::string> items, uniques, sets, mons, supers, zones, types, acts;
};

bool LoadNames(const std::string& json, Names& out, std::string& error);
// The panel's English strings (df-ui.json), keyed by the website's own names.
bool LoadStrings(const std::string& json, std::map<std::string, std::string>& out, std::string& error);

// A monster the panel can list: a plain monster flagged in the data, or any superunique.
struct MonsterEntry {
    bool superunique = false;
    int index = 0;
    std::string name;
};
std::vector<MonsterEntry> MonsterList(const Data& data, const Names& names);
// Every monster, listed or not, and every superunique: what the "dropped by" lookup searches.
std::vector<MonsterEntry> EveryMonster(const Data& data, const Names& names);

// Everything the panel needs from the website's Drop Finder page: the data, its names and its English strings.
struct DropFinderData {
    Data data;
    Names names;
    std::map<std::string, std::string> strings;
};

// The panel's search setup for a monster: zone (-1 for none), difficulty 0..2, monster type 0..3, terror 0..2.
struct Setup {
    int zone = -1;
    int diff = 0;
    int type = 0;
    int terror = 0;
};

// One row of "Drops": the name of the drop, its chance per kill and the quest chance of the same drop.
struct DropRow {
    std::string key;
    std::string name;
    double chance = 0;
    double quest = 0;
};
// Rows sorted by chance, highest first. An empty list means the monster drops nothing for this setup.
std::vector<DropRow> DropsOf(Engine& engine, const Names& names, const MonsterEntry& monster, const Settings& settings,
                             const Setup& setup);

// One row of "Dropped by": the monster and the difficulty and type that give the best chance for the item.
struct WhoRow {
    MonsterEntry monster;
    int diff = 0;
    int type = 0;
    double chance = 0;
    double quest = 0;
    int zone = -1;      // the zone the best chance comes from; -1 is no fixed zone
    int zoneCount = 1;  // how many zones this monster drops the item in at this difficulty and type
};
// The monsters that drop the item, as the website's "Dropped by" lists them: every zone's monsters at each
// difficulty and type, plus the listed monsters no zone spawns, and every superunique.
std::vector<WhoRow> WhoDrops(Engine& engine, const Data& data, const Names& names, const Settings& settings, int terror,
                             const std::string& key);

// The display name of a result key ("i12", "u4", "s7", "r3" and the like, as the engine writes them).
std::string KeyName(const Names& names, const std::map<std::string, std::string>& strings, const std::string& key);

// Items the panel can search for: every base item, unique and set, as result keys.
struct ItemEntry {
    std::string key;
    std::string name;
};
std::vector<ItemEntry> ItemList(const Data& data, const Names& names);

}  // namespace esrwiki::drop
