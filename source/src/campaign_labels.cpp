#include "campaign_labels.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace esrwiki::campaign {

double OverlapArea(const Box& a, const Box& b) {
    const double w = std::min(a.Right(), b.Right()) - std::max(a.x, b.x);
    const double h = std::min(a.Bottom(), b.Bottom()) - std::max(a.y, b.y);
    return w > 0 && h > 0 ? w * h : 0;
}

// Liang-Barsky clipping of a-b against the box; the length of what is left.
double SegmentLengthInBox(Point a, Point b, const Box& box) {
    double t0 = 0, t1 = 1;
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a.x - box.x, box.Right() - a.x, a.y - box.y, box.Bottom() - a.y};
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(p[i]) < 1e-12) {
            if (q[i] <= 0) return 0;  // parallel to this side and outside (or on) it
            continue;
        }
        const double t = q[i] / p[i];
        if (p[i] < 0) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
        if (t0 >= t1) return 0;
    }
    return (t1 - t0) * std::hypot(dx, dy);
}

double DistanceToPolyline(Point p, const std::vector<Point>& line) {
    if (line.empty()) return 1e300;
    double best = std::hypot(p.x - line[0].x, p.y - line[0].y);
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const Point a = line[i], b = line[i + 1];
        const double dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
        double t = length2 > 1e-12 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2 : 0;
        t = std::clamp(t, 0.0, 1.0);
        best = std::min(best, std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy)));
    }
    return best;
}

bool CircleHitsBox(Point centre, double radius, const Box& box) {
    const double nx = std::clamp(centre.x, box.x, box.Right());
    const double ny = std::clamp(centre.y, box.y, box.Bottom());
    return (centre.x - nx) * (centre.x - nx) + (centre.y - ny) * (centre.y - ny) < radius * radius;
}

Box StackPill(Point first, Point last, double s) {
    const double pad = DotRadius(s) + 4 * s;
    return Box{first.x - pad, first.y - pad, pad * 2, last.y - first.y + pad * 2};
}

Box MemberLabelBox(Point dot, Extent size, double s) {
    return Box{dot.x + DotRadius(s) + 5 * s, dot.y - size.h / 2, size.w, size.h};
}

bool MarkBoss(const Area& area) { return !area.bosses.empty(); }

bool MarkQuestItem(const Area& area) {
    return std::any_of(area.quests.begin(), area.quests.end(), [](const Quest& quest) { return !quest.item.empty(); });
}

bool MarkCaveat(const Area& area) {
    for (const Note& note : area.notes)
        if (note.warning) return true;
    return area.teleportOff || area.teleportLimited || area.teleportWalls || area.noTownPortal || area.entryUnknown;
}

int MarkCount(const Area& area) {
    return (MarkBoss(area) ? 1 : 0) + (MarkQuestItem(area) ? 1 : 0) + (MarkCaveat(area) ? 1 : 0);
}

MarkerSlot DotMarkerSlot(Point dot, double s) {
    MarkerSlot slot;
    slot.k = MarkerScale(s);
    slot.pitch = 9 * slot.k;
    slot.half = 4 * slot.k;
    const double diagonal = RingRadius(s) * 0.7071;  // the ring's top right
    slot.first = Point{dot.x + diagonal + 4.5 * slot.k, dot.y - diagonal - 4 * slot.k};
    return slot;
}

MarkerSlot MemberMarkerSlot(Point dot, double rightOf, double s) {
    MarkerSlot slot;
    slot.k = MarkerScale(s);
    slot.pitch = 9 * slot.k;
    slot.half = 4 * slot.k;
    slot.first = Point{rightOf + 2 * s + slot.half, dot.y};
    return slot;
}

Box MarkerBox(const MarkerSlot& slot, int count) {
    if (count <= 0) return Box{slot.first.x, slot.first.y, 0, 0};
    return Box{slot.first.x - slot.half, slot.first.y - slot.half, (count - 1) * slot.pitch + 2 * slot.half, 2 * slot.half};
}

std::vector<LabelGroup> LabelGroups(const std::vector<Node>& nodes) {
    std::vector<LabelGroup> groups;
    for (std::size_t i = 0; i < nodes.size();) {
        std::size_t j = i + 1;
        while (j < nodes.size() && nodes[j].station == nodes[i].station) ++j;
        LabelGroup group;
        group.first = i;
        group.last = j - 1;
        group.ghost = nodes[i].ghost;
        group.stack = group.last > group.first && !group.ghost;
        groups.push_back(group);
        i = j;
    }
    return groups;
}

LabelScene MakeLabelScene(const Data& data, const Tab& tab, const std::vector<Node>& nodes, const std::vector<DrawnEdge>& edges,
                          double s, const LabelSceneText& text) {
    LabelScene scene;
    scene.scale = s;
    scene.dotR = DotRadius(s);
    scene.ringR = RingRadius(s);
    scene.gap = 2 * s;
    int maxCol = std::max(0, tab.cols - 1);
    for (const Station& station : tab.stations) maxCol = std::max(maxCol, station.col);
    scene.width = std::ceil((2 * kMarginX + maxCol * kCellW) * s);
    double maxY = 0;
    for (const Node& node : nodes) {
        scene.dots.push_back(Point{PixelX(node.at.x, s), PixelY(node.at.y, s)});
        maxY = std::max(maxY, node.at.y);
    }
    // The graph grows downwards to fit its labels: the room under the last row is most of a row.
    scene.height = PixelY(maxY, s) + std::max(kMarginY, 0.75 * kCellH) * s;
    // The room above the graph fits at least a row's share, and always the tallest label (plus its gap).
    double maxLabelH = 0;
    for (const Extent& label : text.labels) maxLabelH = std::max(maxLabelH, label.h);
    scene.topRoom = std::max(std::round(0.35 * kCellH * s), std::ceil(maxLabelH + scene.gap));
    for (std::size_t i = 0; i < tab.regions.size(); ++i) {
        const Region& region = tab.regions[i];
        const Box box{PixelX(region.x0, s), PixelY(region.y0, s), PixelX(region.x1, s) - PixelX(region.x0, s),
                      PixelY(region.y1, s) - PixelY(region.y0, s)};
        scene.regions.push_back(box);
        const Extent caption = i < text.captions.size() ? text.captions[i] : Extent{};
        scene.captions.push_back(Box{std::round(box.x + text.captionInsetX), std::round(box.y + text.captionInsetY), caption.w, caption.h});
        scene.height = std::max(scene.height, box.Bottom() + 2 * s);
    }
    for (const DrawnEdge& edge : edges) {
        std::vector<Point> line;
        for (const Point& p : edge.points) line.push_back(Point{PixelX(p.x, s), PixelY(p.y, s)});
        scene.edges.push_back(std::move(line));
    }

    scene.groups = LabelGroups(nodes);
    std::vector<std::size_t> groupOf(nodes.size(), 0);
    for (std::size_t gi = 0; gi < scene.groups.size(); ++gi)
        for (std::size_t k = scene.groups[gi].first; k <= scene.groups[gi].last; ++k) groupOf[k] = gi;
    scene.members.assign(nodes.size(), Box{});
    scene.markerSlots.assign(nodes.size(), MarkerSlot{});
    scene.markerCounts.assign(nodes.size(), 0);
    for (std::size_t gi = 0; gi < scene.groups.size(); ++gi) {
        const LabelGroup& group = scene.groups[gi];
        const Point first = scene.dots[group.first], last = scene.dots[group.last];
        Box anchor{first.x - scene.ringR, first.y - scene.ringR, scene.ringR * 2, scene.ringR * 2};
        double side = anchor.Right();
        double span = 0;
        if (group.stack) {
            anchor = StackPill(first, last, s);
            scene.pills.push_back(anchor);
            side = anchor.Right();
        }
        for (std::size_t k = group.first; k <= group.last; ++k) {
            const Area* area = FindArea(data, nodes[k].id);
            if (group.stack) {
                double rightOf = anchor.Right();
                if (k < text.members.size() && text.members[k].w > 0) {
                    scene.members[k] = MemberLabelBox(scene.dots[k], text.members[k], s);
                    rightOf = scene.members[k].Right();
                    span = std::max(span, text.members[k].w);
                    side = std::max(side, rightOf);
                }
                scene.markerSlots[k] = MemberMarkerSlot(scene.dots[k], rightOf, s);
            } else {
                scene.markerSlots[k] = DotMarkerSlot(scene.dots[k], s);
            }
            if (area != nullptr && !group.ghost) scene.markerCounts[k] = MarkCount(*area);
            if (scene.markerCounts[k] > 0) {
                const Box marks = MarkerBox(scene.markerSlots[k], scene.markerCounts[k]);
                scene.markers.push_back(marks);
                if (group.stack) side = std::max(side, marks.Right());
            }
        }
        const Area* head = FindArea(data, nodes[group.first].id);
        scene.anchors.push_back(anchor);
        scene.sideRight.push_back(side);
        scene.memberSpan.push_back(span);
        scene.priority.push_back(head != nullptr && head->town && !group.ghost ? 1 : 0);
        scene.sizes.push_back(gi < text.labels.size() ? text.labels[gi] : Extent{});
    }
    scene.degree.assign(scene.groups.size(), 0);
    for (const DrawnEdge& edge : edges) {
        if (edge.fromNode >= 0 && static_cast<std::size_t>(edge.fromNode) < nodes.size()) ++scene.degree[groupOf[static_cast<std::size_t>(edge.fromNode)]];
        if (edge.toNode >= 0 && static_cast<std::size_t>(edge.toNode) < nodes.size()) ++scene.degree[groupOf[static_cast<std::size_t>(edge.toNode)]];
    }
    return scene;
}

namespace {

struct Candidate {
    Box box;
    double preference = 0;
};

// The positions tried for group gi, the reference's default (below) first.
std::vector<Candidate> Candidates(const LabelScene& scene, std::size_t gi) {
    const LabelGroup& group = scene.groups[gi];
    const Box& a = scene.anchors[gi];
    const double w = scene.sizes[gi].w, h = scene.sizes[gi].h, g = scene.gap, s = scene.scale;
    const double cx = a.x + a.w / 2, cy = a.y + a.h / 2;
    const double right = group.stack ? scene.sideRight[gi] + g : a.Right() + g;
    struct Base { double x, y, dx, dy; };  // a position and the direction "further out" moves it
    std::vector<Base> bases = {
        {cx - w / 2, a.Bottom() + g, 0, 1},             // below
        {cx - w / 2, a.y - g - h, 0, -1},               // above
        {right, cy - h / 2, 1, 0},                      // right
        {a.x - g - w, cy - h / 2, -1, 0},               // left
        {a.Right() - g, a.Bottom() + g, 0.7, 0.7},      // below right
        {a.x + g - w, a.Bottom() + g, -0.7, 0.7},       // below left
        {a.Right() - g, a.y - g - h, 0.7, -0.7},        // above right
        {a.x + g - w, a.y - g - h, -0.7, -0.7},         // above left
    };
    if (group.stack) {
        bases.push_back({right, a.y, 1, 0});            // beside the pill, at its top
        bases.push_back({right, a.Bottom() - h, 1, 0}); // beside the pill, at its bottom
        bases.push_back({a.x - g - w, a.y, -1, 0});
        bases.push_back({a.x - g - w, a.Bottom() - h, -1, 0});
    }
    std::vector<Candidate> out;
    const double step = 7 * s;
    for (std::size_t b = 0; b < bases.size(); ++b) {
        for (int reach = 0; reach < 4; ++reach) {
            const double x = bases[b].x + bases[b].dx * step * reach, y = bases[b].y + bases[b].dy * step * reach;
            const double rank = static_cast<double>(b) * 0.5 + reach * 6.0;
            out.push_back({Box{std::round(x), std::round(y), w, h}, rank});
            if (b < 2) {  // below and above: also shifted sideways
                for (const double shift : {-0.3, 0.3, -0.5, 0.5}) {
                    out.push_back({Box{std::round(x + shift * w), std::round(y), w, h}, rank + 3 + std::fabs(shift) * 6});
                }
            } else if (b < 4) {  // right and left: also shifted up and down
                for (const double shift : {-0.5, 0.5}) {
                    out.push_back({Box{std::round(x), std::round(y + shift * h), w, h}, rank + 3});
                }
            }
        }
    }
    return out;
}

struct Costs {
    double hard = 0, crossing = 0;
};

// What a position costs apart from the other labels (it never changes, so it is worked out once).
Costs FixedCost(const LabelScene& scene, const Box& box, std::size_t self) {
    Costs c;
    const Box bounds{0, -scene.topRoom, scene.width, scene.height + scene.topRoom};
    c.hard += box.w * box.h - OverlapArea(box, bounds);
    if (box.y < 0) c.crossing += -box.y * 2;  // the room above the graph is there, but the graph grows to use it
    const LabelGroup& own = scene.groups[self];
    for (std::size_t k = 0; k < scene.dots.size(); ++k) {
        const double rr = scene.ringR;
        const bool mine = k >= own.first && k <= own.last;
        if (mine && own.stack) continue;  // inside its own pill, which counts already
        c.hard += OverlapArea(box, Box{scene.dots[k].x - rr, scene.dots[k].y - rr, rr * 2, rr * 2});
    }
    for (const Box& pill : scene.pills) c.hard += OverlapArea(box, pill);
    for (const Box& member : scene.members)
        if (member.w > 0) c.hard += OverlapArea(box, member);
    for (const Box& marks : scene.markers) c.hard += OverlapArea(box, marks);
    for (const Box& caption : scene.captions) c.hard += OverlapArea(box, caption);
    for (const std::vector<Point>& line : scene.edges)
        for (std::size_t i = 0; i + 1 < line.size(); ++i) c.crossing += SegmentLengthInBox(line[i], line[i + 1], box);
    return c;
}

double Total(const Costs& c, double preference) { return c.hard * 1000 + c.crossing * 10 + preference; }

// The overlap of a position with the labels placed so far.
double LabelCost(const Box& box, std::size_t self, const std::vector<Box>& placed, const std::vector<char>& isPlaced) {
    double area = 0;
    for (std::size_t i = 0; i < placed.size(); ++i)
        if (i != self && isPlaced[i]) area += OverlapArea(box, placed[i]);
    return area;
}

}  // namespace

std::vector<Box> PlaceLabels(const LabelScene& scene) {
    const std::size_t n = scene.groups.size();
    std::vector<Box> placed(n);
    std::vector<char> isPlaced(n, 0);
    if (n == 0) return placed;
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (scene.priority[a] != scene.priority[b]) return scene.priority[a] > scene.priority[b];
        return scene.degree[a] > scene.degree[b];
    });
    std::vector<std::vector<Candidate>> candidates(n);
    std::vector<std::vector<double>> fixedTotal(n);
    std::vector<std::vector<double>> fixedHard(n);
    for (std::size_t gi = 0; gi < n; ++gi) {
        candidates[gi] = Candidates(scene, gi);
        for (const Candidate& candidate : candidates[gi]) {
            const Costs c = FixedCost(scene, candidate.box, gi);
            fixedTotal[gi].push_back(Total(c, candidate.preference));
            fixedHard[gi].push_back(c.hard);
        }
    }
    std::vector<std::size_t> chosen(n, 0);
    const auto totalOf = [&](std::size_t gi, std::size_t c) {
        return fixedTotal[gi][c] + 1000 * LabelCost(candidates[gi][c].box, gi, placed, isPlaced);
    };
    const auto cheapest = [&](std::size_t gi) {
        std::size_t pick = 0;
        double pickTotal = totalOf(gi, 0);
        for (std::size_t c = 1; c < candidates[gi].size(); ++c) {
            const double total = totalOf(gi, c);
            if (total < pickTotal - 1e-9) {
                pick = c;
                pickTotal = total;
            }
        }
        return pick;
    };
    for (const std::size_t gi : order) {
        chosen[gi] = cheapest(gi);
        placed[gi] = candidates[gi][chosen[gi]].box;
        isPlaced[gi] = 1;
    }
    // Repair: every label once more against all the others; it moves only when that is cheaper.
    for (int round = 0; round < 3; ++round) {
        bool moved = false;
        for (const std::size_t gi : order) {
            const std::size_t next = cheapest(gi);
            if (next != chosen[gi] && totalOf(gi, next) < totalOf(gi, chosen[gi]) - 1e-9) {
                chosen[gi] = next;
                placed[gi] = candidates[gi][next].box;
                moved = true;
            }
        }
        if (!moved) break;
    }
    // Labels still over a hard obstacle: try each of their positions together with moving the labels that position would
    // cover (each to its cheapest place); kept when the whole map gets cheaper.
    const auto hardOf = [&](std::size_t gi) { return fixedHard[gi][chosen[gi]] + LabelCost(placed[gi], gi, placed, isPlaced); };
    const auto mapTotal = [&]() {
        double sum = 0;
        for (std::size_t gi = 0; gi < n; ++gi) sum += totalOf(gi, chosen[gi]);
        return sum;
    };
    for (int round = 0; round < 3; ++round) {
        bool improved = false;
        for (const std::size_t gi : order) {
            if (hardOf(gi) <= 0) continue;
            double bestTotal = mapTotal();
            std::vector<std::size_t> bestChosen = chosen;
            const std::vector<std::size_t> start = chosen;
            for (std::size_t c = 0; c < candidates[gi].size(); ++c) {
                chosen = start;
                for (std::size_t k = 0; k < n; ++k) placed[k] = candidates[k][chosen[k]].box;
                chosen[gi] = c;
                placed[gi] = candidates[gi][c].box;
                for (const std::size_t other : order) {
                    if (other == gi || OverlapArea(placed[other], placed[gi]) <= 0) continue;
                    chosen[other] = cheapest(other);
                    placed[other] = candidates[other][chosen[other]].box;
                }
                const double total = mapTotal();
                if (total < bestTotal - 1e-6) {
                    bestTotal = total;
                    bestChosen = chosen;
                }
            }
            chosen = bestChosen;
            for (std::size_t k = 0; k < n; ++k) placed[k] = candidates[k][chosen[k]].box;
            if (chosen != start) improved = true;
        }
        if (!improved) break;
    }
    return placed;
}

std::vector<Box> PlaceLabelsReference(const LabelScene& scene) {
    const double s = scene.scale, r = scene.dotR, labelGap = 4 * s;
    std::vector<Box> boxes = scene.captions;
    for (const Box& member : scene.members)
        if (member.w > 0) boxes.push_back(member);
    std::vector<Box> out;
    for (std::size_t gi = 0; gi < scene.groups.size(); ++gi) {
        const LabelGroup& group = scene.groups[gi];
        const double w = scene.sizes[gi].w, h = scene.sizes[gi].h;
        const Point first = scene.dots[group.first], last = scene.dots[group.last];
        const double cx = first.x, cy = (first.y + last.y) / 2;
        const double below = last.y + r + labelGap, above = first.y - r - labelGap - h;
        const double mw = group.stack ? scene.memberSpan[gi] + 6 * s : 0;
        std::vector<Box> candidates = {{cx - w / 2, below, w, h}};
        if (!group.stack) candidates.push_back({cx + r + labelGap + 2 * s, cy - h / 2, w, h});
        candidates.push_back({cx - w / 2, above, w, h});
        candidates.push_back({cx - r - labelGap - 2 * s - w, cy - h / 2, w, h});
        candidates.push_back({cx + 3 * s + mw, below - 2 * s, w, h});
        candidates.push_back({cx - 3 * s - w, below - 2 * s, w, h});
        candidates.push_back({cx + 3 * s + mw, above + 2 * s, w, h});
        candidates.push_back({cx - 3 * s - w, above + 2 * s, w, h});
        Box chosen = candidates[0];
        int bestScore = -1;
        for (Box box : candidates) {
            box.x = std::round(box.x);
            box.y = std::round(box.y);
            const Box padded{box.x - 1, box.y - 1, box.w + 2, box.h + 2};
            int score = 0;
            if (box.x < 2 || box.Right() > scene.width - 2 || box.y < 2) score += 10;
            for (const std::vector<Point>& line : scene.edges)
                for (std::size_t k = 0; k + 1 < line.size(); ++k)
                    if (SegmentLengthInBox(line[k], line[k + 1], padded) > 0) score += 1;
            for (std::size_t k = 0; k < scene.dots.size(); ++k) {
                const bool own = k >= group.first && k <= group.last;
                if (CircleHitsBox(scene.dots[k], own ? r + 1 : r + 2, padded)) score += 2;
            }
            for (const Box& other : boxes)
                if (OverlapArea(other, padded) > 0) score += 3;
            if (bestScore < 0 || score < bestScore) {
                chosen = box;
                bestScore = score;
                if (score == 0) break;
            }
        }
        boxes.push_back(chosen);
        out.push_back(chosen);
    }
    return out;
}

double GraphTop(const std::vector<Box>& labels) {
    double top = 0;
    for (const Box& box : labels) top = std::min(top, box.y);
    return -top;
}

LabelStats ScoreLabels(const LabelScene& scene, const std::vector<Box>& labels) {
    LabelStats stats;
    const Box bounds{0, -scene.topRoom, scene.width, scene.height + scene.topRoom};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const Box& box = labels[i];
        for (std::size_t j = i + 1; j < labels.size(); ++j)
            if (OverlapArea(box, labels[j]) > 0) ++stats.labelOverlaps;
        for (const Point& dot : scene.dots)
            if (CircleHitsBox(dot, scene.dotR, box)) ++stats.dotOverlaps;
        for (const Box& pill : scene.pills)
            if (OverlapArea(box, pill) > 0) ++stats.pillOverlaps;
        for (const Box& caption : scene.captions)
            if (OverlapArea(box, caption) > 0) ++stats.captionOverlaps;
        for (const Box& member : scene.members)
            if (member.w > 0 && OverlapArea(box, member) > 0) ++stats.fixedOverlaps;
        for (const Box& marks : scene.markers)
            if (OverlapArea(box, marks) > 0) ++stats.fixedOverlaps;
        if (OverlapArea(box, bounds) < box.w * box.h - 1e-9) ++stats.outside;
        for (const std::vector<Point>& line : scene.edges)
            for (std::size_t k = 0; k + 1 < line.size(); ++k) stats.edgeCrossing += SegmentLengthInBox(line[k], line[k + 1], box);
    }
    return stats;
}

}  // namespace esrwiki::campaign
