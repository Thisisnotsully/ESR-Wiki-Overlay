// Drop Finder engine: a C++ port of the website's drop calculator (data/drop-finder/df-engine.js). The website's data
// is read from df-data.json; every chance this produces is checked against the page's own script by
// tests/drop_finder_test.cpp. Chances are plain doubles and are reported as the page reports them.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace esrwiki::drop {

// What the page calls a Map of result keys to chances. Keys are the engine's own keys: "g" gold, "i<item>" an
// item, "q<item>" a quest item, "u<unique>" and "s<set>" a unique or set entry, "x<item>" a rare-less unique item,
// and "r/m/h/n/l<type>" the rare, magic, superior, normal and low bands of a type.
using OutMap = std::map<std::string, double>;

// The Drop Finder's settings: players in the game, Magic Find and Item Quality (plugin config).
struct Settings {
    int players = 1;
    int mf = 0;
    int iq = 0;
};

// The page's config block (cfg in df-data.json).
struct Config {
    std::set<int> cowMons;
    std::set<int> cowSets;
    std::string iqMode;
    bool iqExcludePE = false;
    int iqMaxBonus = 0;
    std::array<int, 3> mfTail{};
    int heraldBoost = 0;
    std::array<int, 3> terror{};
};

struct Type {
    int flags = 0;  // TYPES[i][2]
};

struct Item {
    int type = 0;    // I[0]
    int ilvl = 0;    // I[2]
    int flags = 0;   // I[3]
    int ratio = 0;   // I[4], an index into Data::ratios
};

struct Unique {
    int item = 0;    // U[0]
    int ilvl = 0;    // U[1]
    int weight = 0;  // U[2]
    int flags = 0;   // U[3]
};

struct Set {
    int item = 0;    // S[0]
    int ilvl = 0;    // S[1]
    int weight = 0;  // S[2]
    int cow = 0;     // S[3], compared with Config::cowSets
};

struct TreasureClass {
    int group = 0;                           // T[0], rows of one group are a level walk
    int level = 0;                           // T[1]
    int picks = 0;                           // T[2], negative means a final chance
    int noDrop = 0;                          // T[3]
    std::array<int, 4> mods{};               // T[4]
    std::vector<std::array<int, 3>> entries;  // T[5]: {kind, ref, weight}
};

struct Monster {
    int code = 0;                             // M[0]
    int flags = 0;                            // M[1]: 1 boss, 2 no-regular, 4 pe, 8 quest, plus the drop-list flag
    std::array<int, 3> base{};                // M[2]
    std::array<std::array<int, 8>, 3> tc{};   // M[3]: per difficulty
    bool listed = false;                      // M[5]: the monster appears in the monster list
};

struct Superunique {
    int mon = 0;                    // SUP[i][0], an index into Data::mons
    std::array<int, 3> normal{};    // SUP[i][1]
    std::array<int, 3> terror{};    // SUP[i][2]
};

struct Zone {
    std::array<int, 3> base{};     // ZON[i][1]
    std::array<int, 3> terror{};   // ZON[i][5]
    int act = 0;                   // ZON[i][0]: the act index, into Names::acts
    // The monsters that spawn here: groups[g][d] for ZON[i][2..4][d] (regular, champion and unique, minion).
    std::array<std::array<std::vector<int>, 3>, 3> groups{};
};

struct Data {
    int txtCount = 0;
    int goldItem = 0;
    std::vector<Type> types;
    std::vector<Item> items;
    std::vector<std::array<int, 16>> ratios;
    std::vector<Unique> uniques;
    std::vector<Set> sets;
    std::vector<TreasureClass> tcs;
    std::vector<Monster> mons;
    std::vector<Superunique> supers;
    std::vector<Zone> zones;
    Config cfg;
};

// Reads df-data.json. On failure `error` says what was malformed and `out` is not meant to be used.
bool LoadData(const std::string& json, Data& out, std::string& error);

// A monster selection: a plain monster (superunique false, i is an index into Data::mons) or a superunique (i is
// an index into Data::supers); d difficulty 0..2, t type 0..3, x terror 0..2, z zone index or -1 for none.
struct Selection {
    bool superunique = false;
    int i = 0;
    int d = 0;
    int t = 0;
    int x = 0;
    int z = -1;
};

// The treasure class context: the monster's item level and whether it is a cow-plugin monster; k is the cache key.
struct Ctx {
    int ilvl = 0;
    bool cow = false;
    std::string k;
};

// What Resolve gives for a selection: the treasure classes to read, the quest treasure class, and the context.
struct Resolved {
    int tc = -1;
    int qtc = -1;
    int ilvl = 0;
    Ctx c;
    bool pe = false;
    bool approx = false;
};

// The item (or set of items) a "dropped by" lookup asks about: the keys it stands for and the bases they cover.
struct Bases {
    std::vector<int> arr;
    std::vector<char> in;  // in[b] is 1 when base item b is in arr
};

class Engine {
public:
    explicit Engine(Data data);

    const Data& data() const { return data_; }

    // Changes the settings. Clears the chance caches, as the page's script does.
    void SetSettings(const Settings& s);

    Resolved Resolve(const Selection& v) const;

    // The chance of each result key for a monster's main treasure class (and its quest one).
    OutMap TopMap(int t, const Ctx& c, bool pe);

    // The chance that a monster's treasure class drops the item (key R) with bases B.
    double Top1(int t, const Ctx& c, bool pe, const std::string& R, const Bases& B);

    Bases BasesOf(const std::string& R) const;

private:
    using Mods = std::array<int, 4>;
    using Choice = std::vector<std::pair<std::string, double>>;

    struct TcPick {
        bool neg = false;
        int k = 1;
        OutMap map;
    };

    struct IqExtra {
        int e0 = 0;
        double r = 0;
    };

    IqExtra Iq(int picks, bool pe) const;
    double Roll(double v) const;
    std::array<double, 7> QDist(int b, const Mods& mods, int ilvl) const;
    Choice UniSel(int b, int req, int ilvl) const;
    Choice SetSel(int b, int req, int ilvl, bool cow) const;
    void AddUnique(OutMap& out, int b, int req, double p, const Ctx& c) const;
    void AddSet(OutMap& out, int b, int req, double p, const Ctx& c) const;
    const OutMap& ItemOut(int b, const Mods& mods, const Ctx& c);
    const OutMap& DirectOut(int kind, int ref, const Ctx& c);
    const OutMap& EntryMap(const std::array<int, 3>& e, const Mods& mods, const Ctx& c);
    const TcPick& TcPickOf(int t, const Mods& mods, const Ctx& c);
    const OutMap& TcMap(int t, const Mods& mods, const Ctx& c);
    double Pick1(int t, const Mods& mods, const Ctx& c, const std::string& R, const Bases& B);
    double Hit1(int t, const Mods& mods, const Ctx& c, const std::string& R, const Bases& B);
    double Entry1(const std::array<int, 3>& e, const Mods& mods, const Ctx& c, const std::string& R, const Bases& B);
    const std::vector<std::uint32_t>& ReachBits(int t);
    bool Reach(int t, const Bases& B);
    int Upgrade(int t, int lvl) const;

    Data data_;
    Settings env_;
    std::vector<std::vector<int>> uniByItem_;
    std::vector<std::vector<int>> setByItem_;
    std::vector<std::vector<int>> itemsByType_;
    int nw_ = 0;
    std::unordered_map<std::string, OutMap> itemCache_;
    std::unordered_map<std::string, OutMap> directCache_;
    std::unordered_map<std::string, OutMap> tcMapCache_;
    std::unordered_map<std::string, TcPick> pickCache_;
    std::unordered_map<std::string, double> pick1Cache_;
    std::unordered_map<int, std::vector<std::uint32_t>> reachCache_;
};

}  // namespace esrwiki::drop
