// ---------------------------------------------------------------------------------------------------------------
// The Campaign Progression map: drawn in the detail pane under the text of the Welcome page's "Campaign Progression"
// entry (the entry with the "campaignmap" flag in wiki-data.txt and its translated copies; an English file without the
// flag is recognised by the entry's English title). The data is campaign-map.txt (campaign_data.cpp): seven tabs, five
// act graphs and two card grids, with the areas' names in every game language. This file draws it and answers the mouse,
// the controller and the search. It is included by overlay.cpp, after OverlayState is defined, and uses that file's
// drawing helpers.
//
// Layout, top to bottom (all part of the scrolling entry): a rule and the "Campaign map" heading, the tab strip, the
// graph (or the cards), the tab's notes, and the details of the selected area (or a hint to click one). The graph is
// scaled to fit the pane's width, never larger than the reference design at this screen's scale; the labels keep the
// 9 pt search-snippet font, so they stay readable however small the graph gets.
//
// Graph: a dot per area coloured by terrain (a town is a square), a blue ring for a waypoint, a pill round a dungeon's
// levels, a dashed dot for an area drawn on another tab (clicking it opens that tab). Marks pinned to a dot's top right
// (a stack member's: right of its level label), in this order: a red triangle (boss), an amber diamond (quest item), an
// orange "!" (a caveat). Edges by kind: border thick and faint, passage thin, portal dashed fuchsia, cube dash-dot amber,
// act bold; one-way edges and act edges end in an arrowhead. A key under the act tabs shows the ring, the marks and the
// edge kinds. Drawing order: regions, edges, dots, label backings and texts, marks, highlights.
// Labels (campaign_labels.cpp): one line up to 1.5 cells, on a dark rounded backing so no line runs through them, placed
// where they cover no other label, dot, pill, mark or region caption and as little of the edges as possible. An edge's
// "via" caption shows only while the edge is hovered or one of its ends is hovered or selected (the details list it
// always). While an area is hovered or selected, its edges and neighbours stay bright and the rest of the map fades.
// Search: the areas are records of the search index (SetExtra source kExtraAreas: the name in the data language, the
// English and official names, the bosses, the quests and the quest items). A hit opens Welcome > Campaign Progression
// on the area's tab, selects the area, rings it for a moment and scrolls it into view. While text is typed, the entry
// stays in the list when an area matches; matching dots get a gold ring and their tabs a gold frame.
//
// Controller (PadZone::Campaign, entered with D-pad right from the list): the D-pad moves between the dots (the nearest
// in that direction, campaign::NeighbourTable) or the cards, LB / RB change the tab, A selects (shows the details),
// B closes the details first and then goes back to the list. The highlighted area shows its tooltip.
// ---------------------------------------------------------------------------------------------------------------

namespace {

Gdiplus::REAL Fl(double value) { return static_cast<Gdiplus::REAL>(value); }

Gdiplus::Color GdiColor(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

constexpr COLORREF kCampaignAmber = RGB(251, 191, 36);
constexpr COLORREF kCampaignRed = RGB(248, 113, 113);
constexpr COLORREF kCampaignOrange = RGB(251, 146, 60);
constexpr COLORREF kCampaignSky = RGB(56, 189, 248);
constexpr COLORREF kCampaignFuchsia = RGB(232, 121, 249);
constexpr COLORREF kCampaignCaption = RGB(128, 116, 96);  // region captions, fainter than kDim

// Dot fill and outline per terrain (the reference's dark theme).
void TerrainColors(const std::string& terrain, COLORREF& fill, COLORREF& stroke) {
    fill = kPanel;
    stroke = kText;
    if (terrain == "outdoor") { fill = RGB(6, 78, 59); stroke = RGB(52, 211, 153); }
    else if (terrain == "cave") { fill = RGB(68, 64, 60); stroke = RGB(214, 211, 209); }
    else if (terrain == "tomb") { fill = RGB(51, 65, 85); stroke = RGB(203, 213, 225); }
    else if (terrain == "dungeon") { fill = RGB(120, 53, 15); stroke = kCampaignAmber; }
    else if (terrain == "temple") { fill = RGB(124, 45, 18); stroke = kCampaignOrange; }
    else if (terrain == "tower") { fill = RGB(63, 63, 70); stroke = RGB(212, 212, 216); }
    else if (terrain == "special") { fill = RGB(76, 29, 149); stroke = RGB(196, 181, 253); }
}

// The UI text key of a terrain.
const wchar_t* TerrainLabel(const std::string& terrain) {
    if (terrain == "town") return L"Town";
    if (terrain == "outdoor") return L"Outdoor area";
    if (terrain == "cave") return L"Cave";
    if (terrain == "tomb") return L"Tomb";
    if (terrain == "dungeon") return L"Dungeon";
    if (terrain == "temple") return L"Temple";
    if (terrain == "tower") return L"Tower";
    return L"Special area";
}

const wchar_t* EdgeKindLabel(campaign::EdgeKind kind) {
    switch (kind) {
    case campaign::EdgeKind::Border: return L"Walk across";
    case campaign::EdgeKind::Passage: return L"Entrance";
    case campaign::EdgeKind::Portal: return L"Portal";
    case campaign::EdgeKind::Cube: return L"Cube portal";
    case campaign::EdgeKind::Act: return L"Next act";
    }
    return L"Entrance";
}

std::wstring ActTitle(int act) {
    static const wchar_t* const kActs[] = {L"Act I", L"Act II", L"Act III", L"Act IV", L"Act V"};
    return act >= 0 && act < 5 ? UiText(kActs[act]) : std::wstring();
}

// Text drawn and measured with DrawText itself: the map's labels are not page text, so the search highlight (Text) does
// not cut them into pieces.
void CampaignText(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font, UINT format) {
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_NOPREFIX);
    SelectObject(dc, previous);
}

// The size of `text` wrapped at `wrapWidth` (its widest line and its height).
SIZE CampaignMeasure(HDC dc, const std::wstring& text, int wrapWidth, HFONT font) {
    SIZE out{0, 0};
    if (text.empty()) return out;
    RECT measured = Rect(0, 0, wrapWidth, 0);
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, previous);
    out.cx = measured.right - measured.left;
    out.cy = measured.bottom - measured.top;
    return out;
}

// The shared part of a dungeon stack's names ("Tower Cellar Level 1" .. "Level 5" gives "Tower Cellar"; "Turmkeller
// Ebene 1" .. gives "Turmkeller Ebene"): the common start, without the level numbers left at its end.
std::wstring StackBase(const std::vector<std::wstring>& names, bool english) {
    if (names.empty()) return std::wstring();
    std::wstring base = names[0];
    for (const std::wstring& name : names) {
        std::size_t same = 0;
        while (same < base.size() && same < name.size() && base[same] == name[same]) ++same;
        base.resize(same);
    }
    while (!base.empty() && (base.back() == L' ' || (base.back() >= L'0' && base.back() <= L'9') || base.back() == L'\u7B2C' ||
                             base.back() == L'(' || base.back() == L'-'))
        base.pop_back();
    if (english && base.size() > 6 && base.compare(base.size() - 6, 6, L" Level") == 0) base.resize(base.size() - 6);
    return base.empty() ? names[0] : base;
}

// The last run of digits in a name ("Tower Cellar Level 3" gives "3"), or "".
std::wstring LevelNumber(const std::wstring& name) {
    std::size_t end = name.size();
    while (end > 0 && !(name[end - 1] >= L'0' && name[end - 1] <= L'9')) --end;
    std::size_t start = end;
    while (start > 0 && name[start - 1] >= L'0' && name[start - 1] <= L'9') --start;
    return name.substr(start, end - start);
}

void RoundRectPath(Gdiplus::GraphicsPath& path, Gdiplus::REAL x, Gdiplus::REAL y, Gdiplus::REAL w, Gdiplus::REAL h,
                   Gdiplus::REAL radius) {
    const Gdiplus::REAL d = std::max(Fl(1), std::min({radius * 2, w, h}));
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

// A rounded backing under a label or caption (so no line runs through its text); `radius` at most half its height.
void FillBacking(Gdiplus::Graphics& graphics, const RECT& box, COLORREF color, BYTE alpha, int radius) {
    const int w = box.right - box.left, h = box.bottom - box.top;
    if (w <= 0 || h <= 0) return;
    Gdiplus::GraphicsPath path;
    RoundRectPath(path, Fl(box.left), Fl(box.top), Fl(w), Fl(h), Fl(std::min(radius, h / 2)));
    Gdiplus::SolidBrush brush(GdiColor(color, alpha));
    graphics.FillPath(&brush, &path);
}

// `color` faded towards the panel (what is out of focus while an area or edge is hovered): 35% of it is left.
COLORREF FadeColor(COLORREF color) {
    const auto mix = [](int a, int b) { return static_cast<BYTE>((a * 35 + b * 65) / 100); };
    // The panel's channels taken with shifts, not GetRValue: its BYTE cast of a constant warns (C4310).
    const int panelR = static_cast<int>(kPanel & 0xFF), panelG = static_cast<int>((kPanel >> 8) & 0xFF), panelB = static_cast<int>((kPanel >> 16) & 0xFF);
    return RGB(mix(GetRValue(color), panelR), mix(GetGValue(color), panelG), mix(GetBValue(color), panelB));
}
BYTE FadeAlpha(BYTE alpha) { return static_cast<BYTE>(alpha * 35 / 100); }

// One mark centred on (x, y): 0 boss (red triangle), 1 quest item (amber diamond), 2 caveat (orange "!"); k is the scale.
void DrawMark(Gdiplus::Graphics& graphics, int which, double x, double y, double k, BYTE alpha) {
    if (which == 0) {
        Gdiplus::SolidBrush red(GdiColor(kCampaignRed, alpha));
        const Gdiplus::PointF tri[3] = {{Fl(x), Fl(y - 3.5 * k)}, {Fl(x + 3.5 * k), Fl(y + 3 * k)}, {Fl(x - 3.5 * k), Fl(y + 3 * k)}};
        graphics.FillPolygon(&red, tri, 3);
    } else if (which == 1) {
        Gdiplus::SolidBrush amber(GdiColor(kCampaignAmber, alpha));
        const Gdiplus::PointF diamond[4] = {{Fl(x), Fl(y - 4 * k)}, {Fl(x + 3.5 * k), Fl(y)}, {Fl(x), Fl(y + 4 * k)}, {Fl(x - 3.5 * k), Fl(y)}};
        graphics.FillPolygon(&amber, diamond, 4);
    } else {
        Gdiplus::SolidBrush orange(GdiColor(kCampaignOrange, alpha));
        graphics.FillEllipse(&orange, Fl(x - 4 * k), Fl(y - 4 * k), Fl(8 * k), Fl(8 * k));
        Gdiplus::Pen mark(GdiColor(RGB(24, 17, 11), alpha), Fl(std::max(1.0, 1.3 * k)));
        graphics.DrawLine(&mark, Fl(x), Fl(y - 2.4 * k), Fl(x), Fl(y + 0.7 * k));
        graphics.DrawLine(&mark, Fl(x), Fl(y + 1.7 * k), Fl(x), Fl(y + 2.3 * k));
    }
}

// Boss, quest item and caveat marks in a row from (x, y), the centre of the first one, 9 k apart; k is the scale.
void DrawBadges(Gdiplus::Graphics& graphics, const campaign::Area& area, double x, double y, double k, BYTE alpha = 255) {
    double at = x;
    const bool marks[3] = {campaign::MarkBoss(area), campaign::MarkQuestItem(area), campaign::MarkCaveat(area)};
    for (int which = 0; which < 3; ++which) {
        if (!marks[which]) continue;
        DrawMark(graphics, which, at, y, k, alpha);
        at += 9 * k;
    }
}

// How an edge of each kind is drawn: border thick and faint, passage thin, portal dashed fuchsia, cube dash-dot amber,
// act bold (pen widths at scale 1).
struct EdgeLook {
    COLORREF color = kDim;
    BYTE alpha = 255;
    double width = 1.6;
    std::vector<Gdiplus::REAL> dash;
};

EdgeLook EdgeLookOf(campaign::EdgeKind kind) {
    EdgeLook look;
    switch (kind) {
    case campaign::EdgeKind::Border: look.alpha = 90; look.width = 5; break;
    case campaign::EdgeKind::Passage: break;
    case campaign::EdgeKind::Portal: look.color = kCampaignFuchsia; look.width = 2; look.dash = {3.0f, 2.0f}; break;
    case campaign::EdgeKind::Cube: look.color = kCampaignAmber; look.width = 2; look.dash = {4.5f, 1.5f, 1.0f, 1.5f}; break;
    case campaign::EdgeKind::Act: look.color = kText; look.width = 2.5; break;
    }
    return look;
}

void StyleEdgePen(Gdiplus::Pen& pen, const EdgeLook& look) {
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    if (!look.dash.empty()) pen.SetDashPattern(look.dash.data(), static_cast<INT>(look.dash.size()));
    else {
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
    }
}

}  // namespace

// ---- data and state ---------------------------------------------------------------------------------------------

bool OverlayState::IsCampaignEntry(const Entry& entry) { return entry.campaignMap || entry.title == "Campaign Progression"; }

void Overlay::SetCampaignData(std::shared_ptr<const campaign::Data> data) {
    if (!impl_ || !data) return;
    {
        std::lock_guard<std::mutex> guard(impl_->campaignLock);
        impl_->campaignPending = std::move(data);
    }
    // Adopted on the UI thread (ApplyCampaignData); a window not made yet adopts it on its first look-up or paint.
    if (impl_->window != nullptr) PostMessageW(impl_->window, kMsgCampaignData, 0, 0);
}

bool OverlayState::AdoptCampaignData() {
    std::shared_ptr<const campaign::Data> next;
    {
        std::lock_guard<std::mutex> guard(campaignLock);
        next = std::move(campaignPending);
        campaignPending.reset();
    }
    if (!next) return false;
    campaignData = std::move(next);
    campaignGraph = CampaignGraph{};
    campaignHits.clear();
    campaignSpots.clear();
    campaignHoverKind = -1;
    campaignHover = 0;
    campaignPadIndex = 0;
    campaignFlashArea = 0;
    campaignTab = campaignData->tabs.empty() ? 0 : std::clamp(campaignTab, 0, static_cast<int>(campaignData->tabs.size()) - 1);
    if (campaign::FindArea(*campaignData, campaignSelected) == nullptr) campaignSelected = 0;
    return true;  // the search index and CampaignMatches see the new pointer and are made again when next needed
}

void OverlayState::ApplyCampaignData() {
    if (!AdoptCampaignData() || database == nullptr) return;
    if (!query.empty()) {
        const bool suggestWasOpen = suggestOpen;
        Refilter();
        UpdateSuggest();
        if (!suggestWasOpen) suggestOpen = false;
    }
    InvalidateRect(window, nullptr, FALSE);
}

std::string OverlayState::CampaignLang() const { return gKeyboardLocale.empty() ? std::string("enUS") : gKeyboardLocale; }

bool OverlayState::CampaignShowing() const {
    if (database == nullptr || database->pages.empty() || !campaignData || campaignData->tabs.empty()) return false;
    if (pageIndex >= views.size()) return false;
    const Page& page = CurrentPage();
    if (page.id != "welcome") return false;
    const PageView& view = views[pageIndex];
    if (view.selected >= filtered.size() || filtered[view.selected] >= page.entries.size()) return false;
    return IsCampaignEntry(page.entries[filtered[view.selected]]);
}

int OverlayState::CampaignPageIndex() const {
    if (database == nullptr) return -1;
    for (std::size_t i = 0; i < database->pages.size(); ++i)
        if (database->pages[i].id == "welcome" && CampaignEntryIndex(database->pages[i]) >= 0) return static_cast<int>(i);
    return -1;
}

int OverlayState::CampaignEntryIndex(const Page& page) const {
    if (page.id != "welcome") return -1;
    for (std::size_t i = 0; i < page.entries.size(); ++i)
        if (page.entries[i].campaignMap) return static_cast<int>(i);
    for (std::size_t i = 0; i < page.entries.size(); ++i)
        if (IsCampaignEntry(page.entries[i])) return static_cast<int>(i);
    return -1;
}

// True when `areaId` is drawn on tab `tabIndex` itself (a dot that is not a ghost, or a card).
bool OverlayState::CampaignOnTab(int tabIndex, int areaId) const {
    if (!campaignData || tabIndex < 0 || static_cast<std::size_t>(tabIndex) >= campaignData->tabs.size()) return false;
    const campaign::Tab& tab = campaignData->tabs[static_cast<std::size_t>(tabIndex)];
    int id = areaId;
    if (const campaign::Area* area = campaign::FindArea(*campaignData, areaId); area != nullptr && area->twinOf != 0) id = area->twinOf;
    if (std::find(tab.cards.begin(), tab.cards.end(), id) != tab.cards.end()) return true;
    for (const campaign::Station& station : tab.stations)
        if (!station.ghost && std::find(station.ids.begin(), station.ids.end(), id) != station.ids.end()) return true;
    return false;
}

// The tab an area belongs to: the one its data names when it is drawn there, else the first that draws it; -1 for none.
int OverlayState::CampaignTabOf(int areaId) const {
    if (!campaignData) return -1;
    const campaign::Area* area = campaign::FindArea(*campaignData, areaId);
    if (area == nullptr) return -1;
    const int count = static_cast<int>(campaignData->tabs.size());
    for (int i = 0; i < count; ++i)
        if (campaignData->tabs[static_cast<std::size_t>(i)].key == area->tab && CampaignOnTab(i, areaId)) return i;
    for (int i = 0; i < count; ++i)
        if (CampaignOnTab(i, areaId)) return i;
    return -1;
}

bool OverlayState::CampaignFlashing() const { return campaignFlashArea != 0 && GetTickCount64() <= campaignFlashUntil; }

void OverlayState::CampaignSelectTab(int index) {
    if (!campaignData || index < 0 || static_cast<std::size_t>(index) >= campaignData->tabs.size()) return;
    if (index != campaignTab) campaignPadIndex = 0;
    campaignTab = index;
    campaignHoverKind = -1;
    campaignHover = 0;
    InvalidateRect(window, nullptr, FALSE);
}

// Opens the area's own tab, selects it and scrolls it into view after the next layout; `flash` rings it for a moment.
void OverlayState::CampaignShowArea(int areaId, bool flash) {
    if (!campaignData) return;
    const campaign::Area* area = campaign::FindArea(*campaignData, areaId);
    if (area == nullptr) return;
    const int shown = area->twinOf != 0 ? area->twinOf : areaId;  // a folded twin is selected as the row the map shows
    const int tabIndex = CampaignOnTab(campaignTab, shown) ? campaignTab : CampaignTabOf(shown);
    if (tabIndex < 0) return;
    CampaignSelectTab(tabIndex);
    campaignSelected = shown;
    campaignSpoilers = false;
    campaignReveal = true;
    campaignPadFollow = true;
    if (flash) {
        campaignFlashArea = shown;
        campaignFlashUntil = GetTickCount64() + 1800;
        SetTimer(window, kTimerFlash, 1900, nullptr);
    }
    InvalidateRect(window, nullptr, FALSE);
}

// A click (or A) on an area: selected here when it is on the shown tab, else (a ghost of another tab) its own tab opens.
void OverlayState::CampaignPick(int areaId) {
    if (!campaignData || campaign::FindArea(*campaignData, areaId) == nullptr) return;
    if (!CampaignOnTab(campaignTab, areaId)) {
        CampaignShowArea(areaId, false);
        return;
    }
    const campaign::Area* area = campaign::FindArea(*campaignData, areaId);
    campaignSelected = area->twinOf != 0 ? area->twinOf : areaId;
    campaignSpoilers = false;
    InvalidateRect(window, nullptr, FALSE);
}

// Escape: closes the details of the selected area first. True when it did.
bool OverlayState::CampaignEscape() {
    if (!CampaignShowing() || campaignSelected == 0) return false;
    campaignSelected = 0;
    InvalidateRect(window, nullptr, FALSE);
    return true;
}

// ---- search -----------------------------------------------------------------------------------------------------

// One record per area the map draws (folded twins are left out: the row the map shows stands for them).
std::vector<esrwiki::search::ExtraRecord> OverlayState::CampaignSearchRecords() const {
    std::vector<esrwiki::search::ExtraRecord> records;
    const int page = CampaignPageIndex();
    if (!campaignData || page < 0) return records;
    const Page& welcome = database->pages[static_cast<std::size_t>(page)];
    const int entryIndex = CampaignEntryIndex(welcome);
    if (entryIndex < 0) return records;
    const std::string context = esrwiki::search::StripColorCodes(welcome.title) + " \xE2\x80\xBA " +
                                esrwiki::search::StripColorCodes(welcome.entries[static_cast<std::size_t>(entryIndex)].title);
    const std::string lang = CampaignLang();
    for (const campaign::Area& area : campaignData->areas) {
        if (area.twinOf != 0) continue;
        const int tabIndex = CampaignTabOf(area.id);
        if (tabIndex < 0) continue;  // not on the map
        esrwiki::search::ExtraRecord record;
        record.label = campaign::NameIn(area, lang);
        record.context = context;
        const std::string text = campaign::SearchText(*campaignData, area.id, lang);
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t end = std::min(text.find('\n', start), text.size());
            if (end > start && text.compare(start, end - start, record.label) != 0) record.lines.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        record.page = page;
        record.group = tabIndex;
        record.item = area.id;
        record.source = kExtraAreas;
        records.push_back(std::move(record));
    }
    return records;
}

// The areas (ids, sorted) that match the search text, from the index's area records. Made again only when the text, the
// data or the database changed. It never adopts new data itself (the map holds references into campaignData).
const std::vector<int>& OverlayState::CampaignMatches() {
    if (campaignMatchValid && campaignMatchQuery == query && campaignMatchData == campaignData && campaignMatchDb == database)
        return campaignMatch;
    campaignMatchValid = true;
    campaignMatchQuery = query;
    campaignMatchData = campaignData;
    campaignMatchDb = database;
    campaignMatch.clear();
    if (query.empty() || !campaignData || database == nullptr) return campaignMatch;
    EnsureSearchIndex();
    const std::vector<esrwiki::search::ExtraRecord>& records = searchIndex.Extra();
    for (const std::size_t at : searchIndex.MatchingExtra(query)) {
        if (at >= records.size() || records[at].source != kExtraAreas || records[at].item <= 0) continue;
        campaignMatch.push_back(records[at].item);
    }
    std::sort(campaignMatch.begin(), campaignMatch.end());
    campaignMatch.erase(std::unique(campaignMatch.begin(), campaignMatch.end()), campaignMatch.end());
    return campaignMatch;
}

// The area a search hit names, checked against the data by name (new data can have come since the list was made).
int OverlayState::CampaignAreaOfHit(const esrwiki::search::Hit& hit) const {
    if (!campaignData) return 0;
    const std::string lang = CampaignLang();
    const campaign::Area* area = campaign::FindArea(*campaignData, hit.extraItem);
    if (area != nullptr && esrwiki::search::StripColorCodes(campaign::NameIn(*area, lang)) == hit.label) return area->id;
    for (const campaign::Area& other : campaignData->areas)
        if (other.twinOf == 0 && esrwiki::search::StripColorCodes(campaign::NameIn(other, lang)) == hit.label) return other.id;
    return 0;
}

// ---- layout of a graph tab ----------------------------------------------------------------------------------------

void OverlayState::CampaignLayoutGraph(HDC dc, int tabIndex, double s) {
    const std::string lang = CampaignLang();
    const int scaleKey = static_cast<int>(std::lround(s * 1000));
    CampaignGraph& g = campaignGraph;
    if (g.data == campaignData && g.tab == tabIndex && g.scaleKey == scaleKey && g.font == fontSnippet && g.lang == lang) return;
    g = CampaignGraph{};
    g.data = campaignData;
    g.tab = tabIndex;
    g.scaleKey = scaleKey;
    g.font = fontSnippet;
    g.lang = lang;
    g.scale = s;
    g.padX = S(3);
    g.padY = S(1);
    const campaign::Data& data = *campaignData;
    const campaign::Tab& tab = data.tabs[static_cast<std::size_t>(tabIndex)];
    const double r = campaign::kDotR * s;
    const auto ix = [](double v) { return static_cast<int>(std::lround(v)); };
    const auto backed = [&g](SIZE size) {
        return campaign::Extent{static_cast<double>(size.cx + 2 * g.padX + 1), static_cast<double>(size.cy + 2 * g.padY)};
    };
    // The bigger of the plain and the bold size (a selected label is bold and must still fit its box).
    const auto measureBoth = [&](const std::wstring& what, int wrap, HFONT plain, HFONT bold) {
        const SIZE a = CampaignMeasure(dc, what, wrap, plain), b = CampaignMeasure(dc, what, wrap, bold);
        return SIZE{std::max(a.cx, b.cx), std::max(a.cy, b.cy)};
    };

    g.nodes = campaign::LayoutNodes(data, tab);
    g.neighbours = campaign::NeighbourTable(g.nodes);
    const std::vector<campaign::DrawnEdge> drawnEdges = campaign::TabEdges(data, tab, g.nodes);
    const std::vector<campaign::LabelGroup> groups = campaign::LabelGroups(g.nodes);
    campaign::LabelSceneText text;
    text.members.assign(g.nodes.size(), campaign::Extent{});
    std::vector<std::wstring> memberText(g.nodes.size());

    // A stack's levels get short labels ("L3") right of their dots, when every level has a number.
    for (const campaign::LabelGroup& group : groups) {
        if (!group.stack) continue;
        std::vector<std::wstring> numbers;
        bool all = true;
        for (std::size_t k = group.first; k <= group.last; ++k) {
            numbers.push_back(LevelNumber(Wide(campaign::AreaName(data, g.nodes[k].id, lang))));
            if (numbers.back().empty()) all = false;
        }
        if (!all) continue;
        for (std::size_t k = group.first; k <= group.last; ++k) {
            memberText[k] = UiFormat(L"L{0}", {numbers[k - group.first]});
            text.members[k] = backed(measureBoth(memberText[k], 4000, fontSnippet, fontSnippetBold));
        }
    }

    // The station labels: one line up to 1.5 cells wide; a stack's common name in the small font; a ghost names its tab.
    const int wrapWidth = std::max(S(64), static_cast<int>(1.5 * campaign::kCellW * s));
    for (const campaign::LabelGroup& group : groups) {
        const campaign::Node& node = g.nodes[group.first];
        CampaignGraph::Label label;
        label.area = node.id;
        label.first = static_cast<int>(group.first);
        label.last = static_cast<int>(group.last);
        label.stack = group.stack;
        label.ghost = group.ghost;
        label.smallFont = group.stack;
        std::vector<std::wstring> names;
        for (std::size_t k = group.first; k <= group.last; ++k) names.push_back(Wide(campaign::AreaName(data, g.nodes[k].id, lang)));
        if (group.ghost) {
            label.name = names[0];
            const int home = CampaignTabOf(node.id);
            if (home >= 0) label.info = L"\u2192 " + UiText(Wide(data.tabs[static_cast<std::size_t>(home)].title));
        } else {
            label.name = group.stack ? StackBase(names, lang == "enUS") : names[0];
            if (!node.folded.empty()) label.name += L" \u00D7" + std::to_wstring(node.folded.size() + 1);
            // Monster levels and "WP" are left to the tooltip, the details and the waypoint ring.
        }
        const SIZE nameSize = label.smallFont ? CampaignMeasure(dc, label.name, wrapWidth, fontSmall)
                                          : measureBoth(label.name, wrapWidth, fontSnippet, fontSnippetBold);
        const SIZE infoSize = CampaignMeasure(dc, label.info, 4000, fontSnippet);
        label.nameHeight = static_cast<int>(nameSize.cy);
        text.labels.push_back(backed(SIZE{std::max(nameSize.cx, infoSize.cx), nameSize.cy + infoSize.cy}));
        g.labels.push_back(std::move(label));
    }

    // Region captions: small, at the region's top left; the placer keeps the labels off them.
    std::vector<std::wstring> captions;
    for (const campaign::Region& region : tab.regions) {
        captions.push_back(UiDataText(Wide(region.label)));
        text.captions.push_back(backed(CampaignMeasure(dc, captions.back(), 4000, fontSmall)));
    }
    text.captionInsetX = S(6);
    text.captionInsetY = S(3);

    const campaign::LabelScene scene = campaign::MakeLabelScene(data, tab, g.nodes, drawnEdges, s, text);
    const std::vector<campaign::Box> placed = campaign::PlaceLabels(scene);
    // Labels that use the room above the top row move the whole graph down.
    const double top = campaign::GraphTop(placed) > 0 ? std::ceil(campaign::GraphTop(placed)) + S(2) : 0.0;
    const auto shifted = [top](campaign::Point p) { return campaign::Point{p.x, p.y + top}; };
    const auto toRect = [&](const campaign::Box& box) {
        return Rect(ix(box.x), ix(box.y + top), ix(box.x) + ix(box.w), ix(box.y + top) + ix(box.h));
    };
    g.width = static_cast<int>(scene.width);

    double bottom = 0;
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        CampaignGraph::Dot dot;
        dot.at = shifted(scene.dots[i]);
        dot.area = g.nodes[i].id;
        dot.ghost = g.nodes[i].ghost;
        dot.member = memberText[i];
        if (scene.members[i].w > 0) dot.memberBox = toRect(scene.members[i]);
        dot.marks = scene.markerSlots[i];
        dot.marks.first = shifted(dot.marks.first);
        dot.markCount = scene.markerCounts[i];
        bottom = std::max({bottom, dot.at.y + r + 8 * s, static_cast<double>(dot.memberBox.bottom)});
        if (dot.markCount > 0) bottom = std::max(bottom, dot.marks.first.y + dot.marks.half);
        g.dots.push_back(std::move(dot));
    }
    for (std::size_t i = 0; i < drawnEdges.size(); ++i) {
        const campaign::Edge& edge = data.edges[drawnEdges[i].edge];
        CampaignGraph::Line line;
        line.kind = edge.kind;
        line.arrow = edge.oneWay || edge.kind == campaign::EdgeKind::Act;
        line.fromDot = drawnEdges[i].fromNode;
        line.toDot = drawnEdges[i].toNode;
        for (const campaign::Point& p : scene.edges[i]) line.points.push_back(shifted(p));
        if (!edge.via.empty() && line.points.size() >= 2) {
            line.via = Wide(edge.via);
            double longest = -1;
            for (std::size_t k = 0; k + 1 < line.points.size(); ++k) {
                const double length = std::hypot(line.points[k + 1].x - line.points[k].x, line.points[k + 1].y - line.points[k].y);
                if (length > longest) {
                    longest = length;
                    line.viaAt = campaign::Point{(line.points[k].x + line.points[k + 1].x) / 2, (line.points[k].y + line.points[k + 1].y) / 2};
                }
            }
        }
        g.lines.push_back(std::move(line));
    }
    for (std::size_t i = 0; i < tab.regions.size(); ++i) {
        CampaignGraph::Region out;
        out.box = toRect(scene.regions[i]);
        out.caption = toRect(scene.captions[i]);
        out.label = captions[i];
        bottom = std::max(bottom, static_cast<double>(out.box.bottom));
        g.regions.push_back(std::move(out));
    }
    for (std::size_t gi = 0; gi < g.labels.size() && gi < placed.size(); ++gi) {
        g.labels[gi].box = toRect(placed[gi]);
        bottom = std::max(bottom, static_cast<double>(g.labels[gi].box.bottom));
    }
    g.height = static_cast<int>(std::ceil(bottom + campaign::kMarginY / 2 * s));
}

// ---- drawing ----------------------------------------------------------------------------------------------------

// The map section under the entry's text. Returns the y below it. With draw == false it only measures, and records where
// the areas are (campaignSpots, relative to the top of the entry) for the controller and for scrolling to an area.
int OverlayState::RenderCampaign(HDC dc, Gdiplus::Graphics* graphics, int left, int paneW, int y, bool draw) {
    if (draw) {
        campaignHits.clear();
        campaignGraphShown = false;
    }
    else {
        campaignSpots.clear();
        AdoptCampaignData();  // data that came before the window could be told (never between the layout and the paint)
        AdoptMapsData();      // the same for the official Maps page (overlay_campaign_live.inc)
    }
    if (!campaignData || campaignData->tabs.empty()) return y;  // no data: the section is not drawn
    if (draw && (y > clipBottom || graphics == nullptr)) return y;
    const campaign::Data& data = *campaignData;
    campaignTab = std::clamp(campaignTab, 0, static_cast<int>(data.tabs.size()) - 1);
    const campaign::Tab& tab = data.tabs[static_cast<std::size_t>(campaignTab)];
    const std::vector<int>& matches = CampaignMatches();

    y += S(4);
    if (draw) FillColor(dc, Rect(left, y, left + paneW, y + 1), kBorder);
    y += S(10);
    {
        const std::wstring heading = UiText(L"Campaign map");
        const int h = MeasureHeight(dc, heading, paneW, fontBold);
        if (draw) Text(dc, heading, Rect(left, y, left + paneW, y + h), kHeaderGold, fontBold, DT_LEFT | DT_WORDBREAK);
        y += h + S(6);
    }

    // The tab strip: Act I to Act V, Special areas, Ubers & portals (a tab with a matching area has a gold frame).
    {
        std::vector<char> tabMatched(data.tabs.size(), 0);
        for (const int id : matches) {
            const int at = CampaignTabOf(id);
            if (at >= 0) tabMatched[static_cast<std::size_t>(at)] = 1;
        }
        const int buttonH = bodyLine + S(10);
        int x = left;
        for (std::size_t i = 0; i < data.tabs.size(); ++i) {
            const std::wstring label = UiText(Wide(data.tabs[i].title));
            const int buttonW = MeasureWidth(dc, label, fontBody) + S(20);
            if (x > left && x + buttonW > left + paneW) {
                x = left;
                y += buttonH + S(4);
            }
            const RECT button = Rect(x, y, x + buttonW, y + buttonH);
            if (draw) {
                const bool current = static_cast<int>(i) == campaignTab;
                const bool hot = campaignHoverKind == 0 && campaignHover == static_cast<int>(i);
                FillPanel(dc, button, current ? kSelected : hot ? kHighlightFill : kPanel, 210);
                FrameColor(dc, button, current || tabMatched[i] != 0 || hot ? kGold : kBorder);
                CampaignText(dc, label, button, current ? kText : kHeaderGold, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                if (current) FillColor(dc, Rect(button.left + S(6), button.bottom - S(4), button.right - S(6), button.bottom - S(2)), kGold);
                campaignHits.push_back({button, 0, static_cast<int>(i)});
            }
            x += buttonW + S(4);
        }
        y += buttonH + S(10);
    }
    if (tab.grid) y = RenderCampaignLegend(dc, graphics, left, paneW, y, draw);

    y = tab.grid ? RenderCampaignGraph(dc, graphics, left, paneW, y, draw, matches) : RenderCampaignCards(dc, graphics, left, paneW, y, draw, matches);

    // The tab's own notes (spoilers are left out here).
    for (const campaign::Note& note : tab.notes) {
        if (note.spoiler || CampaignLiveHidesNote(tab, note)) continue;
        std::wstring text = L"\u2022 " + Wide(note.text);
        if (campaign::Unconfirmed(note.source)) text += L" " + UiText(L"(not confirmed yet)");
        const int h = MeasureHeight(dc, text, paneW, fontSnippet);
        if (draw && y + h >= clipTop && y <= clipBottom)
            Text(dc, text, Rect(left, y, left + paneW, y + h), note.warning ? kCampaignOrange : kDim, fontSnippet, DT_LEFT | DT_WORDBREAK);
        y += h + S(2);
    }
    y = RenderCampaignLiveNotes(dc, tab, left, paneW, y, draw);  // the official Maps page's own notes (overlay_campaign_live.inc)
    y += S(6);

    y = RenderCampaignDetails(dc, left, paneW, y, draw);
    return y + S(8);
}

// The key under the act tabs, in one row where it fits: the waypoint ring, the marks, and a sample of each kind of edge
// drawn with the map's own pens.
int OverlayState::RenderCampaignLegend(HDC dc, Gdiplus::Graphics* graphics, int left, int paneW, int y, bool draw) {
    const double k = static_cast<double>(dpi) / 96.0;
    const int rowH = std::max(smallLine, S(14));
    // kind: 0 waypoint, 1 boss, 2 quest item, 3 caveat, 10 + an edge kind
    struct Item { int kind; std::wstring text; int x, y, sampleW; };
    std::vector<Item> items = {
        {0, UiText(L"Waypoint"), 0, 0, 0},
        {1, UiText(L"Boss"), 0, 0, 0},
        {2, UiText(L"Quest items"), 0, 0, 0},
        {3, UiText(L"Heads up"), 0, 0, 0},
        {10 + static_cast<int>(campaign::EdgeKind::Border), UiText(L"Walk across"), 0, 0, 0},
        {10 + static_cast<int>(campaign::EdgeKind::Passage), UiText(L"Entrance"), 0, 0, 0},
        {10 + static_cast<int>(campaign::EdgeKind::Portal), UiText(L"Portal"), 0, 0, 0},
        {10 + static_cast<int>(campaign::EdgeKind::Cube), UiText(L"Cube portal"), 0, 0, 0},
        {10 + static_cast<int>(campaign::EdgeKind::Act), UiText(L"Next act"), 0, 0, 0},
    };
    int x = left;
    for (Item& item : items) {
        item.sampleW = item.kind >= 10 ? S(22) : S(14);
        const int itemW = item.sampleW + S(4) + MeasureWidth(dc, item.text, fontSmall);
        if (x > left && x + itemW > left + paneW) {
            x = left;
            y += rowH + S(2);
        }
        item.x = x;
        item.y = y;
        x += itemW + S(12);
    }
    y += rowH;
    if (draw && graphics != nullptr && y >= clipTop && items.front().y <= clipBottom) {
        Gdiplus::Graphics& gr = *graphics;
        const Gdiplus::GraphicsState saved = gr.Save();
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        for (const Item& item : items) {
            const double cx = item.x + item.sampleW / 2.0, cy = item.y + rowH / 2.0;
            if (item.kind == 0) {
                const double dotR = campaign::kDotR * k * 0.6;
                Gdiplus::Pen ring(GdiColor(kCampaignSky), Fl(std::max(2.0, 3 * k * 0.6)));
                const double ringR = dotR + 3 * k * 0.6;
                gr.DrawEllipse(&ring, Fl(cx - ringR), Fl(cy - ringR), Fl(ringR * 2), Fl(ringR * 2));
                Gdiplus::SolidBrush fill(GdiColor(RGB(6, 78, 59)));
                Gdiplus::Pen rim(GdiColor(RGB(52, 211, 153)), Fl(std::max(1.0, 1.5 * k * 0.6)));
                gr.FillEllipse(&fill, Fl(cx - dotR), Fl(cy - dotR), Fl(dotR * 2), Fl(dotR * 2));
                gr.DrawEllipse(&rim, Fl(cx - dotR), Fl(cy - dotR), Fl(dotR * 2), Fl(dotR * 2));
            } else if (item.kind < 10) {
                DrawMark(gr, item.kind - 1, cx, cy, campaign::MarkerScale(k), 255);
            } else {
                const EdgeLook look = EdgeLookOf(static_cast<campaign::EdgeKind>(item.kind - 10));
                Gdiplus::Pen pen(GdiColor(look.color, look.alpha), Fl(std::max(1.0, look.width * k)));
                StyleEdgePen(pen, look);
                const double half = item.sampleW / 2.0 - look.width * k / 2;
                gr.DrawLine(&pen, Fl(cx - half), Fl(cy), Fl(cx + half), Fl(cy));
            }
        }
        gr.Flush(Gdiplus::FlushIntentionSync);
        gr.Restore(saved);
        for (const Item& item : items) {
            const int tx = item.x + item.sampleW + S(4);
            CampaignText(dc, item.text, Rect(tx, item.y, tx + MeasureWidth(dc, item.text, fontSmall) + S(2), item.y + rowH), kDim, fontSmall,
                         DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
    }
    return y + S(8);
}

int OverlayState::RenderCampaignGraph(HDC dc, Gdiplus::Graphics* graphics, int left, int paneW, int y, bool draw,
                                      const std::vector<int>& matches) {
    const campaign::Data& data = *campaignData;
    const campaign::Tab& tab = data.tabs[static_cast<std::size_t>(campaignTab)];
    int maxCol = std::max(0, tab.cols - 1);
    for (const campaign::Station& station : tab.stations) maxCol = std::max(maxCol, station.col);
    const double gridW = 2 * campaign::kMarginX + maxCol * campaign::kCellW;
    const double screen = static_cast<double>(dpi) / 96.0;
    // Fits the pane, never larger than the design at this screen's scale, never below half of it (the labels keep 9 pt).
    const double s = std::max(0.5 * screen, std::min(static_cast<double>(paneW) / gridW, screen));
    CampaignLayoutGraph(dc, campaignTab, s);
    const CampaignGraph& g = campaignGraph;
    const int gx = left + std::max(0, (paneW - g.width) / 2);
    const int gy = y;
    const double r = campaign::kDotR * s;
    const auto ix = [](double v) { return static_cast<int>(std::lround(v)); };
    const auto shift = [gx, gy](const RECT& box) { return Rect(gx + box.left, gy + box.top, gx + box.right, gy + box.bottom); };

    // Where the areas are: the dots (the controller's places), then the labels and the level labels.
    const int reach = ix(r + 7 * s);
    for (std::size_t i = 0; i < g.dots.size(); ++i) {
        const CampaignGraph::Dot& dot = g.dots[i];
        const int cx = gx + ix(dot.at.x), cy = gy + ix(dot.at.y);
        const RECT box = Rect(cx - reach, cy - reach, cx + reach, cy + reach);
        if (draw) campaignHits.push_back({box, 1, dot.area});
        else campaignSpots.push_back({dot.area, Rect(box.left, box.top - matchBase, box.right, box.bottom - matchBase)});
    }
    if (draw) {
        campaignGraphAt = POINT{gx, gy};
        campaignGraphShown = true;
        for (const CampaignGraph::Label& label : g.labels) campaignHits.push_back({shift(label.box), 1, label.area});
        for (const CampaignGraph::Dot& dot : g.dots)
            if (!dot.member.empty()) campaignHits.push_back({shift(dot.memberBox), 1, dot.area});
    }
    if (!draw || gy > clipBottom || gy + g.height < clipTop) return gy + g.height + S(8);

    const bool flashing = CampaignFlashing();
    const bool padOn = padActive && !oskOpen && !suggestPadFocus && padZone == PadZone::Campaign;
    const int padSpot = campaignSpots.empty() ? -1 : std::clamp(campaignPadIndex, 0, static_cast<int>(campaignSpots.size()) - 1);
    const auto isMatch = [&](int id) { return std::binary_search(matches.begin(), matches.end(), id); };
    const auto pt = [&](const campaign::Point& p) { return Gdiplus::PointF(Fl(gx + p.x), Fl(gy + p.y)); };

    // Focus: only while an area or an edge is hovered (or an area has the controller highlight), it, its edges and the
    // areas at their other ends are drawn at full strength and everything else faded; a selection alone fades nothing.
    // lit: 2 the focused dot, 1 a neighbour, 0 neither.
    std::vector<int> lit(g.dots.size(), 0);
    bool focus = false;
    for (std::size_t i = 0; i < g.dots.size(); ++i) {
        const CampaignGraph::Dot& dot = g.dots[i];
        const bool hot = (campaignHoverKind == 1 && campaignHover == dot.area) || (padOn && padSpot == static_cast<int>(i));
        if (hot) {
            lit[i] = 2;
            focus = true;
        }
    }
    const int hoveredLine = campaignHoverKind == 5 ? campaignHover : -1;
    if (hoveredLine >= 0) focus = true;  // hovering an edge focuses it and its two ends
    const auto dotIn = [&](int d) { return d >= 0 && static_cast<std::size_t>(d) < lit.size(); };
    std::vector<char> lineLit(g.lines.size(), 0);
    for (std::size_t li = 0; li < g.lines.size(); ++li) {
        const CampaignGraph::Line& line = g.lines[li];
        const bool fromLit = dotIn(line.fromDot) && lit[static_cast<std::size_t>(line.fromDot)] == 2;
        const bool toLit = dotIn(line.toDot) && lit[static_cast<std::size_t>(line.toDot)] == 2;
        if (!fromLit && !toLit && static_cast<int>(li) != hoveredLine) continue;  // a hovered edge lights its two ends too
        lineLit[li] = 1;
        if (dotIn(line.fromDot)) lit[static_cast<std::size_t>(line.fromDot)] = std::max(lit[static_cast<std::size_t>(line.fromDot)], 1);
        if (dotIn(line.toDot)) lit[static_cast<std::size_t>(line.toDot)] = std::max(lit[static_cast<std::size_t>(line.toDot)], 1);
    }
    const auto dotOn = [&](std::size_t i) { return !focus || lit[i] > 0; };
    const auto labelOn = [&](const CampaignGraph::Label& label) {
        for (int k = label.first; k <= label.last; ++k)
            if (dotOn(static_cast<std::size_t>(k))) return true;
        return false;
    };
    const auto tone = [](COLORREF color, bool on) { return on ? color : FadeColor(color); };

    Gdiplus::Graphics& gr = *graphics;
    const Gdiplus::GraphicsState saved = gr.Save();
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const RECT frame = Rect(gx, gy, gx + g.width, gy + g.height);
    FillPanel(dc, frame, RGB(8, 6, 4), 150);
    FrameColor(dc, frame, kBorder);

    // 1. Regions.
    for (const CampaignGraph::Region& region : g.regions) {
        Gdiplus::Pen pen(GdiColor(kDim, 120), Fl(std::max(1.0, s)));
        pen.SetDashStyle(Gdiplus::DashStyleDash);
        gr.DrawRectangle(&pen, Fl(gx + region.box.left), Fl(gy + region.box.top), Fl(region.box.right - region.box.left),
                         Fl(region.box.bottom - region.box.top));
    }

    // 2. Edges, with an arrowhead at the `to` end of a one-way or act edge.
    for (std::size_t li = 0; li < g.lines.size(); ++li) {
        const CampaignGraph::Line& line = g.lines[li];
        if (line.points.size() < 2) continue;
        const EdgeLook look = EdgeLookOf(line.kind);
        const bool on = !focus || lineLit[li] != 0 || static_cast<int>(li) == hoveredLine;
        const BYTE alpha = on ? look.alpha : FadeAlpha(look.alpha);
        std::vector<Gdiplus::PointF> points;
        for (const campaign::Point& p : line.points) points.push_back(pt(p));
        Gdiplus::PointF head[3];
        bool arrow = false;
        if (line.arrow) {
            const Gdiplus::PointF a = points[points.size() - 2], b = points.back();
            const double dx = b.X - a.X, dy = b.Y - a.Y, length = std::hypot(dx, dy);
            const double headLength = std::max(6.0, 8 * s), halfWidth = std::max(3.5, 4.5 * s);
            if (length > r + 2 * s + headLength) {
                const double ux = dx / length, uy = dy / length;
                const double tipX = b.X - ux * (r + 2 * s), tipY = b.Y - uy * (r + 2 * s);
                const double baseX = tipX - ux * headLength, baseY = tipY - uy * headLength;
                head[0] = Gdiplus::PointF(Fl(tipX), Fl(tipY));
                head[1] = Gdiplus::PointF(Fl(baseX - uy * halfWidth), Fl(baseY + ux * halfWidth));
                head[2] = Gdiplus::PointF(Fl(baseX + uy * halfWidth), Fl(baseY - ux * halfWidth));
                points.back() = Gdiplus::PointF(Fl(baseX + ux), Fl(baseY + uy));
                arrow = true;
            }
        }
        Gdiplus::Pen pen(GdiColor(look.color, alpha), Fl(std::max(1.0, look.width * s)));
        StyleEdgePen(pen, look);
        gr.DrawLines(&pen, points.data(), static_cast<INT>(points.size()));
        if (arrow) {
            const BYTE headAlpha = look.alpha < 200 ? BYTE{200} : look.alpha;
            Gdiplus::SolidBrush brush(GdiColor(look.color, on ? headAlpha : FadeAlpha(headAlpha)));
            gr.FillPolygon(&brush, head, 3);
        }
    }

    // 3. Dots: a pill round each dungeon stack, the selection glow, the waypoint ring, the terrain shape.
    for (const CampaignGraph::Label& label : g.labels) {
        if (!label.stack) continue;
        const campaign::Box pillBox = campaign::StackPill(g.dots[static_cast<std::size_t>(label.first)].at,
                                                          g.dots[static_cast<std::size_t>(label.last)].at, s);
        Gdiplus::GraphicsPath path;
        RoundRectPath(path, Fl(gx + pillBox.x), Fl(gy + pillBox.y), Fl(pillBox.w), Fl(pillBox.h), Fl(pillBox.w / 2));
        Gdiplus::SolidBrush fill(GdiColor(kPanel));
        gr.FillPath(&fill, &path);
        Gdiplus::Pen rim(GdiColor(tone(kDim, labelOn(label))), Fl(std::max(1.0, 1.25 * s)));
        gr.DrawPath(&rim, &path);
    }
    for (std::size_t i = 0; i < g.dots.size(); ++i) {
        const CampaignGraph::Dot& dot = g.dots[i];
        const campaign::Area* area = campaign::FindArea(data, dot.area);
        const Gdiplus::PointF c = pt(dot.at);
        const bool on = dotOn(i);
        const auto R = [&](double extra) { return Fl(r + extra * s); };
        const bool selected = !dot.ghost && dot.area == campaignSelected;
        const bool hot = (campaignHoverKind == 1 && campaignHover == dot.area) || (padOn && padSpot == static_cast<int>(i));
        if (selected) {
            Gdiplus::SolidBrush glow(GdiColor(kCampaignAmber, 64));
            const Gdiplus::REAL gr15 = R(15);
            gr.FillEllipse(&glow, c.X - gr15, c.Y - gr15, gr15 * 2, gr15 * 2);
        }
        if (area != nullptr && area->waypoint && !dot.ghost) {  // a little bolder than the reference's (2 px)
            Gdiplus::Pen pen(GdiColor(tone(kCampaignSky, on)), Fl(std::max(2.0, 3 * s)));
            const Gdiplus::REAL rr = R(3);
            gr.DrawEllipse(&pen, c.X - rr, c.Y - rr, rr * 2, rr * 2);
        }
        COLORREF fill = kPanel, stroke = kDim;
        if (area != nullptr && !dot.ghost) TerrainColors(area->terrain, fill, stroke);
        if (hot) stroke = kGold;
        Gdiplus::SolidBrush brush(GdiColor(tone(fill, on)));
        Gdiplus::Pen pen(GdiColor(tone(stroke, on)), Fl(std::max(1.0, 1.5 * s)));
        if (dot.ghost) pen.SetDashStyle(Gdiplus::DashStyleDot);
        const Gdiplus::REAL rr = R(0);
        if (area != nullptr && area->town && !dot.ghost) {
            const Gdiplus::REAL half = R(1);
            gr.FillRectangle(&brush, c.X - half, c.Y - half, half * 2, half * 2);
            gr.DrawRectangle(&pen, c.X - half, c.Y - half, half * 2, half * 2);
        } else {
            gr.FillEllipse(&brush, c.X - rr, c.Y - rr, rr * 2, rr * 2);
            gr.DrawEllipse(&pen, c.X - rr, c.Y - rr, rr * 2, rr * 2);
        }
    }

    // The search ring, the jump ring and the selection ring go under the label backings, so they never cut over a label.
    for (std::size_t i = 0; i < g.dots.size(); ++i) {
        const CampaignGraph::Dot& dot = g.dots[i];
        const Gdiplus::PointF c = pt(dot.at);
        const auto ring = [&](COLORREF color, double radius, double penW, BYTE alpha) {
            Gdiplus::Pen pen(GdiColor(color, alpha), Fl(std::max(1.0, penW * s)));
            const Gdiplus::REAL rr = Fl(r + radius * s);
            gr.DrawEllipse(&pen, c.X - rr, c.Y - rr, rr * 2, rr * 2);
        };
        if (!dot.ghost && isMatch(dot.area)) ring(kGold, 5, 2, 230);
        if (flashing && dot.area == campaignFlashArea) ring(RGB(255, 226, 140), 9, 3, 255);
        if (!dot.ghost && dot.area == campaignSelected) ring(kCampaignAmber, 6, 3.5, 255);
    }

    // 4. Labels: a dark rounded backing under every text (region captions, level labels, station labels), then the texts.
    const int roundness = S(7);
    for (const CampaignGraph::Region& region : g.regions) FillBacking(gr, shift(region.caption), kPanel, 190, roundness);
    for (const CampaignGraph::Dot& dot : g.dots)
        if (!dot.member.empty()) FillBacking(gr, shift(dot.memberBox), kPanel, 215, roundness);
    std::vector<char> labelSelected(g.labels.size(), 0), labelHot(g.labels.size(), 0);
    for (std::size_t li = 0; li < g.labels.size(); ++li) {
        const CampaignGraph::Label& label = g.labels[li];
        for (int k = label.first; k <= label.last; ++k) {  // a stack's label is lit when one of its levels is
            const int id = g.dots[static_cast<std::size_t>(k)].area;
            if (!label.ghost && id == campaignSelected) labelSelected[li] = 1;
            if ((campaignHoverKind == 1 && campaignHover == id) || (padOn && padSpot == k)) labelHot[li] = 1;
        }
        FillBacking(gr, shift(label.box), labelSelected[li] != 0 ? kCampaignAmber : kPanel, labelSelected[li] != 0 ? BYTE{235} : BYTE{215},
                    roundness);
    }
    gr.Flush(Gdiplus::FlushIntentionSync);
    const auto inset = [&](const RECT& box) { return Rect(box.left + g.padX, box.top + g.padY, box.right - g.padX, box.bottom - g.padY); };
    for (const CampaignGraph::Region& region : g.regions)
        CampaignText(dc, region.label, inset(shift(region.caption)), kCampaignCaption, fontSmall, DT_LEFT | DT_SINGLELINE);
    for (std::size_t i = 0; i < g.dots.size(); ++i) {
        const CampaignGraph::Dot& dot = g.dots[i];
        if (dot.member.empty()) continue;
        const bool selected = dot.area == campaignSelected;
        CampaignText(dc, dot.member, inset(shift(dot.memberBox)), tone(selected ? kText : kDim, dotOn(i)),
                     selected ? fontSnippetBold : fontSnippet, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    for (std::size_t li = 0; li < g.labels.size(); ++li) {
        const CampaignGraph::Label& label = g.labels[li];
        const RECT box = inset(shift(label.box));
        if (box.bottom < clipTop || box.top > clipBottom) continue;
        const bool on = labelOn(label);
        const RECT nameBox = Rect(box.left, box.top, box.right, box.top + label.nameHeight);
        const RECT infoBox = Rect(box.left, box.top + label.nameHeight, box.right, box.bottom);
        const HFONT nameFont = label.smallFont ? fontSmall : fontSnippet;
        if (labelSelected[li] != 0) {
            CampaignText(dc, label.name, nameBox, RGB(20, 14, 8), label.smallFont ? fontSmall : fontSnippetBold, DT_CENTER | DT_WORDBREAK);
            CampaignText(dc, label.info, infoBox, RGB(70, 50, 20), fontSnippet, DT_CENTER | DT_SINGLELINE);
        } else {
            CampaignText(dc, label.name, nameBox, tone(label.ghost ? kDim : labelHot[li] != 0 ? kGold : kText, on), nameFont,
                         DT_CENTER | DT_WORDBREAK);
            CampaignText(dc, label.info, infoBox, tone(kDim, on), fontSnippet, DT_CENTER | DT_SINGLELINE);
        }
    }

    // 5. Marks, pinned to their dot: boss, quest item, caveat in a row at the dot's top right (a stack member's: right of
    // its level label).
    for (std::size_t i = 0; i < g.dots.size(); ++i) {
        const CampaignGraph::Dot& dot = g.dots[i];
        if (dot.ghost || dot.markCount <= 0) continue;
        const campaign::Area* area = campaign::FindArea(data, dot.area);
        if (area == nullptr) continue;
        DrawBadges(gr, *area, gx + dot.marks.first.x, gy + dot.marks.first.y, dot.marks.k, dotOn(i) ? BYTE{255} : FadeAlpha(255));
    }

    // 6. Highlights: the "via" captions of the hovered edge and of the
    // focused area's edges.
    struct ViaBox { const std::wstring* text; RECT box; };
    std::vector<ViaBox> vias;
    for (std::size_t li = 0; li < g.lines.size(); ++li) {
        const CampaignGraph::Line& line = g.lines[li];
        if (line.via.empty()) continue;
        const bool endFocused = (dotIn(line.fromDot) && lit[static_cast<std::size_t>(line.fromDot)] == 2) ||
                                (dotIn(line.toDot) && lit[static_cast<std::size_t>(line.toDot)] == 2);
        if (!endFocused && static_cast<int>(li) != hoveredLine) continue;
        const SIZE size = CampaignMeasure(dc, line.via, 4000, fontSnippet);
        const int vx = gx + ix(line.viaAt.x) - static_cast<int>(size.cx) / 2 - g.padX;
        const int vy = gy + ix(line.viaAt.y) - static_cast<int>(size.cy) / 2 - g.padY;
        const RECT box = Rect(vx, vy, vx + static_cast<int>(size.cx) + 2 * g.padX, vy + static_cast<int>(size.cy) + 2 * g.padY);
        FillBacking(gr, box, kPanel, 235, roundness);
        Gdiplus::GraphicsPath path;
        RoundRectPath(path, Fl(box.left), Fl(box.top), Fl(box.right - box.left), Fl(box.bottom - box.top),
                      Fl(std::min(roundness, static_cast<int>(box.bottom - box.top) / 2)));
        Gdiplus::Pen rim(GdiColor(EdgeLookOf(line.kind).color, 200), Fl(1));
        gr.DrawPath(&rim, &path);
        vias.push_back({&line.via, box});
    }
    gr.Flush(Gdiplus::FlushIntentionSync);
    for (const ViaBox& via : vias) CampaignText(dc, *via.text, inset(via.box), kText, fontSnippet, DT_CENTER | DT_SINGLELINE);
    gr.Restore(saved);
    return gy + g.height + S(8);
}

// The Special areas and Ubers & portals tabs: a grid of cards (name, act and terrain, levels, bosses, how to get in).
int OverlayState::RenderCampaignCards(HDC dc, Gdiplus::Graphics* graphics, int left, int paneW, int y, bool draw,
                                      const std::vector<int>& matches) {
    const campaign::Data& data = *campaignData;
    const campaign::Tab& tab = data.tabs[static_cast<std::size_t>(campaignTab)];
    const std::string lang = CampaignLang();
    const int gap = S(8);
    const int columns = std::max(1, (paneW + gap) / (S(230) + gap));
    const int cardW = (paneW - gap * (columns - 1)) / columns;
    const int pad = S(8);
    const int inner = std::max(S(40), cardW - 2 * pad);
    const int badgeRoom = S(34);
    struct CardText { std::wstring name, facts, bosses, entry; int nameH, factsH, bossesH, entryH; };
    const auto textOf = [&](const campaign::Area& area) {
        CardText card{};
        card.name = Wide(campaign::NameIn(area, lang));
        std::wstring facts = ActTitle(area.act);
        facts += (facts.empty() ? L"" : L" \u00B7 ") + UiText(TerrainLabel(area.terrain));
        if (area.waypoint) facts += L" \u00B7 " + UiText(L"WP");
        card.facts = facts;
        if (!area.bosses.empty()) {
            std::wstring bosses = UiText(L"Boss") + L": ";
            for (std::size_t b = 0; b < area.bosses.size(); ++b) bosses += (b > 0 ? L", " : L"") + Wide(area.bosses[b]);
            card.bosses = bosses;
        }
        card.entry = CampaignLiveCardEntry(tab, area);  // the official Maps page's lines when it lists the area, else area.entry
        card.nameH = MeasureHeight(dc, card.name, inner - badgeRoom, fontBold);
        card.factsH = MeasureHeight(dc, card.facts, inner, fontSnippet);
        card.bossesH = MeasureHeight(dc, card.bosses, inner, fontSnippet);
        card.entryH = MeasureHeight(dc, card.entry, inner, fontSnippet);
        return card;
    };
    const bool padOn = padActive && !oskOpen && !suggestPadFocus && padZone == PadZone::Campaign;
    const int padSpot = campaignSpots.empty() ? -1 : std::clamp(campaignPadIndex, 0, static_cast<int>(campaignSpots.size()) - 1);
    const bool flashing = CampaignFlashing();
    std::size_t at = 0;
    int cardNumber = 0;
    while (at < tab.cards.size()) {
        const std::size_t end = std::min(tab.cards.size(), at + static_cast<std::size_t>(columns));
        std::vector<CardText> texts;
        int rowH = 0;
        for (std::size_t k = at; k < end; ++k) {
            const campaign::Area* area = campaign::FindArea(data, tab.cards[k]);
            texts.push_back(area != nullptr ? textOf(*area) : CardText{});
            const CardText& t = texts.back();
            const int h = pad + t.nameH + S(2) + t.factsH + (t.bossesH > 0 ? t.bossesH + S(2) : 0) + (t.entryH > 0 ? t.entryH + S(4) : 0) + pad;
            rowH = std::max(rowH, h);
        }
        for (std::size_t k = at; k < end; ++k, ++cardNumber) {
            const int id = tab.cards[k];
            const campaign::Area* area = campaign::FindArea(data, id);
            const int col = static_cast<int>(k - at);
            const RECT card = Rect(left + col * (cardW + gap), y, left + col * (cardW + gap) + cardW, y + rowH);
            if (!draw) {
                campaignSpots.push_back({id, Rect(card.left, card.top - matchBase, card.right, card.bottom - matchBase)});
                continue;
            }
            campaignHits.push_back({card, 1, id});
            if (card.bottom < clipTop || card.top > clipBottom || area == nullptr) continue;
            const CardText& t = texts[k - at];
            const bool selected = id == campaignSelected;
            const bool hot = (campaignHoverKind == 1 && campaignHover == id) || (padOn && padSpot == cardNumber);
            FillPanel(dc, card, selected ? kSelected : kTile, 220);
            FrameColor(dc, card, selected ? kCampaignAmber : hot || std::binary_search(matches.begin(), matches.end(), id) ? kGold : kBorder);
            if (flashing && id == campaignFlashArea)
                for (int grow = 1; grow <= std::max(2, S(2)); ++grow)
                    FrameColor(dc, Rect(card.left - grow, card.top - grow, card.right + grow, card.bottom + grow), RGB(255, 226, 140));
            int ty = card.top + pad;
            const int tx = card.left + pad;
            Text(dc, t.name, Rect(tx, ty, tx + inner - badgeRoom, ty + t.nameH), kGold, fontBold, DT_LEFT | DT_WORDBREAK);
            if (graphics != nullptr) {
                const Gdiplus::GraphicsState saved = graphics->Save();
                graphics->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                const double k15 = 1.15 * static_cast<double>(dpi) / 96.0;
                DrawBadges(*graphics, *area, card.right - pad - badgeRoom + 5 * k15, ty + boldLine / 2.0, k15);
                graphics->Flush(Gdiplus::FlushIntentionSync);
                graphics->Restore(saved);
            }
            ty += t.nameH + S(2);
            Text(dc, t.facts, Rect(tx, ty, tx + inner, ty + t.factsH), kDim, fontSnippet, DT_LEFT | DT_WORDBREAK);
            ty += t.factsH;
            if (t.bossesH > 0) {
                ty += S(2);
                Text(dc, t.bosses, Rect(tx, ty, tx + inner, ty + t.bossesH), kCampaignRed, fontSnippet, DT_LEFT | DT_WORDBREAK);
                ty += t.bossesH;
            }
            if (t.entryH > 0) {
                ty += S(4);
                Text(dc, t.entry, Rect(tx, ty, tx + inner, ty + t.entryH), kBody, fontSnippet, DT_LEFT | DT_WORDBREAK);
            }
        }
        y += rowH + gap;
        at = end;
    }
    return y + S(2);
}

// What to watch out for in an area: its caveat notes, then the teleport and town portal rules.
std::vector<std::wstring> OverlayState::CampaignCaveats(const campaign::Area& area) const {
    std::vector<std::wstring> out;
    const auto mark = [](char source) { return campaign::Unconfirmed(source) ? L" " + UiText(L"(not confirmed yet)") : std::wstring(); };
    for (const campaign::Note& note : area.notes)
        if (note.warning) out.push_back(Wide(note.text) + mark(note.source));
    if (area.teleportOff) out.push_back(UiText(L"Teleport is off here"));
    if (area.teleportLimited) out.push_back(UiText(L"Teleport is limited here"));
    if (!CampaignLiveTeleport(area, out) && area.teleportWalls) out.push_back(UiText(L"No teleport across the walls"));
    if (area.noTownPortal) out.push_back(UiText(L"No town portal"));
    if (area.entryUnknown) out.push_back(UiText(L"No known way in"));
    return out;
}

// The selected area's details, in a framed panel under the map; a hint when nothing is selected.
int OverlayState::RenderCampaignDetails(HDC dc, int left, int paneW, int y, bool draw) {
    const campaign::Area* area = campaignSelected != 0 ? campaign::FindArea(*campaignData, campaignSelected) : nullptr;
    if (area == nullptr) {
        const std::wstring hint = UiText(L"Click an area on the map to see its details.");
        const int h = MeasureHeight(dc, hint, paneW, fontSnippet);
        if (draw) Text(dc, hint, Rect(left, y, left + paneW, y + h), kDim, fontSnippet, DT_LEFT | DT_WORDBREAK);
        return y + h + S(4);
    }
    const int top = y;
    const int bottom = CampaignDetailBody(dc, *area, left + S(12), paneW - S(24), top + S(10), false);
    const int boxH = bottom + S(10) - top;
    if (draw && top <= clipBottom && top + boxH >= clipTop) {
        const RECT panel = Rect(left, top, left + paneW, top + boxH);
        FillPanel(dc, panel, RGB(16, 11, 7), 220);
        FrameColor(dc, panel, kCampaignAmber);
        CampaignDetailBody(dc, *area, left + S(12), paneW - S(24), top + S(10), true);
    }
    return top + boxH;
}

int OverlayState::CampaignDetailBody(HDC dc, const campaign::Area& area, int x, int bodyW, int y, bool draw) {
    const campaign::Data& data = *campaignData;
    const std::string lang = CampaignLang();
    const auto mark = [](char source) { return campaign::Unconfirmed(source) ? L" " + UiText(L"(not confirmed yet)") : std::wstring(); };
    const auto para = [&](const std::wstring& text, COLORREF color, HFONT font, int after) {
        if (text.empty()) return;
        const int h = MeasureHeight(dc, text, bodyW, font);
        if (draw) Text(dc, text, Rect(x, y, x + bodyW, y + h), color, font, DT_LEFT | DT_WORDBREAK);
        y += h + after;
    };
    const auto heading = [&](const std::wstring& text) {
        y += S(6);
        para(text, kHeaderGold, fontBold, S(2));
    };

    // Header: the name, the close button, the official name, what kind of place it is.
    const int closeSize = std::max(S(22), bodyLine + S(6));
    const std::wstring name = Wide(campaign::NameIn(area, lang));
    const int nameH = std::max(closeSize, MeasureHeight(dc, name, bodyW - closeSize - S(8), fontTitle));
    if (draw) {
        Text(dc, name, Rect(x, y, x + bodyW - closeSize - S(8), y + nameH), kHeaderGold, fontTitle, DT_LEFT | DT_WORDBREAK);
        const RECT closeBox = Rect(x + bodyW - closeSize, y, x + bodyW, y + closeSize);
        const bool hot = campaignHoverKind == 4;
        FillPanel(dc, closeBox, hot ? kSelected : kPanel, 220);
        FrameColor(dc, closeBox, hot ? kGold : kBorder);
        CampaignText(dc, L"X", closeBox, kText, fontBold, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        campaignHits.push_back({closeBox, 4, 0});
    }
    y += nameH + S(2);
    if (!area.officialName.empty() && area.officialName != campaign::NameIn(area, lang))
        para(UiFormat(L"Official name: {0}", {Wide(area.officialName)}), kDim, fontSnippet, S(2));
    {
        std::wstring facts = UiText(TerrainLabel(area.terrain));
        const std::wstring act = ActTitle(area.act);
        if (!act.empty()) facts = act + L" \u00B7 " + facts;
        if (area.waypoint) facts += L" \u00B7 " + UiText(L"Waypoint");
        if (area.endgameZone) facts += L" \u00B7 " + UiText(L"Endgame zone");
        para(facts, kDim, fontSnippet, S(2));
    }

    if (!area.quests.empty()) {
        heading(UiText(L"Quest items"));
        for (const campaign::Quest& quest : area.quests) {
            std::wstring line = UiFormat(L"Quest: {0}", {Wide(quest.quest)});
            if (!quest.item.empty()) line += L" \u2014 " + Wide(campaign::ItemName(data, quest.item, lang));
            para(line + mark(quest.source), kCampaignAmber, fontBody, S(1));
            para(Wide(quest.how), kBody, fontSnippet, S(4));
        }
    }
    const std::vector<std::wstring> caveats = CampaignCaveats(area);
    if (!caveats.empty()) {
        heading(UiText(L"Heads up"));
        for (const std::wstring& caveat : caveats) para(L"! " + caveat, kCampaignOrange, fontSnippet, S(2));
    }
    if (!area.bosses.empty()) {
        heading(UiText(L"Boss"));
        std::wstring bosses;
        for (std::size_t b = 0; b < area.bosses.size(); ++b) bosses += (b > 0 ? L", " : L"") + Wide(area.bosses[b]);
        para(bosses, kCampaignRed, fontBody, S(2));
    }
    bool anyNote = false, anySpoiler = false;
    for (const campaign::Note& note : area.notes) {
        if (note.warning) continue;
        if (note.spoiler) anySpoiler = true;
        else anyNote = true;
    }
    if (anyNote || anySpoiler) {
        heading(UiText(L"Notes"));
        for (const campaign::Note& note : area.notes)
            if (!note.warning && (!note.spoiler || campaignSpoilers)) para(L"\u2022 " + Wide(note.text) + mark(note.source), kBody, fontSnippet, S(2));
        if (anySpoiler) {
            const std::wstring toggle = UiText(campaignSpoilers ? L"Spoiler: click to hide" : L"Spoiler: click to show");
            const int h = MeasureHeight(dc, toggle, bodyW, fontSnippet);
            if (draw) {
                const int w = std::min(bodyW, MeasureWidth(dc, toggle, fontSnippet) + S(4));
                const RECT box = Rect(x, y, x + w, y + h);
                CampaignText(dc, toggle, box, campaignHoverKind == 3 ? kGold : kHeaderGold, fontSnippet, DT_LEFT | DT_WORDBREAK);
                campaignHits.push_back({box, 3, 0});
            }
            y += h + S(2);
        }
    }

    // Getting there: how to get in, then a chip for every connected area (a click opens it).
    struct Link { int id; std::wstring text; };
    std::vector<Link> links;
    for (const std::size_t e : campaign::EdgesOf(data, area.id)) {
        const campaign::Edge& edge = data.edges[e];
        int other = edge.from == area.id ? edge.to : edge.from;
        if (const campaign::Area* twin = campaign::FindArea(data, other); twin != nullptr && twin->twinOf != 0) other = twin->twinOf;
        if (other == area.id || std::any_of(links.begin(), links.end(), [other](const Link& link) { return link.id == other; })) continue;
        std::wstring text = Wide(campaign::AreaName(data, other, lang));
        if (edge.oneWay) text = (edge.from == area.id ? L"\u2192 " : L"\u2190 ") + text;
        text += L" \u00B7 " + UiText(EdgeKindLabel(edge.kind));
        if (!edge.via.empty()) text += L" (" + Wide(edge.via) + L")";
        if (campaign::Unconfirmed(edge.source)) text += L" " + UiText(L"(not confirmed yet)");
        links.push_back({other, std::move(text)});
    }
    if (!area.entry.empty() || !links.empty()) {
        heading(UiText(L"Getting there"));
        para(Wide(area.entry) + (area.entry.empty() ? std::wstring() : mark(area.entrySource)), kBody, fontSnippet, S(4));
        const int chipH = snippetLine + S(8);
        int cx = x;
        for (const Link& link : links) {
            const int chipW = std::min(bodyW, MeasureWidth(dc, link.text, fontSnippet) + S(16));
            if (cx > x && cx + chipW > x + bodyW) {
                cx = x;
                y += chipH + S(6);
            }
            if (draw) {
                const RECT chip = Rect(cx, y, cx + chipW, y + chipH);
                const bool hot = campaignHoverKind == 2 && campaignHover == link.id;
                FillPanel(dc, chip, hot ? kSelected : kPanel, 220);
                FrameColor(dc, chip, hot ? kGold : kBorder);
                CampaignText(dc, link.text, Rect(chip.left + S(8), chip.top, chip.right - S(8), chip.bottom), kText, fontSnippet,
                             DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                campaignHits.push_back({chip, 2, link.id});
            }
            cx += chipW + S(6);
        }
        if (!links.empty()) y += chipH + S(4);
    }

    // Monsters: the monster level in each difficulty.
    if (area.level[0] > 0 || area.level[1] > 0 || area.level[2] > 0) {
        heading(UiText(L"Monsters"));
        const int colW = bodyW / 4;
        const int rowH = snippetLine + S(4);
        const wchar_t* const kNames[] = {L"Normal", L"Nightmare", L"Hell"};
        if (draw) {
            for (int d = 0; d < 3; ++d) {
                const RECT cell = Rect(x + colW * (d + 1), y, x + colW * (d + 2), y + rowH);
                CampaignText(dc, UiText(kNames[d]), cell, kDim, fontSnippet, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
            FillColor(dc, Rect(x, y + rowH, x + bodyW, y + rowH + 1), kBorder);
            const RECT labelCell = Rect(x, y + rowH + S(2), x + colW, y + 2 * rowH + S(2));
            CampaignText(dc, UiText(L"Monster level"), labelCell, kBody, fontSnippet, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            for (int d = 0; d < 3; ++d) {
                const RECT cell = Rect(x + colW * (d + 1), y + rowH + S(2), x + colW * (d + 2), y + 2 * rowH + S(2));
                const std::wstring value = area.level[d] > 0 ? std::to_wstring(area.level[d]) : std::wstring(L"-");
                CampaignText(dc, value, cell, kText, fontSnippetBold, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            }
        }
        y += 2 * rowH + S(4);
    }
    return y;
}

// After the layout: scrolls to the area a jump or a link selected, and puts the controller's highlight on it.
void OverlayState::CampaignApplyReveal(int total, int visible) {
    if (campaignPadFollow) {
        campaignPadFollow = false;
        for (std::size_t i = 0; i < campaignSpots.size(); ++i)
            if (campaignSpots[i].area == campaignSelected) { campaignPadIndex = static_cast<int>(i); break; }
    }
    if (campaignPadRevealNext) {
        campaignPadRevealNext = false;
        if (padZone == PadZone::Campaign) CampaignPadReveal();
    }
    if (!campaignReveal) return;
    campaignReveal = false;
    if (!CampaignShowing()) return;
    for (const CampaignSpot& spot : campaignSpots) {
        if (spot.area != campaignSelected) continue;
        PageView& view = CurrentView();
        const int shownTop = spot.rect.top + bodyTop - view.detailScroll;
        const int shownBottom = spot.rect.bottom + bodyTop - view.detailScroll;
        if (shownTop >= clipTop + S(8) && shownBottom + S(40) <= layout.detail.bottom) return;  // already in view
        view.detailScroll = std::clamp(static_cast<int>(spot.rect.top) - visible / 3, 0, std::max(0, total - visible));
        return;
    }
}

// ---- mouse ------------------------------------------------------------------------------------------------------

// The pointer moved over the detail pane. True while the map's entry is showing (its text has no other hover).
bool OverlayState::CampaignMove(int x, int y) {
    if (!CampaignShowing()) return false;
    int kind = -1, value = 0;
    RECT rect{};
    if (y >= clipTop && y <= clipBottom) {
        for (const CampaignHit& hit : campaignHits) {
            if (!Inside(hit.rect, x, y)) continue;
            kind = hit.kind;
            value = hit.value;
            rect = hit.rect;
            break;
        }
        // Not on anything listed: an edge with a "via" caption within S(5) of the pointer (its caption then shows).
        const CampaignGraph& g = campaignGraph;
        if (kind < 0 && campaignGraphShown && g.data == campaignData && g.tab == campaignTab) {
            const campaign::Point at{static_cast<double>(x - campaignGraphAt.x), static_cast<double>(y - campaignGraphAt.y)};
            double nearest = S(5) + 0.5;
            for (std::size_t li = 0; li < g.lines.size(); ++li) {
                if (g.lines[li].via.empty()) continue;
                const double distance = campaign::DistanceToPolyline(at, g.lines[li].points);
                if (distance <= nearest) {
                    nearest = distance;
                    kind = 5;
                    value = static_cast<int>(li);
                }
            }
        }
    }
    if (kind != campaignHoverKind || value != campaignHover) {
        campaignHoverKind = kind;
        campaignHover = value;
        campaignHoverRect = rect;
        InvalidateRect(window, nullptr, FALSE);
    }
    return true;
}

bool OverlayState::CampaignClick(int x, int y) {
    if (!CampaignShowing() || !Inside(layout.detail, x, y) || y < clipTop || y > clipBottom) return false;
    for (const CampaignHit& hit : campaignHits) {
        if (!Inside(hit.rect, x, y)) continue;
        switch (hit.kind) {
        case 0: CampaignSelectTab(hit.value); break;
        case 1: CampaignPick(hit.value); break;
        case 2: CampaignShowArea(hit.value, false); break;
        case 3: campaignSpoilers = !campaignSpoilers; break;
        case 4: campaignSelected = 0; break;
        default: break;
        }
        InvalidateRect(window, nullptr, FALSE);
        return true;
    }
    return false;
}

// ---- tooltip ----------------------------------------------------------------------------------------------------

// The tooltip of the area under the pointer, else the one the controller highlights, else the one a jump arrived at.
// Placed like the item tooltips: beside the target, clear of the cursor, below the search box and its buttons, as wide
// as its longest line (at most 640, scaled), in columns only when even that is taller than the window.
void OverlayState::DrawCampaignTooltip(HDC dc) {
    if (!CampaignShowing() || oskOpen) return;
    int areaId = 0;
    RECT target{};
    if (campaignHoverKind == 1 && campaignHover != 0) {
        areaId = campaignHover;
        target = campaignHoverRect;
    } else if (padActive && !suggestPadFocus && padZone == PadZone::Campaign && GetTickCount64() >= padMouseUntil) {
        if (!CampaignPadRect(target)) return;
        areaId = campaignSpots[static_cast<std::size_t>(std::clamp(campaignPadIndex, 0, static_cast<int>(campaignSpots.size()) - 1))].area;
    } else if (CampaignFlashing()) {
        for (const CampaignSpot& spot : campaignSpots) {
            if (spot.area != campaignFlashArea) continue;
            const int shift = bodyTop - CurrentView().detailScroll;
            target = Rect(spot.rect.left, spot.rect.top + shift, spot.rect.right, spot.rect.bottom + shift);
            areaId = spot.area;
            break;
        }
    }
    if (areaId == 0 || target.bottom < clipTop || target.top > clipBottom) return;
    const campaign::Data& data = *campaignData;
    const campaign::Area* area = campaign::FindArea(data, areaId);
    if (area == nullptr) return;
    const std::string lang = CampaignLang();

    struct TipRow { std::wstring text; COLORREF color; HFONT font; };
    std::vector<TipRow> rows;
    const std::string name = campaign::NameIn(*area, lang);
    rows.push_back({Wide(name), kGold, fontBold});
    if (!area->officialName.empty() && area->officialName != name)
        rows.push_back({UiFormat(L"Official name: {0}", {Wide(area->officialName)}), kDim, fontSmall});
    {
        std::wstring facts = UiText(TerrainLabel(area->terrain));
        if (area->waypoint) facts += L" \u00B7 " + UiText(L"Waypoint");
        if (area->endgameZone) facts += L" \u00B7 " + UiText(L"Endgame zone");
        rows.push_back({facts, kDim, fontBody});
    }
    if (area->level[0] > 0 || area->level[1] > 0 || area->level[2] > 0) {
        rows.push_back({UiText(L"Monster level") + L": " + UiText(L"Normal") + L" " + std::to_wstring(area->level[0]) + L" \u00B7 " +
                            UiText(L"Nightmare") + L" " + std::to_wstring(area->level[1]) + L" \u00B7 " + UiText(L"Hell") + L" " +
                            std::to_wstring(area->level[2]),
                        kBody, fontBody});
    }
    for (const campaign::Quest& quest : area->quests)
        if (!quest.item.empty())
            rows.push_back({UiFormat(L"Quest: {0}", {Wide(quest.quest)}) + L" \u2014 " + Wide(campaign::ItemName(data, quest.item, lang)),
                            kCampaignAmber, fontBody});
    if (!area->bosses.empty()) {
        std::wstring bosses = UiText(L"Boss") + L": ";
        for (std::size_t b = 0; b < area->bosses.size(); ++b) bosses += (b > 0 ? L", " : L"") + Wide(area->bosses[b]);
        rows.push_back({bosses, kCampaignRed, fontBody});
    }
    const std::vector<std::wstring> caveats = CampaignCaveats(*area);
    for (std::size_t c = 0; c < caveats.size() && c < 2; ++c) rows.push_back({L"! " + caveats[c], kCampaignOrange, fontBody});
    if (!CampaignOnTab(campaignTab, areaId)) {
        const int home = CampaignTabOf(areaId);
        if (home >= 0) rows.push_back({UiFormat(L"Opens {0}", {UiText(Wide(data.tabs[static_cast<std::size_t>(home)].title))}), kDim, fontSmall});
    }

    const int gap = S(4);
    RECT client{};
    GetClientRect(window, &client);
    int minTop = S(8);
    for (const RECT* above : {&layout.search, &layout.clearQuery, &layout.matchPrev, &layout.matchNext})
        minTop = std::max(minTop, static_cast<int>(above->bottom) + S(6));
    for (const RECT& tab : layout.tabs) minTop = std::max(minTop, static_cast<int>(tab.bottom) + S(6));  // never over the page tabs
    const int availableHeight = static_cast<int>(client.bottom) - minTop - S(8);
    const int maxTipWidth = std::max(S(340), static_cast<int>(client.right) - S(16));
    int widest = 0;
    for (const TipRow& row : rows) widest = std::max(widest, NaturalWidth(dc, row.text, row.font));
    const int fitWidth = std::min(std::max(widest + S(24) + S(8), S(160)), std::min(maxTipWidth, S(640)));
    std::vector<int> widths{fitWidth};
    for (const int candidate : {S(340), S(420), S(520), S(640)})
        if (candidate > fitWidth) widths.push_back(candidate);
    int tipWidth = fitWidth;
    std::vector<int> heights;
    int totalHeight = S(16);
    for (const int candidate : widths) {
        tipWidth = std::min(candidate, maxTipWidth);
        heights.clear();
        totalHeight = S(16);
        for (const TipRow& row : rows) {
            heights.push_back(MeasureHeight(dc, row.text, tipWidth - S(24), row.font));
            totalHeight += heights.back() + gap;
        }
        if (totalHeight <= availableHeight) break;
    }
    int columnWidth = tipWidth - S(24);
    std::vector<int> rowColumn(rows.size(), 0);
    std::vector<int> rowY(rows.size(), 0);
    {
        int ry = 0;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            rowY[i] = ry;
            ry += heights[i] + gap;
        }
    }
    if (totalHeight > availableHeight && !rows.empty()) {
        for (int columns = 2; columns <= 4; ++columns) {
            const int spare = maxTipWidth - S(24) - (columns - 1) * S(12);
            columnWidth = std::max(S(80), std::min({S(420), spare / columns, widest}));
            int column = 0, ry = 0, tallest = 0;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                heights[i] = MeasureHeight(dc, rows[i].text, columnWidth, rows[i].font);
                if (ry > 0 && ry + heights[i] + S(16) > availableHeight) {
                    ++column;
                    ry = 0;
                }
                rowColumn[i] = column;
                rowY[i] = ry;
                ry += heights[i] + gap;
                tallest = std::max(tallest, ry);
            }
            tipWidth = S(24) + columns * columnWidth + (columns - 1) * S(12);
            totalHeight = tallest + S(16);
            if (column < columns) break;
        }
    }

    const int cursorRight = std::max(gCursorDrawWidth.load() - gCursorHotX.load(), 0) + S(6);
    const int cursorLeft = gCursorHotX.load() + S(6);
    int tipLeft = static_cast<int>(target.right) + cursorRight;
    if (tipLeft + tipWidth > client.right - S(8)) {
        const int leftSide = static_cast<int>(target.left) - cursorLeft - tipWidth;
        tipLeft = leftSide >= S(8) ? leftSide : std::max(S(8), static_cast<int>(client.right) - S(8) - tipWidth);
    }
    int tipTop = static_cast<int>(target.top);
    if (tipTop + totalHeight > client.bottom - S(8)) tipTop = static_cast<int>(client.bottom) - totalHeight - S(8);
    tipTop = std::max(minTop, tipTop);
    const int visibleHeight = std::min(totalHeight, static_cast<int>(client.bottom) - tipTop - S(8));
    const RECT box = Rect(tipLeft, tipTop, tipLeft + tipWidth, tipTop + visibleHeight);
    CornerShadow(dc, box, S(2));
    FillPanel(dc, box, RGB(10, 7, 4), 214);
    FrameColor(dc, box, kGold);
    const int savedDc = SaveDC(dc);
    IntersectClipRect(dc, box.left, box.top, box.right, box.bottom);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const int columnLeft = tipLeft + S(12) + rowColumn[i] * (columnWidth + S(12));
        const int rowTop = tipTop + S(8) + rowY[i];
        Text(dc, rows[i].text, Rect(columnLeft, rowTop, columnLeft + columnWidth, rowTop + heights[i]), rows[i].color, rows[i].font,
             DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
    RestoreDC(dc, savedDc);
}

// ---- the controller ---------------------------------------------------------------------------------------------

void OverlayState::PadEnterCampaign() {
    if (campaignSpots.empty()) return;
    padZone = PadZone::Campaign;
    campaignHoverKind = -1;
    campaignHover = 0;
    campaignPadIndex = 0;
    for (std::size_t i = 0; i < campaignSpots.size(); ++i)
        if (campaignSpots[i].area == campaignSelected) { campaignPadIndex = static_cast<int>(i); break; }
    CampaignPadReveal();
}

// The highlighted area's place on screen (from the last layout).
bool OverlayState::CampaignPadRect(RECT& out) const {
    if (campaignSpots.empty() || pageIndex >= views.size()) return false;
    const CampaignSpot& spot = campaignSpots[static_cast<std::size_t>(std::clamp(campaignPadIndex, 0, static_cast<int>(campaignSpots.size()) - 1))];
    const int shift = bodyTop - views[pageIndex].detailScroll;
    out = Rect(spot.rect.left, spot.rect.top + shift, spot.rect.right, spot.rect.bottom + shift);
    return true;
}

// Scrolls the page so the highlighted area (and room for its label) is in view.
void OverlayState::CampaignPadReveal() {
    RECT r{};
    if (!CampaignPadRect(r)) return;
    const int margin = S(8);
    int delta = 0;
    if (r.top < clipTop + margin) delta = static_cast<int>(r.top) - (clipTop + margin);
    else if (r.bottom + S(32) > layout.detail.bottom - margin) delta = static_cast<int>(r.bottom) + S(32) - (static_cast<int>(layout.detail.bottom) - margin);
    if (delta == 0) return;
    PageView& view = CurrentView();
    view.detailScroll = std::clamp(view.detailScroll + delta, 0, std::max(0, scrollTotal - scrollVisible));
    InvalidateRect(window, nullptr, FALSE);
}

// D-pad: to the nearest dot (or card) in that direction. Left of the leftmost one: the categories.
void OverlayState::CampaignPadMove(int dx, int dy) {
    if (!CampaignShowing() || campaignSpots.empty()) {
        padZone = PadZone::List;
        return;
    }
    campaignHoverKind = -1;
    campaignHover = 0;
    const int current = std::clamp(campaignPadIndex, 0, static_cast<int>(campaignSpots.size()) - 1);
    int next = -1;
    const CampaignGraph& g = campaignGraph;
    const bool graph = campaignData->tabs[static_cast<std::size_t>(campaignTab)].grid && g.data == campaignData && g.tab == campaignTab &&
                       g.neighbours.size() == campaignSpots.size();
    if (graph) {
        const campaign::Neighbours& n = g.neighbours[static_cast<std::size_t>(current)];
        next = dx < 0 ? n.left : dx > 0 ? n.right : dy < 0 ? n.up : n.down;
    } else {
        std::vector<esrwiki::pad::Box> boxes;
        for (const CampaignSpot& spot : campaignSpots) boxes.push_back({spot.rect.left, spot.rect.top, spot.rect.right, spot.rect.bottom});
        next = esrwiki::pad::Neighbor(boxes, current, dx, dy);
    }
    if (next < 0 || static_cast<std::size_t>(next) >= campaignSpots.size()) {
        if (dx < 0) padZone = PadZone::List;
        return;
    }
    campaignPadIndex = next;
    CampaignPadReveal();
    InvalidateRect(window, nullptr, FALSE);
}

// A: selects the highlighted area (its details show under the map; a ghost opens its own tab).
void OverlayState::CampaignPadConfirm() {
    if (!CampaignShowing() || campaignSpots.empty()) return;
    const int index = std::clamp(campaignPadIndex, 0, static_cast<int>(campaignSpots.size()) - 1);
    CampaignPick(campaignSpots[static_cast<std::size_t>(index)].area);
    campaignPadFollow = true;
}

// B: closes the details first. False when there were none (the zone is left).
bool OverlayState::CampaignPadBack() {
    if (campaignSelected == 0) return false;
    campaignSelected = 0;
    InvalidateRect(window, nullptr, FALSE);
    return true;
}

// LB / RB while the map has the controller: the previous / next tab. False when the map does not have it.
bool OverlayState::CampaignPadStepTab(int direction) {
    if (padZone != PadZone::Campaign || !CampaignShowing()) return false;
    const int count = static_cast<int>(campaignData->tabs.size());
    CampaignSelectTab(((campaignTab + direction) % count + count) % count);
    campaignPadIndex = 0;
    campaignPadRevealNext = true;  // the first area of the new tab, once it is laid out
    return true;
}
