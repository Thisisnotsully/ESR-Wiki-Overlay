// The Campaign Progression map (Welcome page) as data without any Win32: the reader of data/campaign-map.txt
// (written by tools/build_campaign.py) and the layout helpers the renderer needs.
//
// The file holds seven tabs: Act I-V are hand-placed node graphs on a grid ("GRID" rows of tokens), Special areas and
// Ubers & portals are card grids (a list of area ids). Every area carries its act, terrain, waypoint, monster levels,
// caveats, bosses, quest steps, notes and its name in the 13 game languages; edges carry their kind and label.
//
// Coordinates: the layout helpers work in CELL UNITS: x = grid column, y = grid row (fractions for the dots of a
// dungeon stack: member i of a stack sits at row + i * kStackPitchCells). The renderer maps a cell point to pixels with
//     px = left + kMarginX * s + x * kCellW * s,   py = top + kMarginY * s + y * kCellH * s
// (s = its scale; the constants are the reference design's sizes at scale 1). Edge routes are octilinear in that
// pixel space as long as the renderer keeps the kCellW : kCellH ratio (or passes its own cell size to TabEdges).
//
// Curated facts keep their source as one letter: 'O' official Maps page, 'T' the txt files, 'V' vanilla Diablo II,
// 'W' confirmed in game; lowercase ('o', 't', 'v', 'w') = not confirmed yet. Platform-neutral.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace esrwiki::campaign {

inline constexpr const char* kFileName = "campaign-map.txt";
inline constexpr int kFormatVersion = 1;

// The reference design's sizes at scale 1 (pixels).
inline constexpr double kCellW = 124;
inline constexpr double kCellH = 88;
inline constexpr double kMarginX = 62;
inline constexpr double kMarginY = 44;
inline constexpr double kStackPitch = 22;  // distance between the dots of a dungeon stack
inline constexpr double kDotR = 6;
inline constexpr double kStackPitchCells = kStackPitch / kCellH;  // 0.25 of a row

enum class EdgeKind { Border, Passage, Portal, Cube, Act };

inline bool Unconfirmed(char source) { return source >= 'a' && source <= 'z'; }

struct Note {
    char source = 'T';
    bool warning = false;  // a caveat to show before going there
    bool spoiler = false;  // hidden behind a click
    std::string text;
};

struct Quest {
    char source = 'T';
    std::string item;   // quest item code ("vip"), empty when none; ItemName() gives its name
    std::string quest;  // "The Horadric Staff"
    std::string how;    // what to do here
};

struct Area {
    int id = 0;              // levels.txt Id
    std::string tab;         // the tab it is drawn on: act1..act5, portals (uber levels, the unique map)
    int act = 0;             // 0-4
    std::string terrain;     // town outdoor cave tomb dungeon temple tower special
    bool waypoint = false;
    int level[3] = {0, 0, 0};  // monster level Normal, Nightmare, Hell; 0 = no monsters (towns)
    bool town = false;
    bool special = false;       // ESR special area (listed on the Special areas tab)
    bool endgameZone = false;   // the game names it "... (Endgame Zone)" (suffix removed from the names)
    bool noTownPortal = false;  // levels.txt PreventTownPortal
    bool teleportOff = false;   // levels.txt Teleport 0
    bool teleportLimited = false;  // levels.txt Teleport 2
    bool teleportWalls = false;    // on the official page's "No Teleport across the walls" list
    bool entryUnknown = false;     // no known way in
    int twinOf = 0;  // a folded twin row (Tal Rasha's Tomb x7): the id of the row the grid shows; 0 otherwise
    std::vector<std::pair<std::string, std::string>> names;  // language code -> name; "enUS" always present
    std::string officialName;  // the official Maps page's name when it differs
    std::vector<std::string> bosses;
    std::vector<Quest> quests;
    std::vector<Note> notes;
    char entrySource = 0;
    std::string entry;  // how to get in when it is not a plain walk
};

struct Edge {
    int from = 0, to = 0;
    EdgeKind kind = EdgeKind::Passage;
    bool oneWay = false;  // only from -> to
    char source = 'T';
    std::string via;      // short label ("Cairn Stones"), may be empty
};

struct Point {
    double x = 0, y = 0;
};

// One grid cell: a station (one id), a dungeon stack (several ids, top to bottom) or a ghost (an area drawn on
// another tab; selecting it should switch to that area's tab).
struct Station {
    int col = 0, row = 0;
    std::vector<int> ids;
    bool stack = false;
    bool ghost = false;
};

struct Region {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // cell units, corners
    std::string label;
};

struct RouteHint {
    int from = 0, to = 0;        // the route is computed from `from`
    bool diagonalFirst = false;  // start with the 45 degree segment
};

struct Tab {
    std::string key;    // act1..act5, special, portals
    std::string title;  // English ("Act I", "Special areas", "Ubers & portals")
    bool grid = false;  // node graph (else a card grid)
    int act = -1;
    int rows = 0, cols = 0;
    std::vector<std::string> gridRows;  // the raw rows (tokens separated by spaces)
    std::vector<Station> stations;
    std::vector<Region> regions;
    std::vector<RouteHint> routes;
    std::vector<int> cards;  // card tabs: the areas in order
    std::vector<Note> notes;
};

struct Data {
    int version = 0;
    std::vector<std::string> languages;
    std::vector<Tab> tabs;
    std::vector<Area> areas;  // sorted by id
    std::vector<Edge> edges;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> items;  // code -> language -> name
    std::map<int, std::size_t> areaIndex;
};

// Reads campaign-map.txt. Fails, leaving `out` untouched, on a wrong header, a malformed record, an unknown tab or
// area id, an edge or grid cell naming an unknown area, or an area without an English name.
bool Parse(const std::string& text, Data& out, std::string& error);

const Area* FindArea(const Data& data, int id);
const Tab* FindTab(const Data& data, const std::string& key);
// The name in `lang` ("deDE"), else the English one; empty for an unknown id.
std::string AreaName(const Data& data, int id, const std::string& lang);
std::string NameIn(const Area& area, const std::string& lang);
// A quest item's name in `lang`, else English, else the code.
std::string ItemName(const Data& data, const std::string& code, const std::string& lang);
// Indices into data.edges of the edges touching `id` (both directions).
std::vector<std::size_t> EdgesOf(const Data& data, int id);
// Everything the search should match for an area, one entry per line: its name in `lang`, the English name (when
// different), the official name, the bosses, the quest names and the quest item names in `lang`.
std::string SearchText(const Data& data, int id, const std::string& lang);

// ---- layout (cell units) -------------------------------------------------------------------------------------

struct Node {
    int id = 0;               // the area (for a ghost: the area on the other tab)
    std::size_t station = 0;  // index into tab.stations
    int member = 0;           // position in its stack (0 for a plain station)
    Point at;                 // dot centre in cell units
    bool ghost = false;
    std::vector<int> folded;  // twin rows drawn as this one dot (Tal Rasha's Tomb: 67..72)
};

// One node per dot of a grid tab, in reading order (row by row, a stack's members top to bottom).
std::vector<Node> LayoutNodes(const Data& data, const Tab& tab);
// The node index showing `areaId` (twins resolve to their folded dot), or -1.
int NodeOf(const Data& data, const std::vector<Node>& nodes, int areaId);

// Pad navigation: the nearest node in each direction (node indices, -1 = none). A node counts for "right" when it
// lies within 45 degrees of the direction (else, as a fallback, anywhere on that side); the score is the distance
// along the direction plus twice the distance across it, measured in pixel proportions (kCellW : kCellH).
struct Neighbours {
    int left = -1, right = -1, up = -1, down = -1;
};
std::vector<Neighbours> NeighbourTable(const std::vector<Node>& nodes);

// Octilinear polyline a -> b: one horizontal or vertical segment and one 45 degree segment (diagonal first or straight
// first). Aligned or exactly diagonal points give 2 points, else 3. Works in whatever space a and b are in.
std::vector<Point> RoutePoints(Point a, Point b, bool diagonalFirst);

struct DrawnEdge {
    std::size_t edge = 0;      // index into data.edges
    int fromNode = -1, toNode = -1;
    std::vector<Point> points;  // cell units, from the edge's `from` end to its `to` end
};
// The edges of a grid tab whose two ends are both drawn on it (twins folded, duplicates and links inside one stack
// dropped), routed with the tab's hints in a space of cellW x cellH per cell, returned in cell units.
std::vector<DrawnEdge> TabEdges(const Data& data, const Tab& tab, const std::vector<Node>& nodes,
                                double cellW = kCellW, double cellH = kCellH);

const char* EdgeKindName(EdgeKind kind);

}  // namespace esrwiki::campaign
