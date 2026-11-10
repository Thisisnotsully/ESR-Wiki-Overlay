// Reader and layout helpers of the Campaign Progression map. See campaign_data.hpp.
#include "campaign_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

namespace esrwiki::campaign {

namespace {

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return out;
}

std::vector<std::string> SplitSpaces(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool ToInt(const std::string& s, int& v) {
    if (s.empty()) return false;
    std::size_t i = (s[0] == '-') ? 1 : 0;
    if (i == s.size()) return false;
    long value = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + (s[i] - '0');
        if (value > 1000000) return false;
    }
    v = static_cast<int>(s[0] == '-' ? -value : value);
    return true;
}

bool ToDouble(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0';
}

bool SourceOk(const std::string& s) {
    return s.size() == 1 && std::string("OTVWotvw").find(s[0]) != std::string::npos;
}

bool NoteFlags(const std::string& s, Note& note) {
    if (s == "-") return true;
    for (char c : s) {
        if (c == 'w') note.warning = true;
        else if (c == 's') note.spoiler = true;
        else return false;
    }
    return true;
}

bool KindOf(const std::string& s, EdgeKind& kind) {
    if (s == "border") kind = EdgeKind::Border;
    else if (s == "passage") kind = EdgeKind::Passage;
    else if (s == "portal") kind = EdgeKind::Portal;
    else if (s == "cube") kind = EdgeKind::Cube;
    else if (s == "act") kind = EdgeKind::Act;
    else return false;
    return true;
}

// One grid token -> a station (false for a malformed token). '.' is handled by the caller.
bool ParseToken(const std::string& tok, Station& st) {
    st.ids.clear();
    st.stack = st.ghost = false;
    int v = 0;
    if (tok[0] == '@') {
        if (!ToInt(tok.substr(1), v) || v <= 0) return false;
        st.ghost = true;
        st.ids.push_back(v);
        return true;
    }
    const std::size_t range = tok.find("..");
    if (range != std::string::npos) {
        int lo = 0, hi = 0;
        if (!ToInt(tok.substr(0, range), lo) || !ToInt(tok.substr(range + 2), hi) || hi <= lo || lo <= 0) return false;
        for (int i = lo; i <= hi; ++i) st.ids.push_back(i);
        st.stack = true;
        return true;
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t plus = tok.find('+', start);
        if (!ToInt(tok.substr(start, plus == std::string::npos ? std::string::npos : plus - start), v) || v <= 0)
            return false;
        st.ids.push_back(v);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    st.stack = st.ids.size() > 1;
    return true;
}

std::string Lookup(const std::vector<std::pair<std::string, std::string>>& names, const std::string& lang) {
    const std::string* en = nullptr;
    for (const auto& [code, text] : names) {
        if (code == lang) return text;
        if (code == "enUS") en = &text;
    }
    return en ? *en : std::string();
}

}  // namespace

const char* EdgeKindName(EdgeKind kind) {
    switch (kind) {
        case EdgeKind::Border: return "border";
        case EdgeKind::Passage: return "passage";
        case EdgeKind::Portal: return "portal";
        case EdgeKind::Cube: return "cube";
        case EdgeKind::Act: return "act";
    }
    return "passage";
}

bool Parse(const std::string& text, Data& out, std::string& error) {
    Data d;
    std::size_t pos = 0;
    int lineNo = 0;
    bool header = false;
    Area* area = nullptr;
    auto fail = [&](const std::string& why) {
        // Checks made after the last line was read (lineNo reset to 0) have no line to name.
        error = lineNo > 0 ? "campaign-map.txt line " + std::to_string(lineNo) + ": " + why : "campaign-map.txt: " + why;
        return false;
    };
    auto tabOf = [&](const std::string& key) -> Tab* {
        for (Tab& t : d.tabs)
            if (t.key == key) return &t;
        return nullptr;
    };
    auto areaOf = [&](const std::string& idText) -> Area* {
        int id = 0;
        if (!ToInt(idText, id)) return nullptr;
        if (area != nullptr && area->id == id) return area;
        for (Area& a : d.areas)
            if (a.id == id) return &a;
        return nullptr;
    };
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lineNo == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);
        if (!header) {
            if (line.empty()) continue;
            const auto f = SplitTabs(line);
            if (f.size() < 2 || f[0] != "#ESRCAMPAIGN" || !ToInt(f[1], d.version))
                return fail("not a campaign map file (no #ESRCAMPAIGN header)");
            if (d.version < 1 || d.version > kFormatVersion) return fail("unsupported version " + f[1]);
            header = true;
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        const auto f = SplitTabs(line);
        const std::string& r = f[0];
        if (r == "LANGS") {
            d.languages.assign(f.begin() + 1, f.end());
        } else if (r == "TAB") {
            if (f.size() < 5) return fail("TAB needs 4 fields");
            Tab t;
            t.key = f[1];
            if (f[2] != "grid" && f[2] != "cards") return fail("unknown tab kind " + f[2]);
            t.grid = f[2] == "grid";
            if (!ToInt(f[3], t.act)) return fail("bad act");
            t.title = f[4];
            if (tabOf(t.key) != nullptr) return fail("tab " + t.key + " twice");
            d.tabs.push_back(t);
            area = nullptr;
        } else if (r == "GRID") {
            if (f.size() < 3) return fail("GRID needs 2 fields");
            Tab* t = tabOf(f[1]);
            if (t == nullptr || !t->grid) return fail("GRID for unknown or card tab " + f[1]);
            const auto toks = SplitSpaces(f[2]);
            const int row = t->rows++;
            t->gridRows.push_back(f[2]);
            t->cols = std::max(t->cols, static_cast<int>(toks.size()));
            for (std::size_t c = 0; c < toks.size(); ++c) {
                if (toks[c] == ".") continue;
                Station st;
                if (!ParseToken(toks[c], st)) return fail("bad grid token " + toks[c]);
                st.col = static_cast<int>(c);
                st.row = row;
                t->stations.push_back(st);
            }
        } else if (r == "REGION") {
            if (f.size() < 7) return fail("REGION needs 6 fields");
            Tab* t = tabOf(f[1]);
            Region g;
            if (t == nullptr || !ToDouble(f[2], g.x0) || !ToDouble(f[3], g.y0) || !ToDouble(f[4], g.x1) ||
                !ToDouble(f[5], g.y1))
                return fail("bad REGION");
            g.label = f[6];
            t->regions.push_back(g);
        } else if (r == "ROUTE") {
            if (f.size() < 5) return fail("ROUTE needs 4 fields");
            Tab* t = tabOf(f[1]);
            RouteHint h;
            if (t == nullptr || !ToInt(f[2], h.from) || !ToInt(f[3], h.to) || (f[4] != "d" && f[4] != "s"))
                return fail("bad ROUTE");
            h.diagonalFirst = f[4] == "d";
            t->routes.push_back(h);
        } else if (r == "CARD") {
            if (f.size() < 3) return fail("CARD needs 2 fields");
            Tab* t = tabOf(f[1]);
            int id = 0;
            if (t == nullptr || !ToInt(f[2], id)) return fail("bad CARD");
            t->cards.push_back(id);
        } else if (r == "SNOTE") {
            if (f.size() < 5) return fail("SNOTE needs 4 fields");
            Tab* t = tabOf(f[1]);
            Note n;
            if (t == nullptr || !SourceOk(f[2]) || !NoteFlags(f[3], n)) return fail("bad SNOTE");
            n.source = f[2][0];
            n.text = f[4];
            t->notes.push_back(n);
        } else if (r == "AREA") {
            if (f.size() < 11) return fail("AREA needs 10 fields");
            Area a;
            if (!ToInt(f[1], a.id) || a.id <= 0) return fail("bad area id");
            if (tabOf(f[2]) == nullptr) return fail("area " + f[1] + " names unknown tab " + f[2]);
            a.tab = f[2];
            if (!ToInt(f[3], a.act) || a.act < 0 || a.act > 4) return fail("bad act");
            a.terrain = f[4];
            a.waypoint = f[5] == "1";
            for (int i = 0; i < 3; ++i)
                if (!ToInt(f[6 + i], a.level[i])) return fail("bad level");
            if (f[9] != "-") {
                std::size_t start = 0;
                for (;;) {
                    const std::size_t comma = f[9].find(',', start);
                    const std::string flag = f[9].substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    if (flag == "town") a.town = true;
                    else if (flag == "special") a.special = true;
                    else if (flag == "zone") a.endgameZone = true;
                    else if (flag == "notp") a.noTownPortal = true;
                    else if (flag == "teleoff") a.teleportOff = true;
                    else if (flag == "telelimit") a.teleportLimited = true;
                    else if (flag == "walls") a.teleportWalls = true;
                    else if (flag == "entryunknown") a.entryUnknown = true;
                    // unknown flags are ignored (a newer file)
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
            if (!ToInt(f[10], a.twinOf)) return fail("bad twin");
            for (const Area& other : d.areas)
                if (other.id == a.id) return fail("area " + f[1] + " twice");
            d.areas.push_back(a);
            area = &d.areas.back();
        } else if (r == "NAME" || r == "OFFICIAL" || r == "BOSS" || r == "QUEST" || r == "NOTE" || r == "ENTRY") {
            if (f.size() < 3) return fail(r + " needs more fields");
            Area* a = areaOf(f[1]);
            if (a == nullptr) return fail(r + " for unknown area " + f[1]);
            area = a;
            if (r == "NAME") {
                if (f.size() < 4 || f[3].empty()) return fail("NAME needs a language and a text");
                a->names.emplace_back(f[2], f[3]);
            } else if (r == "OFFICIAL") {
                a->officialName = f[2];
            } else if (r == "BOSS") {
                a->bosses.push_back(f[2]);
            } else if (r == "QUEST") {
                if (f.size() < 6 || !SourceOk(f[2])) return fail("bad QUEST");
                Quest q;
                q.source = f[2][0];
                q.item = f[3] == "-" ? std::string() : f[3];
                q.quest = f[4];
                q.how = f[5];
                a->quests.push_back(q);
            } else if (r == "NOTE") {
                Note n;
                if (f.size() < 5 || !SourceOk(f[2]) || !NoteFlags(f[3], n)) return fail("bad NOTE");
                n.source = f[2][0];
                n.text = f[4];
                a->notes.push_back(n);
            } else {
                if (f.size() < 4 || !SourceOk(f[2])) return fail("bad ENTRY");
                a->entrySource = f[2][0];
                a->entry = f[3];
            }
        } else if (r == "EDGE") {
            if (f.size() < 6) return fail("EDGE needs 6 fields");
            Edge e;
            if (!ToInt(f[1], e.from) || !ToInt(f[2], e.to) || !KindOf(f[3], e.kind) || !SourceOk(f[5]))
                return fail("bad EDGE");
            e.oneWay = f[4] == "1";
            e.source = f[5][0];
            if (f.size() > 6) e.via = f[6];
            d.edges.push_back(e);
        } else if (r == "ITEM") {
            if (f.size() < 4) return fail("ITEM needs 3 fields");
            d.items[f[1]].emplace_back(f[2], f[3]);
        }
        // other records: a newer file; ignored
    }
    if (!header) {
        error = "campaign-map.txt is empty";
        return false;
    }
    lineNo = 0;
    std::sort(d.areas.begin(), d.areas.end(), [](const Area& a, const Area& b) { return a.id < b.id; });
    for (std::size_t i = 0; i < d.areas.size(); ++i) d.areaIndex[d.areas[i].id] = i;
    auto known = [&](int id) { return d.areaIndex.count(id) != 0; };
    for (const Area& a : d.areas) {
        if (Lookup(a.names, "enUS").empty()) return fail("area " + std::to_string(a.id) + " has no English name");
        if (a.twinOf != 0 && !known(a.twinOf)) return fail("area " + std::to_string(a.id) + " twin of unknown area");
    }
    for (const Edge& e : d.edges)
        if (!known(e.from) || !known(e.to))
            return fail("edge " + std::to_string(e.from) + "-" + std::to_string(e.to) + " names an unknown area");
    for (const Tab& t : d.tabs) {
        for (const Station& st : t.stations)
            for (int id : st.ids)
                if (!known(id)) return fail(t.key + ": grid names unknown area " + std::to_string(id));
        for (int id : t.cards)
            if (!known(id)) return fail(t.key + ": card names unknown area " + std::to_string(id));
    }
    if (d.areas.empty()) return fail("no areas");
    out = std::move(d);
    return true;
}

const Area* FindArea(const Data& data, int id) {
    const auto it = data.areaIndex.find(id);
    return it == data.areaIndex.end() ? nullptr : &data.areas[it->second];
}

const Tab* FindTab(const Data& data, const std::string& key) {
    for (const Tab& t : data.tabs)
        if (t.key == key) return &t;
    return nullptr;
}

std::string NameIn(const Area& area, const std::string& lang) { return Lookup(area.names, lang); }

std::string AreaName(const Data& data, int id, const std::string& lang) {
    const Area* a = FindArea(data, id);
    return a ? NameIn(*a, lang) : std::string();
}

std::string ItemName(const Data& data, const std::string& code, const std::string& lang) {
    const auto it = data.items.find(code);
    if (it == data.items.end()) return code;
    const std::string name = Lookup(it->second, lang);
    return name.empty() ? code : name;
}

std::vector<std::size_t> EdgesOf(const Data& data, int id) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < data.edges.size(); ++i)
        if (data.edges[i].from == id || data.edges[i].to == id) out.push_back(i);
    return out;
}

std::string SearchText(const Data& data, int id, const std::string& lang) {
    const Area* a = FindArea(data, id);
    if (a == nullptr) return std::string();
    std::vector<std::string> parts;
    auto add = [&](const std::string& s) {
        if (!s.empty() && std::find(parts.begin(), parts.end(), s) == parts.end()) parts.push_back(s);
    };
    add(NameIn(*a, lang));
    add(NameIn(*a, "enUS"));
    add(a->officialName);
    for (const std::string& b : a->bosses) add(b);
    for (const Quest& q : a->quests) {
        add(q.quest);
        if (!q.item.empty()) add(ItemName(data, q.item, lang));
    }
    std::string out;
    for (const std::string& p : parts) {
        if (!out.empty()) out += '\n';
        out += p;
    }
    return out;
}

// ---- layout ---------------------------------------------------------------------------------------------------

std::vector<Node> LayoutNodes(const Data& data, const Tab& tab) {
    std::vector<std::size_t> order(tab.stations.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const Station& x = tab.stations[a];
        const Station& y = tab.stations[b];
        return x.row != y.row ? x.row < y.row : x.col < y.col;
    });
    std::vector<Node> nodes;
    for (std::size_t si : order) {
        const Station& st = tab.stations[si];
        for (std::size_t m = 0; m < st.ids.size(); ++m) {
            Node n;
            n.id = st.ids[m];
            n.station = si;
            n.member = static_cast<int>(m);
            n.ghost = st.ghost;
            n.at.x = st.col;
            n.at.y = st.row + (st.stack ? static_cast<double>(m) * kStackPitchCells : 0.0);
            if (!st.ghost)
                for (const Area& a : data.areas)
                    if (a.twinOf == n.id) n.folded.push_back(a.id);
            nodes.push_back(n);
            if (st.ghost) break;
        }
    }
    return nodes;
}

int NodeOf(const Data& data, const std::vector<Node>& nodes, int areaId) {
    int id = areaId;
    if (const Area* a = FindArea(data, areaId); a != nullptr && a->twinOf != 0) id = a->twinOf;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].id == id && !nodes[i].ghost) return static_cast<int>(i);
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].id == id) return static_cast<int>(i);
    return -1;
}

std::vector<Neighbours> NeighbourTable(const std::vector<Node>& nodes) {
    const double sx = kCellW / kCellH;  // measure in pixel proportions
    std::vector<Neighbours> out(nodes.size());
    const double dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        for (int d = 0; d < 4; ++d) {
            int best = -1, bestFallback = -1;
            double bestScore = 1e300, bestFallbackScore = 1e300;
            for (std::size_t j = 0; j < nodes.size(); ++j) {
                if (j == i) continue;
                const double vx = (nodes[j].at.x - nodes[i].at.x) * sx;
                const double vy = nodes[j].at.y - nodes[i].at.y;
                const double along = vx * dirs[d][0] + vy * dirs[d][1];
                const double across = std::fabs(vx * dirs[d][1] - vy * dirs[d][0]);
                if (along <= 1e-6) continue;
                const double score = along + 2 * across;
                if (across <= along + 1e-9) {
                    if (score < bestScore) { bestScore = score; best = static_cast<int>(j); }
                } else if (score < bestFallbackScore) {
                    bestFallbackScore = score;
                    bestFallback = static_cast<int>(j);
                }
            }
            const int pick = best >= 0 ? best : bestFallback;
            if (d == 0) out[i].left = pick;
            else if (d == 1) out[i].right = pick;
            else if (d == 2) out[i].up = pick;
            else out[i].down = pick;
        }
    }
    return out;
}

std::vector<Point> RoutePoints(Point a, Point b, bool diagonalFirst) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double adx = std::fabs(dx), ady = std::fabs(dy);
    const double eps = 1e-6 * (1 + adx + ady);
    if (adx < eps || ady < eps || std::fabs(adx - ady) < eps) return {a, b};
    const double d = std::min(adx, ady);
    const double diagX = (dx > 0 ? 1 : -1) * d;
    const double diagY = (dy > 0 ? 1 : -1) * d;
    const Point mid = diagonalFirst ? Point{a.x + diagX, a.y + diagY} : Point{b.x - diagX, b.y - diagY};
    return {a, mid, b};
}

std::vector<DrawnEdge> TabEdges(const Data& data, const Tab& tab, const std::vector<Node>& nodes, double cellW,
                                double cellH) {
    std::vector<DrawnEdge> out;
    if (!tab.grid || cellW <= 0 || cellH <= 0) return out;
    std::set<std::pair<int, int>> seen;
    for (std::size_t i = 0; i < data.edges.size(); ++i) {
        const Edge& e = data.edges[i];
        const int a = NodeOf(data, nodes, e.from);
        const int b = NodeOf(data, nodes, e.to);
        if (a < 0 || b < 0 || a == b) continue;
        if (nodes[a].ghost && nodes[b].ghost) continue;
        if (nodes[a].station == nodes[b].station) continue;  // a stack's own levels: the pill shows them
        const std::pair<int, int> key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
        if (!seen.insert(key).second) continue;
        // the hint keyed from -> to routes from `from`; keyed to -> from routes from `to`
        bool diag = false, reverse = false;
        const int fromId = nodes[a].id, toId = nodes[b].id;
        for (const RouteHint& h : tab.routes) {
            if (h.from == fromId && h.to == toId) { diag = h.diagonalFirst; reverse = false; break; }
            if (h.from == toId && h.to == fromId) { diag = h.diagonalFirst; reverse = true; break; }
        }
        const Point pa{nodes[a].at.x * cellW, nodes[a].at.y * cellH};
        const Point pb{nodes[b].at.x * cellW, nodes[b].at.y * cellH};
        std::vector<Point> pts = reverse ? RoutePoints(pb, pa, diag) : RoutePoints(pa, pb, diag);
        if (reverse) std::reverse(pts.begin(), pts.end());
        for (Point& p : pts) {
            p.x /= cellW;
            p.y /= cellH;
        }
        DrawnEdge de;
        de.edge = i;
        de.fromNode = a;
        de.toNode = b;
        de.points = std::move(pts);
        out.push_back(std::move(de));
    }
    return out;
}

}  // namespace esrwiki::campaign
