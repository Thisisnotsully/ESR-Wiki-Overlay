// Tests for the Campaign Progression map's label placer (campaign_labels.cpp). Run with the path of
// data/campaign-map.txt as the argument. Every act tab is laid out at the scales 0.75, 1 and 1.5 with made-up text sizes
// (7 px a character and 14 px a line for the labels, 6 and 12 for the small font); no label may overlap another label,
// a dot, a stack's pill or a region caption, and the edge lines under the labels must be shorter than with the
// reference's placer.
#include "campaign_labels.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

using esrwiki::campaign::Extent;

// Greedy word wrap at `wrap` pixels with a fixed width per character; the box of the wrapped text.
Extent Measure(const std::string& text, double wrap, double charW, double lineH) {
    if (text.empty()) return Extent{};
    std::vector<std::string> words;
    std::istringstream in(text);
    for (std::string word; in >> word;) words.push_back(word);
    double widest = 0;
    int lines = 0;
    std::size_t line = 0;  // characters on the current line
    for (const std::string& word : words) {
        const std::size_t need = line == 0 ? word.size() : line + 1 + word.size();
        if (line > 0 && static_cast<double>(need) * charW > wrap) {
            widest = std::max(widest, static_cast<double>(line) * charW);
            ++lines;
            line = word.size();
        } else {
            line = need;
        }
    }
    widest = std::max(widest, static_cast<double>(line) * charW);
    ++lines;
    return Extent{widest, lines * lineH};
}

Extent Padded(Extent e) { return Extent{e.w + 6, e.h + 2}; }

// The shared start of a stack's names without the level numbers ("Tower Cellar Level 1".. gives "Tower Cellar").
std::string StackBase(const std::vector<std::string>& names) {
    std::string base = names.empty() ? std::string() : names[0];
    for (const std::string& name : names) {
        std::size_t same = 0;
        while (same < base.size() && same < name.size() && base[same] == name[same]) ++same;
        base.resize(same);
    }
    while (!base.empty() && (base.back() == ' ' || (base.back() >= '0' && base.back() <= '9') || base.back() == '(' || base.back() == '-'))
        base.pop_back();
    if (base.size() > 6 && base.compare(base.size() - 6, 6, " Level") == 0) base.resize(base.size() - 6);
    return base.empty() && !names.empty() ? names[0] : base;
}

std::string LevelNumber(const std::string& name) {
    std::size_t end = name.size();
    while (end > 0 && !(name[end - 1] >= '0' && name[end - 1] <= '9')) --end;
    std::size_t start = end;
    while (start > 0 && name[start - 1] >= '0' && name[start - 1] <= '9') --start;
    return name.substr(start, end - start);
}
}  // namespace

int main(int argc, char** argv) {
    using namespace esrwiki::campaign;

    // Geometry helpers.
    Check(std::fabs(SegmentLengthInBox({0, 5}, {20, 5}, Box{5, 0, 10, 10}) - 10) < 1e-9, "horizontal segment through a box");
    Check(SegmentLengthInBox({0, 20}, {20, 20}, Box{5, 0, 10, 10}) == 0, "segment below a box");
    Check(std::fabs(SegmentLengthInBox({0, 0}, {10, 10}, Box{0, 0, 10, 10}) - std::hypot(10.0, 10.0)) < 1e-9, "diagonal of a box");
    Check(SegmentLengthInBox({0, 0}, {10, 0}, Box{0, 0, 10, 10}) == 0, "segment along the top edge counts nothing");
    Check(std::fabs(DistanceToPolyline({5, 3}, {{0, 0}, {10, 0}}) - 3) < 1e-9, "distance to a segment");
    Check(std::fabs(DistanceToPolyline({13, 4}, {{0, 0}, {10, 0}}) - 5) < 1e-9, "distance past its end");
    Check(OverlapArea(Box{0, 0, 10, 10}, Box{10, 0, 5, 5}) == 0 && OverlapArea(Box{0, 0, 10, 10}, Box{5, 5, 10, 10}) == 25, "overlap area");
    {
        const MarkerSlot slot = DotMarkerSlot({100, 100}, 1);
        const Box one = MarkerBox(slot, 1), three = MarkerBox(slot, 3);
        Check(one.x > 100 && one.Bottom() < 100, "the marks sit at the dot's top right");
        Check(!CircleHitsBox({100, 100}, RingRadius(1) - 0.5, one), "the marks stay outside the waypoint ring");
        Check(std::fabs(three.w - one.w - 2 * slot.pitch) < 1e-9, "three marks in a row");
    }

    if (argc < 2) {
        std::cout << (failures == 0 ? "campaign labels: geometry ok (no data file given)\n" : "campaign labels: FAILED\n");
        return failures == 0 ? 0 : 1;
    }
    std::ifstream file(argv[1], std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    Data data;
    std::string error;
    if (!Parse(buffer.str(), data, error)) {
        std::cerr << "FAIL: cannot read " << argv[1] << ": " << error << "\n";
        return 1;
    }

    int tabsSeen = 0;
    double slowest = 0;
    for (const Tab& tab : data.tabs) {
        if (!tab.grid) continue;
        ++tabsSeen;
        const std::vector<Node> nodes = LayoutNodes(data, tab);
        const std::vector<DrawnEdge> edges = TabEdges(data, tab, nodes);
        const std::vector<LabelGroup> groups = LabelGroups(nodes);
        double tabBefore = 0, tabAfter = 0;
        // 0.5 and 0.6 are only reported (overlap counts per tab); the hard assertions stay at 0.75, 1 and 1.5.
        for (const double s : {0.5, 0.6, 0.75, 1.0, 1.5}) {
            const bool hard = s >= 0.7;
            LabelSceneText text;
            const double wrap = std::max(64.0, 1.5 * kCellW * s);
            text.members.assign(nodes.size(), Extent{});
            for (const LabelGroup& group : groups) {
                std::vector<std::string> names;
                for (std::size_t k = group.first; k <= group.last; ++k) names.push_back(AreaName(data, nodes[k].id, "enUS"));
                const Node& node = nodes[group.first];
                if (group.ghost) {
                    const Extent name = Measure(names[0], wrap, 7, 14), info = Measure("-> Act II", 4000, 7, 14);
                    text.labels.push_back(Padded(Extent{std::max(name.w, info.w), name.h + info.h}));
                } else if (group.stack) {
                    text.labels.push_back(Padded(Measure(StackBase(names), wrap, 6, 12)));
                    bool all = true;
                    for (const std::string& name : names) all = all && !LevelNumber(name).empty();
                    if (all)
                        for (std::size_t k = group.first; k <= group.last; ++k)
                            text.members[k] = Padded(Measure("L" + LevelNumber(names[k - group.first]), 4000, 7, 14));
                } else {
                    std::string name = names[0];
                    if (!node.folded.empty()) name += " x" + std::to_string(node.folded.size() + 1);
                    text.labels.push_back(Padded(Measure(name, wrap, 7, 14)));
                }
            }
            for (const Region& region : tab.regions) text.captions.push_back(Padded(Measure(region.label, 4000, 6, 12)));
            text.captionInsetX = 6 * s;
            text.captionInsetY = 3 * s;

            const LabelScene scene = MakeLabelScene(data, tab, nodes, edges, s, text);
            Check(scene.groups.size() == groups.size() && scene.sizes.size() == groups.size(), tab.key + ": one label per station");
            const std::vector<Box> before = PlaceLabelsReference(scene);
            const auto start = std::chrono::steady_clock::now();
            const std::vector<Box> after = PlaceLabels(scene);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            slowest = std::max(slowest, ms);
            const LabelStats was = ScoreLabels(scene, before), now = ScoreLabels(scene, after);
            if (hard) {
                tabBefore += was.edgeCrossing;
                tabAfter += now.edgeCrossing;
            }
            char line[400];
            std::snprintf(line, sizeof line,
                          "%-5s s=%.2f  labels %2zu | reference: overlaps label %d dot %d pill %d caption %d marks %d out %d, "
                          "crossing %6.1f px | new: overlaps label %d dot %d pill %d caption %d marks %d out %d, crossing %6.1f px "
                          "| %.2f ms\n",
                          tab.key.c_str(), s, after.size(), was.labelOverlaps, was.dotOverlaps, was.pillOverlaps, was.captionOverlaps,
                          was.fixedOverlaps, was.outside, was.edgeCrossing, now.labelOverlaps, now.dotOverlaps, now.pillOverlaps,
                          now.captionOverlaps, now.fixedOverlaps, now.outside, now.edgeCrossing, ms);
            std::cout << line;
            const std::string where = tab.key + " at " + std::to_string(s);
            Check(after.size() == groups.size(), where + ": a box per label");
            if (!hard) {
                std::printf("REPORT %-5s s=%.2f  overlaps (not asserted): label %d dot %d pill %d caption %d marks %d out %d\n", tab.key.c_str(), s,
                            now.labelOverlaps, now.dotOverlaps, now.pillOverlaps, now.captionOverlaps, now.fixedOverlaps, now.outside);
                continue;
            }
            if (now.labelOverlaps + now.dotOverlaps + now.pillOverlaps + now.captionOverlaps + now.fixedOverlaps + now.outside > 0) {
                for (std::size_t i = 0; i < after.size(); ++i) {
                    const Box& b = after[i];
                    const LabelStats one = ScoreLabels(scene, std::vector<Box>{b});
                    bool dot = false;
                    for (const Point& p : scene.dots) dot = dot || CircleHitsBox(p, scene.dotR, b);
                    if (dot || one.outside > 0 || one.fixedOverlaps > 0)
                        std::printf("   %-28s %6.0f %6.0f %4.0f x %3.0f  dot %d out %d marks %d (graph %.0f x %.0f)\n",
                                    AreaName(data, nodes[groups[i].first].id, "enUS").c_str(), b.x, b.y, b.w, b.h, dot ? 1 : 0,
                                    one.outside, one.fixedOverlaps, scene.width, scene.height);
                }
            }
            Check(now.labelOverlaps == 0, where + ": no label overlaps another label");
            Check(now.dotOverlaps == 0, where + ": no label covers a dot");
            Check(now.pillOverlaps == 0, where + ": no label covers a stack's pill");
            Check(now.captionOverlaps == 0, where + ": no label covers a region caption");
            Check(now.fixedOverlaps == 0, where + ": no label covers a level label or a mark");
            Check(now.outside == 0, where + ": every label inside the graph");
            Check(was.edgeCrossing > 0 ? now.edgeCrossing < was.edgeCrossing : now.edgeCrossing <= was.edgeCrossing,
                  where + ": fewer edge pixels under the labels than the reference's placer");
            Check(PlaceLabels(scene).size() == after.size() && ScoreLabels(scene, PlaceLabels(scene)).edgeCrossing == now.edgeCrossing,
                  where + ": deterministic");
        }
        std::printf("%-5s edge-over-label crossing, all three scales: %.1f px before, %.1f px after\n", tab.key.c_str(), tabBefore, tabAfter);
        Check(tabAfter < tabBefore || (tabBefore == 0 && tabAfter == 0), tab.key + ": less edge under the labels than before");
    }
    Check(tabsSeen == 5, "five act tabs");
    std::printf("slowest placement: %.2f ms\n", slowest);
    Check(slowest < 50, "placement is quick");  // < 5 ms expected; generous for slow test machines
    std::cout << (failures == 0 ? "campaign labels: all passed\n" : "campaign labels: FAILED\n");
    return failures == 0 ? 0 : 1;
}
