// Label placement for the Campaign Progression map's graph tabs (Act I-V), in pixels and without any Win32: the
// renderer (overlay_campaign.inc) measures the texts, this file decides where every station's label goes, and the test
// (tests/campaign_labels_test.cpp) runs it on the real data with made-up text sizes.
//
// Pixel space: the graph's top left is (0, 0); a cell point maps to pixels with PixelX / PixelY at scale s (the
// formula in campaign_data.hpp). Every size handed in is the size of the drawn box, the backing pill included.
//
// The placer (PlaceLabels) is greedy in a stable order (towns first, then by the number of edges, then reading order).
// For every label it tries a fixed set of positions round its dot (or its stack's pill): below, above, right, left, the
// four corners, beside the pill for a stack, each also a little further out, and below / above also shifted sideways.
// A position costs
//     1000 x the area it overlaps hard obstacles (labels placed before, dots with their rings, stack pills, the stacks'
//            level labels, the marker slots, region captions, and whatever lies outside the graph)
//   +   10 x the length of edge lines running through it
//   +   a small preference for the nearer and the earlier positions (the reference's default, below, comes first),
// and the cheapest wins. Repair rounds then place every label again against all the others, and a label still over a
// hard obstacle tries each of its positions together with moving the labels that position covers (kept when the whole
// map gets cheaper). The room above the graph's top may be used (topRoom; the drawing then moves the graph down by
// GraphTop). The time is about 2 ms for the largest tab. PlaceLabelsReference is the reference design's placer as it was
// ported before (kept for the comparison in the test). Platform-neutral.
#pragma once

#include <cstddef>
#include <vector>

#include "campaign_data.hpp"

namespace esrwiki::campaign {

struct Box {
    double x = 0, y = 0, w = 0, h = 0;
    double Right() const { return x + w; }
    double Bottom() const { return y + h; }
};

struct Extent {
    double w = 0, h = 0;
};

inline double PixelX(double cellX, double s) { return (kMarginX + cellX * kCellW) * s; }
inline double PixelY(double cellY, double s) { return (kMarginY + cellY * kCellH) * s; }

double OverlapArea(const Box& a, const Box& b);
// The length of the segment a-b that lies inside `box`.
double SegmentLengthInBox(Point a, Point b, const Box& box);
// The distance from p to the nearest point of the polyline (a single point counts as a polyline).
double DistanceToPolyline(Point p, const std::vector<Point>& line);
bool CircleHitsBox(Point centre, double radius, const Box& box);

// ---- the shared geometry of the drawing ------------------------------------------------------------------------

// The dot radius and the outer edge of the waypoint ring at scale s.
inline double DotRadius(double s) { return kDotR * s; }
inline double RingRadius(double s) { return (kDotR + 4.5) * s; }
// The pill round a dungeon stack whose first and last dots are given.
Box StackPill(Point first, Point last, double s);
// The level label ("L3") right of a stack member's dot, `size` being its box.
Box MemberLabelBox(Point dot, Extent size, double s);

// Boss, quest item and caveat marks (in that order) of an area.
bool MarkBoss(const Area& area);
bool MarkQuestItem(const Area& area);
bool MarkCaveat(const Area& area);
int MarkCount(const Area& area);

// Where an area's marks go: the centre of the first mark, then one every `pitch` to the right. `k` is the mark scale
// (1 = the reference's size); a mark reaches `half` from its centre.
struct MarkerSlot {
    Point first;
    double k = 1, pitch = 9, half = 4;
};
inline double MarkerScale(double s) { return 1.5 * s; }  // the reference's marks were 1.15; about 30% larger
// A plain dot: at its top right, just outside the waypoint ring.
MarkerSlot DotMarkerSlot(Point dot, double s);
// A stack member: on its row, right of `rightOf` (the level label's right edge, or the pill's).
MarkerSlot MemberMarkerSlot(Point dot, double rightOf, double s);
Box MarkerBox(const MarkerSlot& slot, int count);

// ---- the scene -------------------------------------------------------------------------------------------------

// One label per grid cell: nodes [first, last] (a stack's members, or one dot).
struct LabelGroup {
    std::size_t first = 0, last = 0;
    bool stack = false;  // several dots in a pill (a ghost is never drawn as a stack)
    bool ghost = false;
};
std::vector<LabelGroup> LabelGroups(const std::vector<Node>& nodes);

// What the caller measured: per group (LabelGroups order) the label's box, per node the level label's box (w = 0: none),
// per region of the tab its caption's box, and where the caption sits inside the region's top left corner.
struct LabelSceneText {
    std::vector<Extent> labels;
    std::vector<Extent> members;
    std::vector<Extent> captions;
    double captionInsetX = 6, captionInsetY = 3;  // pixels
};

struct LabelScene {
    double scale = 1;
    double width = 0, height = 0;  // the graph's bounds; a label must stay inside them
    double topRoom = 0;            // ... or use this much room above them (the graph then grows upwards: GraphTop)
    double dotR = 0, ringR = 0;
    double gap = 2;                // between a label and what it belongs to
    std::vector<Point> dots;       // one per node
    std::vector<LabelGroup> groups;
    std::vector<Extent> sizes;     // per group
    std::vector<Box> anchors;      // per group: its dot (to the ring) or its stack's pill
    std::vector<double> sideRight; // per group: where the room right of it starts (a stack: past its level labels and marks)
    std::vector<double> memberSpan;  // per group: the widest level label (the reference placer's corner offset)
    std::vector<int> priority;     // per group: towns 1, else 0
    std::vector<int> degree;       // per group: the edges touching it
    std::vector<Box> pills;        // the stacks' pills
    std::vector<Box> members;      // per node: its level label (w = 0: none)
    std::vector<MarkerSlot> markerSlots;  // per node
    std::vector<int> markerCounts;        // per node (0 for a ghost)
    std::vector<Box> markers;      // the marker boxes of the nodes that have marks
    std::vector<Box> captions;     // per region
    std::vector<Box> regions;      // per region: its box
    std::vector<std::vector<Point>> edges;  // the drawn edges' polylines
};

// The scene of a grid tab at scale s; `edges` are TabEdges(data, tab, nodes) (cell units).
LabelScene MakeLabelScene(const Data& data, const Tab& tab, const std::vector<Node>& nodes, const std::vector<DrawnEdge>& edges,
                          double s, const LabelSceneText& text);

// The label box of every group (LabelGroups order).
std::vector<Box> PlaceLabels(const LabelScene& scene);
std::vector<Box> PlaceLabelsReference(const LabelScene& scene);

struct LabelStats {
    int labelOverlaps = 0;    // pairs of labels
    int dotOverlaps = 0;      // a label over a dot (the dot itself, not its rings)
    int pillOverlaps = 0;
    int captionOverlaps = 0;
    int fixedOverlaps = 0;    // level labels and marker slots
    int outside = 0;          // labels leaving the graph
    double edgeCrossing = 0;  // the length of edge lines under labels, in pixels
};
// How far the labels reach above the graph's top (0 when they do not): the drawing moves everything down by this.
double GraphTop(const std::vector<Box>& labels);
LabelStats ScoreLabels(const LabelScene& scene, const std::vector<Box>& labels);

}  // namespace esrwiki::campaign
