// Tests for the Campaign Progression map reader. Run with the path of data/campaign-map.txt as the argument.
#include "campaign_data.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace {
int failures = 0;
void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}
bool Octilinear(const esrwiki::campaign::Point& a, const esrwiki::campaign::Point& b, double sx, double sy) {
    const double dx = std::fabs((b.x - a.x) * sx), dy = std::fabs((b.y - a.y) * sy);
    return dx < 1e-6 || dy < 1e-6 || std::fabs(dx - dy) < 1e-6;
}
}  // namespace

int main(int argc, char** argv) {
    using namespace esrwiki::campaign;
    std::string error;
    Data none;
    Check(!Parse("AREA\t1\tact1\t0\toutdoor\t0\t0\t0\t0\t-\t0\n", none, error), "a file without the header fails");
    Check(!Parse("#ESRCAMPAIGN\t9\n", none, error), "a newer major version fails");

    const char* tiny =
        "#ESRCAMPAIGN\t1\n"
        "LANGS\tenUS\tdeDE\n"
        "TAB\tact1\tgrid\t0\tAct I\n"
        "GRID\tact1\t1 2 . 3..4\n"
        "GRID\tact1\t. 5 @7 .\n"
        "ROUTE\tact1\t1\t5\td\n"
        "TAB\tact2\tgrid\t1\tAct II\n"
        "GRID\tact2\t7\n"
        "TAB\tspecial\tcards\t-1\tSpecial areas\n"
        "CARD\tspecial\t2\n"
        "SNOTE\tspecial\tO\tw\tCareful\n"
        "AREA\t1\tact1\t0\ttown\t1\t0\t0\t0\ttown,notp\t0\nNAME\t1\tenUS\tTown\n"
        "AREA\t2\tact1\t0\toutdoor\t0\t2\t38\t68\tspecial,walls\t0\nNAME\t2\tenUS\tBlood Moor\nNAME\t2\tdeDE\tBlutmoor\n"
        "BOSS\t2\tCorpsefire\nQUEST\t2\tW\tbkd\tThe Search\tTouch\nNOTE\t2\to\ts\tHidden\nENTRY\t2\tT\tWalk\n"
        "AREA\t3\tact1\t0\tcave\t0\t1\t2\t3\t-\t0\nNAME\t3\tenUS\tCave Level 1\n"
        "AREA\t4\tact1\t0\tcave\t0\t1\t2\t3\t-\t0\nNAME\t4\tenUS\tCave Level 2\n"
        "AREA\t5\tact1\t0\ttomb\t0\t1\t2\t3\t-\t0\nNAME\t5\tenUS\tTomb\n"
        "AREA\t6\tact1\t0\ttomb\t0\t1\t2\t3\t-\t5\nNAME\t6\tenUS\tTomb\n"
        "AREA\t7\tact2\t1\ttown\t1\t0\t0\t0\ttown\t0\nNAME\t7\tenUS\tOther\n"
        "EDGE\t1\t2\tborder\t0\tW\t\n"
        "EDGE\t1\t5\tportal\t1\tO\tRed portal\n"
        "EDGE\t3\t4\tpassage\t0\tT\t\n"
        "EDGE\t2\t7\tact\t1\tW\tShip\n"
        "ITEM\tbkd\tenUS\tKey\nITEM\tbkd\tdeDE\tSchluessel\n";
    Data d;
    Check(Parse(tiny, d, error), "tiny file parses: " + error);
    Check(d.version == 1 && d.languages.size() == 2 && d.tabs.size() == 3, "header, languages, tabs");
    const Tab* t1 = FindTab(d, "act1");
    Check(t1 != nullptr && t1->rows == 2 && t1->cols == 4 && t1->stations.size() == 5, "grid stations");
    const Area* a2 = FindArea(d, 2);
    Check(a2 != nullptr && a2->special && a2->teleportWalls && a2->level[2] == 68, "area fields and flags");
    Check(a2 && a2->bosses.size() == 1 && a2->quests.size() == 1 && a2->quests[0].item == "bkd", "boss and quest");
    Check(a2 && a2->notes.size() == 1 && a2->notes[0].spoiler && Unconfirmed(a2->notes[0].source), "note flags");
    Check(AreaName(d, 2, "deDE") == "Blutmoor" && AreaName(d, 2, "frFR") == "Blood Moor", "localized name, EN fallback");
    Check(ItemName(d, "bkd", "deDE") == "Schluessel" && ItemName(d, "bkd", "jaJP") == "Key", "item name");
    Check(SearchText(d, 2, "deDE") == "Blutmoor\nBlood Moor\nCorpsefire\nThe Search\nSchluessel", "search text");
    Check(FindTab(d, "special")->cards.size() == 1 && FindTab(d, "special")->notes[0].warning, "cards and tab notes");
    const auto nodes = LayoutNodes(d, *t1);
    // 1, 2, 3 and 4 (stack), 5 (with its twin 6 folded), ghost 7
    Check(nodes.size() == 6, "one node per dot");
    Check(nodes.size() == 6 && nodes[3].at.y == 0.25 && nodes[3].member == 1, "stack member offset");
    Check(nodes.size() == 6 && nodes[5].ghost && nodes[5].id == 7, "ghost node");
    Check(nodes.size() == 6 && nodes[4].folded.size() == 1 && nodes[4].folded[0] == 6, "folded twin listed");
    const int tomb = NodeOf(d, nodes, 6);
    Check(tomb >= 0 && nodes[tomb].id == 5, "a folded twin resolves to its station");
    const auto nb = NeighbourTable(nodes);
    Check(nb[0].right == 1 && nb[1].left == 0 && nb[0].down == NodeOf(d, nodes, 5), "neighbours");
    const auto edges = TabEdges(d, *t1, nodes);
    // 1-2, 1-5 (diagonal hint), 2-7 (ghost); 3-4 is inside one stack
    Check(edges.size() == 3, "edges drawn on the tab");
    for (const auto& e : edges) {
        Check(e.points.size() >= 2 && e.points.size() <= 3, "route has 2 or 3 points");
        if (d.edges[e.edge].from == 1 && d.edges[e.edge].to == 5)
            Check(e.points.size() == 3 && std::fabs(e.points[1].y - 1) < 1e-9, "diagonal-first route starts at 45 degrees");
    }
    const auto r = RoutePoints({0, 0}, {3, 1}, false);
    Check(r.size() == 3 && r[1].x == 2 && r[1].y == 0, "straight-first route");

    if (argc > 1) {
        std::ifstream f(argv[1], std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        Data real;
        Check(Parse(ss.str(), real, error), "campaign-map.txt parses: " + error);
        Check(ss.str().size() < 300 * 1024, "the file stays under 300 KB");
        Check(real.languages.size() == 13, "13 languages");
        Check(real.tabs.size() == 7, "seven tabs");
        Check(real.areas.size() > 120, "the campaign areas");
        const char* acts[] = {"act1", "act2", "act3", "act4", "act5"};
        for (const char* key : acts) {
            const Tab* t = FindTab(real, key);
            Check(t != nullptr && t->grid && t->rows > 0 && !t->stations.empty(), std::string(key) + " has a grid");
            if (t == nullptr) continue;
            std::set<int> shown;
            for (const Station& st : t->stations)
                for (int id : st.ids) {
                    const Area* a = FindArea(real, id);
                    Check(a != nullptr, std::string(key) + ": station " + std::to_string(id) + " resolves");
                    if (a && !st.ghost) {
                        Check(a->tab == key, std::string(key) + ": station " + std::to_string(id) + " belongs to it");
                        shown.insert(id);
                    }
                    if (a && st.ghost) Check(a->tab != key, std::string(key) + ": ghost points at another tab");
                }
            for (const Area& a : real.areas)
                if (a.tab == key)
                    Check(shown.count(a.id) || (a.twinOf && shown.count(a.twinOf)),
                          std::string(key) + ": area " + std::to_string(a.id) + " is placed");
            const auto n = LayoutNodes(real, *t);
            const auto nt = NeighbourTable(n);
            int links = 0, back = 0, lonely = 0;
            for (std::size_t i = 0; i < n.size(); ++i) {
                const Neighbours& x = nt[i];
                if (x.left < 0 && x.right < 0 && x.up < 0 && x.down < 0) ++lonely;
                if (x.right >= 0) { ++links; back += nt[x.right].left == static_cast<int>(i); }
                if (x.left >= 0) { ++links; back += nt[x.left].right == static_cast<int>(i); }
                if (x.down >= 0) { ++links; back += nt[x.down].up == static_cast<int>(i); }
                if (x.up >= 0) { ++links; back += nt[x.up].down == static_cast<int>(i); }
            }
            Check(n.size() < 2 || lonely == 0, std::string(key) + ": every node has a neighbour");
            Check(links > 0 && back * 10 >= links * 6,
                  std::string(key) + ": neighbours mostly symmetric (" + std::to_string(back) + "/" + std::to_string(links) + ")");
            const auto drawn = TabEdges(real, *t, n);
            Check(!drawn.empty(), std::string(key) + ": edges drawn");
            for (const auto& e : drawn) {
                Check(e.points.size() >= 2 && e.points.size() <= 3, std::string(key) + ": route has at most 3 points");
                for (std::size_t p = 1; p < e.points.size(); ++p)
                    Check(Octilinear(e.points[p - 1], e.points[p], kCellW, kCellH), std::string(key) + ": route is octilinear");
            }
        }
        for (const Edge& e : real.edges)
            Check(FindArea(real, e.from) && FindArea(real, e.to), "edge endpoints exist");
        int localized = 0;
        for (const Area& a : real.areas)
            for (const std::string& lang : real.languages) {
                const std::string name = NameIn(a, lang);
                Check(!name.empty(), "area " + std::to_string(a.id) + " has a name in " + lang);
                for (const auto& [code, text] : a.names)
                    if (code == lang) ++localized;
            }
        Check(localized * 10 >= static_cast<int>(real.areas.size() * real.languages.size()) * 9, "names mostly localized");
        Check(AreaName(real, 2, "deDE") == "Blutmoor", "deDE Blood Moor");
        Check(AreaName(real, 3, "jaJP") == "\xE5\x86\xB7\xE3\x81\x9F\xE3\x81\x84\xE5\xB9\xB3\xE5\x8E\x9F", "jaJP Cold Plains");
        Check(AreaName(real, 2, "xxXX") == "Blood Moor", "unknown language falls back to English");
        Check(AreaName(real, 44, "enUS") == "Lost Farm" && FindArea(real, 44)->endgameZone, "Endgame Zone suffix removed");
        Check(FindArea(real, 67) && FindArea(real, 67)->twinOf == 66, "Tal Rasha's tombs fold into one");
        Check(FindArea(real, 25) && FindArea(real, 25)->teleportWalls, "official teleport list");
        Check(FindArea(real, 1) && FindArea(real, 1)->town && FindArea(real, 1)->noTownPortal, "town flags");
        Check(SearchText(real, 61, "enUS").find("Fangskin") != std::string::npos, "search text has the boss");
        Check(SearchText(real, 61, "enUS").find("Top of the Horadric Staff") != std::string::npos, "search text has the quest item");
        const Tab* special = FindTab(real, "special");
        const Tab* portals = FindTab(real, "portals");
        Check(special && !special->grid && special->cards.size() >= 15, "special areas cards");
        Check(portals && !portals->grid && portals->cards.size() >= 5, "ubers and portals cards");
        Check(FindTab(real, "act1") && !FindTab(real, "act1")->notes.empty(), "act notes (removed levels)");
    }
    if (failures == 0) std::cout << "campaign_data tests passed\n";
    return failures == 0 ? 0 : 1;
}
