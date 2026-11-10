// ---------------------------------------------------------------------------------------------------------------
// Search suggestions: a dropdown under the search box that lists cards, entries and pages matching what is typed (in the
// language of the data), and a jump straight to the one picked. The matching is search_index.cpp (the same query
// language and the same matching as the in-page filter); this file draws the list, answers the mouse and keyboard, and
// takes the view to the card. It is included by overlay.cpp, after OverlayState is defined, and uses that file's drawing
// helpers.
//
// Keys while the list is open: Enter opens the picked row, or the top one when none is picked (it is shaded as the
// default); Up / Down and Tab / Shift+Tab pick a row; Shift+Enter, F3 and Shift+F3 step through the matches on the page
// instead; Escape closes the list. Down, or a click on the search box, brings the list back for the text typed.
//
// With a controller (the moves between the places are esrwiki::pad::NextSearchFocus, tested in pad_logic_test.cpp):
// D-pad down from the on-screen keyboard's bottom row, or View (Share / Create) at any time, gives the list the focus;
// then up / down pick a row (the "Did you mean" heading and the no-match message are never picked), up from the first
// row goes back, LB / RB turn a page of rows, A opens the row (and puts the keyboard away), B closes the list. With the
// keyboard showing, the list goes beside it when the window is wide enough, otherwise under it with at least four rows
// (esrwiki::pad::PlacePopups).
// ---------------------------------------------------------------------------------------------------------------

constexpr std::size_t kSuggestMax = 60;  // suggestions kept
constexpr int kSuggestVisible = 8;      // rows showing at once
constexpr UINT kSuggestDelayMs = 100;   // the list is looked up this long after the typing stops
constexpr UINT kSearchNoteMs = 2500;    // how long a note such as "Filters cleared" shows

namespace {

// The line under the rows that says how to use the list with the controller, for where its focus is.
std::wstring PadSuggestHint(bool sony, esrwiki::pad::SearchFocus focus) {
    if (focus == esrwiki::pad::SearchFocus::List)
        return UiFormat(L"{0}: open | {1}: close | {2}: page", {UiText(sony ? L"Cross" : L"A"), UiText(sony ? L"Circle" : L"B"), UiText(sony ? L"L1 / R1" : L"LB / RB")});
    if (focus == esrwiki::pad::SearchFocus::Keyboard) return UiText(L"D-pad down: suggestions");
    return UiFormat(L"{0}: suggestions", {UiText(sony ? L"Share / Create" : L"View")});
}

// Text with some code points in `markColor` and `markFont`, on one line, cut with "..." at maxRight. `marks` are code
// point positions.
void DrawMarkedLineW(HDC dc, const std::wstring& wide, const std::vector<int>& marks, int x, int top, int bottom, int maxRight,
                     HFONT font, HFONT markFont, COLORREF color, COLORREF markColor) {
    std::vector<char> marked(wide.size(), 0);
    if (!marks.empty()) {
        std::size_t unit = 0;
        int point = 0;
        std::size_t next = 0;
        std::vector<int> sorted = marks;
        std::sort(sorted.begin(), sorted.end());
        while (unit < wide.size()) {
            const bool pair = wide[unit] >= 0xD800 && wide[unit] <= 0xDBFF && unit + 1 < wide.size();
            while (next < sorted.size() && sorted[next] < point) ++next;
            if (next < sorted.size() && sorted[next] == point) {
                marked[unit] = 1;
                if (pair) marked[unit + 1] = 1;
            }
            unit += pair ? 2 : 1;
            ++point;
        }
    }
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int y = top + std::max(0, (bottom - top - static_cast<int>(metrics.tmHeight)) / 2);
    SIZE ellipsis{};
    GetTextExtentPoint32W(dc, L"...", 3, &ellipsis);
    std::size_t at = 0;
    while (at < wide.size() && x < maxRight) {
        std::size_t end = at;
        while (end < wide.size() && marked[end] == marked[at]) ++end;
        SelectObject(dc, marked[at] ? markFont : font);
        SIZE size{};
        GetTextExtentPoint32W(dc, wide.c_str() + at, static_cast<int>(end - at), &size);
        SetTextColor(dc, marked[at] ? markColor : color);
        if (x + size.cx <= maxRight) {
            TextOutW(dc, x, y, wide.c_str() + at, static_cast<int>(end - at));
            x += size.cx;
            at = end;
            continue;
        }
        // The run does not fit: keep as many characters as leave room for the dots.
        std::size_t keep = end - at;
        while (keep > 0) {
            GetTextExtentPoint32W(dc, wide.c_str() + at, static_cast<int>(keep), &size);
            if (x + size.cx + ellipsis.cx <= maxRight) break;
            --keep;
        }
        if (keep > 0) {
            TextOutW(dc, x, y, wide.c_str() + at, static_cast<int>(keep));
            x += size.cx;
        }
        SelectObject(dc, font);
        SetTextColor(dc, color);
        TextOutW(dc, x, y, L"...", 3);
        break;
    }
    SelectObject(dc, previous);
}

void DrawMarkedLine(HDC dc, const std::string& utf8, const std::vector<int>& marks, int x, int top, int bottom, int maxRight,
                    HFONT font, HFONT markFont, COLORREF color, COLORREF markColor) {
    DrawMarkedLineW(dc, Wide(utf8), marks, x, top, bottom, maxRight, font, markFont, color, markColor);
}

// Plain text on one line that ends at `right` (cut with "..." when wider than `maxWidth`).
void DrawLineRight(HDC dc, const std::wstring& text, int right, int maxWidth, int top, int bottom, HFONT font, COLORREF color) {
    const int width = std::min(MeasureWidth(dc, text, font), maxWidth);
    DrawMarkedLineW(dc, text, {}, right - width, top, bottom, right, font, font, color, color);
}

// "Page > Entry", right-aligned to end at `right` and at most `maxWidth` wide. The entry name is the part that is kept:
// when the whole does not fit, the page name is cut first, and only an entry name too long by itself is cut at its end.
void DrawContext(HDC dc, const std::wstring& page, const std::wstring& entry, int right, int maxWidth, int top, int bottom,
                 HFONT font, COLORREF color) {
    if (maxWidth <= 0) return;
    if (entry.empty()) {
        DrawLineRight(dc, page, right, maxWidth, top, bottom, font, color);
        return;
    }
    const std::wstring tail = L" \u203A " + entry;
    const int pageWidth = MeasureWidth(dc, page, font);
    const int tailWidth = MeasureWidth(dc, tail, font);
    if (page.empty() || pageWidth + tailWidth <= maxWidth) {
        DrawLineRight(dc, page.empty() ? entry : page + tail, right, maxWidth, top, bottom, font, color);
        return;
    }
    const int pageRoom = maxWidth - tailWidth;
    if (pageRoom >= 3 * MeasureWidth(dc, L"...", font)) {
        const int x = right - maxWidth;
        DrawMarkedLineW(dc, page, {}, x, top, bottom, x + pageRoom, font, font, color, color);
        DrawMarkedLineW(dc, tail, {}, right - tailWidth, top, bottom, right, font, font, color, color);
    } else {
        DrawLineRight(dc, entry, right, maxWidth, top, bottom, font, color);
    }
}

std::wstring JoinTerms(const std::vector<std::string>& terms) {
    std::wstring joined;
    for (const std::string& term : terms) {
        if (!joined.empty()) joined += L", ";
        joined += Wide(term);
    }
    return joined;
}

// What the list says when nothing matches; empty when it should say nothing (a query too short to look up).
std::wstring HintMessage(const esrwiki::search::QueryHint& hint) {
    if (!hint.valid) return hint.excluded.empty() ? std::wstring() : UiText(L"Type a word to look for: -word only leaves things out");
    if (hint.matches > 0) return std::wstring();
    if (!hint.missing.empty()) return UiFormat(L"Nothing has: {0}", {JoinTerms(hint.missing)});
    if (hint.required.size() > 1) return UiFormat(L"No card has all of: {0}", {JoinTerms(hint.required)});
    if (!hint.excluded.empty()) return UiFormat(L"Nothing left without: {0}", {JoinTerms(hint.excluded)});
    return UiText(L"No matches");
}

}  // namespace

// The index of the database, the Specializations board's perks (overlay_spec.inc) and the Campaign Progression map's areas
// (overlay_campaign.inc) as its extra records, each its own source. Each part is made again only when what it was made from
// changed: the database (a new language or a reload), the board's data or the map's data.
void OverlayState::EnsureSearchIndex() {
    if (database == nullptr) return;
    if (searchIndexFor != database || searchIndex.Empty()) {
        searchIndex.Build(*database);
        searchIndexFor = database;
    }
    if (specIndexed != specData || specIndexedFor != database) {
        searchIndex.SetExtra(kExtraPerks, SpecSearchRecords());
        specIndexed = specData;
        specIndexedFor = database;
    }
    if (campaignIndexed != campaignData || campaignIndexedFor != database) {
        searchIndex.SetExtra(kExtraAreas, CampaignSearchRecords());
        campaignIndexed = campaignData;
        campaignIndexedFor = database;
    }
}

void OverlayState::CloseSuggest() {
    suggestPadFocus = false;
    if (suggestDirty) {
        KillTimer(window, kTimerSuggest);
        suggestDirty = false;
    }
    if (!suggestOpen && suggestHover < 0) return;
    suggestOpen = false;
    suggestHover = -1;
    suggestSel = -1;
    InvalidateRect(window, nullptr, FALSE);
}

// The search text changed: look the list up once the typing stops (an empty text closes it at once).
void OverlayState::ScheduleSuggest() {
    if (suggestMute) return;
    if (query.empty()) {
        KillTimer(window, kTimerSuggest);
        suggestDirty = false;
        UpdateSuggest();
        return;
    }
    suggestDirty = true;
    SetTimer(window, kTimerSuggest, kSuggestDelayMs, nullptr);  // the same id again restarts the wait
}

// A key or click needs the list for the text as it is now, not as it was 100 ms ago.
void OverlayState::FlushSuggest() {
    if (!suggestDirty) return;
    KillTimer(window, kTimerSuggest);
    suggestDirty = false;
    UpdateSuggest();
    InvalidateRect(window, nullptr, FALSE);
}

// Brings the list back for the text typed (after Escape or a click elsewhere closed it). True when it is showing.
bool OverlayState::ReopenSuggest() {
    if (query.empty()) return false;
    if (suggestOpen && !suggestDirty) return true;
    if (suggestDirty) FlushSuggest();
    else if (suggestQuery != query || searchIndexFor != database || specIndexed != specData || campaignIndexed != campaignData) UpdateSuggest();
    else if (!suggest.empty() || !suggestMessage.empty()) {
        suggestOpen = true;
        suggestSel = -1;
        suggestHover = -1;
        suggestScroll = 0;
    }
    InvalidateRect(window, nullptr, FALSE);
    return suggestOpen;
}

// Looks up what the search text could mean.
void OverlayState::UpdateSuggest() {
    if (suggestMute) return;
    suggestRows.clear();  // a click or key before the next paint must not index rows of the old list
    suggestRowIndex.clear();
    suggestDirty = false;
    suggestPadFocus = false;  // new rows: the controller picks again from the top
    suggestMessage.clear();
    suggestDivider = -1;
    if (query.empty() || database == nullptr) {
        suggest.clear();
        suggestQuery.clear();
        CloseSuggest();
        return;
    }
    AdoptSpecData();  // Specializations data that came before the window could be told (never while the board is drawing)
    AdoptCampaignData();  // the same for the campaign map
    EnsureSearchIndex();
    suggestQuery = query;
    suggest = searchIndex.Query(*database, query, pageIndex, kSuggestMax);
    suggestScroll = 0;
    suggestSel = -1;
    suggestHover = -1;
    for (std::size_t i = 0; i < suggest.size(); ++i) {
        if (!suggest[i].exact) {  // typo hits are ranked below every exact one: the divider goes above the first
            suggestDivider = static_cast<int>(i);
            break;
        }
    }
    if (suggest.empty()) suggestMessage = HintMessage(searchIndex.Hint(query));
    suggestOpen = !suggest.empty() || !suggestMessage.empty();
}

// The list's rows: the hits, with a "Did you mean" divider before the first typo hit.
int OverlayState::SuggestDisplayCount() const {
    return static_cast<int>(suggest.size()) + (suggestDivider >= 0 ? 1 : 0);
}

int OverlayState::SuggestDisplayRow(int hitIndex) const {
    return hitIndex + (suggestDivider >= 0 && hitIndex >= suggestDivider ? 1 : 0);
}

// The hit a row shows; -1 for the "Did you mean" row (and for no row).
int OverlayState::SuggestHitOfRow(int row) const {
    if (row < 0 || (suggestDivider >= 0 && row == suggestDivider)) return -1;
    return row - (suggestDivider >= 0 && row > suggestDivider ? 1 : 0);
}

// The rows as drawn, and which of them can be opened: every hit, not the "Did you mean" row, not the message shown when
// nothing matches. Empty while the list is closed.
std::vector<bool> OverlayState::SuggestPickRows() const {
    if (!suggestOpen) return {};
    if (suggest.empty()) return std::vector<bool>(suggestMessage.empty() ? 0u : 1u, false);
    std::vector<bool> rows(static_cast<std::size_t>(SuggestDisplayCount()), true);
    if (suggestDivider >= 0 && static_cast<std::size_t>(suggestDivider) < rows.size()) rows[static_cast<std::size_t>(suggestDivider)] = false;
    return rows;
}

// Where the on-screen keyboard and the list go this paint (esrwiki::pad::PlacePopups): the list beside the keyboard when
// the window is wide enough, otherwise under it with at least four rows (the keys get shorter for them); never over the
// keyboard, and beside it never above the top tooltips keep to (under the search box and its buttons). Without the keyboard
// the list hangs under the search box as before. DrawOsk and DrawSuggest both use it.
void OverlayState::PlanSearchPopups() {
    RECT client{};
    GetClientRect(window, &client);
    if (oskOpen && oskRows.empty()) oskRows = esrwiki::pad::OskLayout(oskAccents, gKeyboardLocale);
    oskUnits = 20;
    for (const auto& row : oskRows) oskUnits = std::max(oskUnits, esrwiki::pad::RowSpan(row));
    oskUnit = std::max(S(12), static_cast<int>(S(17) * uiScale));
    oskBoxWidth = std::max(oskUnits * oskUnit + 2 * S(8), listRightX - listLeftX);
    const int clientRight = static_cast<int>(client.right);
    const int clientBottom = static_cast<int>(client.bottom);
    const int searchLeft = static_cast<int>(layout.search.left);
    const int searchBottom = static_cast<int>(layout.search.bottom);
    int floorTop = S(8);  // as DrawTooltip's minimum top
    for (const RECT* above : {&layout.search, &layout.clearQuery, &layout.matchPrev, &layout.matchNext})
        floorTop = std::max(floorTop, static_cast<int>(above->bottom) + S(6));
    esrwiki::pad::PopupInput in;
    in.clientWidth = clientRight;
    in.clientHeight = clientBottom;
    in.edge = S(8);
    in.gap = S(8);
    in.belowGap = S(2);
    in.floorTop = floorTop;
    in.keyboard = oskOpen;
    in.keyboardLeft = searchLeft;
    in.keyboardTop = searchBottom + S(4);
    in.keyboardWidth = oskBoxWidth;
    in.keyRows = static_cast<int>(oskRows.size());
    in.keyHeight = std::max(S(32), bodyLine + S(16));
    in.minKeyHeight = std::min(in.keyHeight, std::max(S(24), bodyLine + S(8)));
    in.keyGap = S(4);
    in.keyPad = S(8);
    in.belowTop = searchBottom + S(2);
    in.listLeft = searchLeft;
    in.listWidth = std::max(S(200), std::min(std::max(static_cast<int>(layout.search.right) - searchLeft, S(560)), clientRight - searchLeft - S(10)));
    in.minSideWidth = std::min(in.listWidth, S(360));
    in.rowHeight = boldLine + snippetLine + S(10);
    in.chrome = S(6) + smallLine + S(6);
    // No rows while the list is not showing, so the keyboard keeps its full key height.
    in.rows = (!suggestOpen || (suggest.empty() && suggestMessage.empty())) ? 0 : (suggest.empty() ? 1 : SuggestDisplayCount());
    in.maxRows = kSuggestVisible;
    in.minRows = 4;
    popupPlace = esrwiki::pad::PlacePopups(in);
}

void OverlayState::DrawSuggest(HDC dc) {
    suggestRows.clear();
    suggestRowIndex.clear();
    if (!suggestOpen || (suggest.empty() && suggestMessage.empty())) return;
    PlanSearchPopups();  // beside or under the keyboard, or under the search box
    const int left = popupPlace.left;
    const int boxWidth = popupPlace.width;
    const int top = popupPlace.top;

    if (suggest.empty()) {  // nothing found: say why, on one row
        suggestRect = Rect(left, top, left + boxWidth, top + std::max(boldLine, snippetLine) + S(14));
        DropShadow(dc, suggestRect, S(3));
        FillPanel(dc, suggestRect, RGB(10, 7, 4), 246);
        FrameColor(dc, suggestRect, kBorder);
        DrawMarkedLineW(dc, suggestMessage, {}, suggestRect.left + S(11), suggestRect.top, suggestRect.bottom,
                        suggestRect.right - S(11), fontSnippet, fontSnippet, kSnippetText, kSnippetText);
        return;
    }

    const int rowH = boldLine + snippetLine + S(10);
    const int hitCount = static_cast<int>(suggest.size());
    const int displayCount = SuggestDisplayCount();
    const int footer = smallLine + S(6);  // "57 results" under the rows
    // A window of rows (as many as fit, at most 8, at least 4 where the window allows) over the whole list; the wheel, the
    // keys, the controller and the thumb move it.
    const int rows = std::clamp(popupPlace.rows, 1, std::max(1, std::min(displayCount, kSuggestVisible)));
    suggestVisible = rows;
    suggestScroll = std::clamp(suggestScroll, 0, std::max(0, displayCount - rows));
    suggestRect = Rect(left, top, left + boxWidth, top + rows * rowH + S(6) + footer);
    DropShadow(dc, suggestRect, S(3));
    FillPanel(dc, suggestRect, RGB(10, 7, 4), 246);
    FrameColor(dc, suggestRect, kGold);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, suggestRect.left, suggestRect.top, suggestRect.right, suggestRect.bottom);
    const bool scrolls = displayCount > rows;
    const int rowRight = static_cast<int>(suggestRect.right) - S(3) - (scrolls ? S(8) : 0);  // room for the scroll thumb
    const int pad = S(8);
    // What each row is, in a small frame before its name, as wide as the widest so the names line up.
    const std::wstring tagTab = UiText(L"Tab"), tagEntry = UiText(L"Entry"), tagCard = UiText(L"Card"), tagPerk = UiText(L"Perk");
    const std::wstring tagArea = UiText(L"Area");  // an area of the Campaign Progression map
    const int tagWidth = std::max({MeasureWidth(dc, tagTab, fontSmall), MeasureWidth(dc, tagEntry, fontSmall),
                                   MeasureWidth(dc, tagCard, fontSmall), MeasureWidth(dc, tagPerk, fontSmall),
                                   MeasureWidth(dc, tagArea, fontSmall)}) + S(10);
    for (int r = 0; r < rows; ++r) {
        const int d = suggestScroll + r;
        const RECT row = Rect(static_cast<int>(suggestRect.left) + S(3), static_cast<int>(suggestRect.top) + S(3) + r * rowH, rowRight,
                              static_cast<int>(suggestRect.top) + S(3) + (r + 1) * rowH);
        if (suggestDivider >= 0 && d == suggestDivider) {  // "Did you mean" and a rule: the hits below are spelled differently
            const std::wstring caption = UiText(L"Did you mean");
            const int captionWidth = MeasureWidth(dc, caption, fontSnippet);
            DrawMarkedLineW(dc, caption, {}, row.left + pad, row.top, row.bottom, row.right - pad, fontSnippet, fontSnippet, kGold, kGold);
            const int middle = (static_cast<int>(row.top) + static_cast<int>(row.bottom)) / 2;
            if (row.left + pad + captionWidth + S(8) < row.right - pad)
                FillColor(dc, Rect(row.left + pad + captionWidth + S(8), middle, row.right - pad, middle + std::max(1, S(1))), kBorder);
            continue;
        }
        const int i = d - (suggestDivider >= 0 && d > suggestDivider ? 1 : 0);
        if (i < 0 || i >= hitCount) continue;
        const esrwiki::search::Hit& hit = suggest[static_cast<std::size_t>(i)];
        suggestRows.push_back(row);
        suggestRowIndex.push_back(i);
        if (i == suggestSel) {  // the row Enter opens: picked with the keys or the pointer, whichever was last
            FillColor(dc, row, kSelected);
            FrameColor(dc, row, kGold);
            if (suggestPadFocus && padActive) {  // the controller's focus: the bright frame the D-pad uses everywhere
                for (int grow : {S(1), S(2)})
                    FrameColor(dc, Rect(row.left - grow, row.top - grow, row.right + grow, row.bottom + grow), RGB(255, 240, 180));
            }
        } else if (suggestSel < 0 && i == 0) {  // nothing picked: Enter opens the top row
            FillPanel(dc, row, kSelected, 120);
        }
        const int lineTop = row.top + S(2);
        const int firstBottom = lineTop + boldLine + S(2);
        const std::wstring& tag = hit.kind == esrwiki::search::HitKind::Perk ? (hit.extraSource == kExtraAreas ? tagArea : tagPerk)
                                  : hit.kind == esrwiki::search::HitKind::Tab ? tagTab
                                  : hit.kind == esrwiki::search::HitKind::Entry ? tagEntry : tagCard;
        const RECT tagRect = Rect(row.left + pad, lineTop + S(3), row.left + pad + tagWidth, firstBottom - S(3));
        FrameColor(dc, tagRect, kBorder);
        DrawMarkedLineW(dc, tag, {}, tagRect.left + (tagWidth - MeasureWidth(dc, tag, fontSmall)) / 2, tagRect.top, tagRect.bottom,
                        tagRect.right, fontSmall, fontSmall, kDim, kDim);
        const int labelLeft = tagRect.right + S(8);
        int labelRight = row.right - pad;
        if (!hit.context.empty()) {
            // Where it is, dim, at the right end of the first line: "Page > Entry", the names as the tabs and the list show them.
            static const std::string kSeparator = " \xE2\x80\xBA ";
            const std::size_t cut = hit.context.find(kSeparator);
            const std::wstring pageName = UiDataText(Wide(hit.context.substr(0, cut)));
            const std::wstring entryName = cut == std::string::npos ? std::wstring() : UiDataText(Wide(hit.context.substr(cut + kSeparator.size())));
            const int room = (static_cast<int>(row.right) - labelLeft) * 9 / 20;
            const int wanted = MeasureWidth(dc, entryName.empty() ? pageName : pageName + L" \u203A " + entryName, fontSmall);
            const int whereWidth = std::min(wanted, room);
            DrawContext(dc, pageName, entryName, row.right - pad, whereWidth, lineTop, firstBottom, fontSmall, kDim);
            labelRight = row.right - pad - whereWidth - S(10);
        }
        DrawMarkedLine(dc, hit.label, hit.labelMarks, labelLeft, lineTop, firstBottom, labelRight, fontBold, fontBold, kText, kGold);
        if (!hit.snippet.empty())
            DrawMarkedLine(dc, hit.snippet, hit.snippetMarks, labelLeft, firstBottom, row.bottom - S(1), row.right - pad, fontSnippet,
                           fontSnippetBold, kSnippetText, kGold);
    }
    const int trackTop = static_cast<int>(suggestRect.top) + S(3), trackBottom = static_cast<int>(suggestRect.top) + S(3) + rows * rowH;
    if (scrolls) {
        const int thumb = std::max(S(14), (trackBottom - trackTop) * rows / displayCount);
        const int offset = (trackBottom - trackTop - thumb) * suggestScroll / std::max(1, displayCount - rows);
        FillColor(dc, Rect(suggestRect.right - S(8), trackTop + offset, suggestRect.right - S(4), trackTop + offset + thumb), kBorder);
    }
    const std::wstring countText = hitCount == 1 ? UiText(L"1 result")
        : UiFormat(L"{0} results", {std::to_wstring(hitCount) + (static_cast<std::size_t>(hitCount) >= kSuggestMax ? L"+" : L"")});
    DrawLineRight(dc, countText, suggestRect.right - S(8), boxWidth - S(16), trackBottom, suggestRect.bottom - S(1), fontSmall, kDim);
    if (padActive) {  // left of the counter: how to use the list with the controller from where the focus is
        const std::wstring hint = PadSuggestHint(padKind == esrwiki::pad::Kind::PlayStation, PadSearchFocus());
        const int hintRight = static_cast<int>(suggestRect.right) - S(8) - MeasureWidth(dc, countText, fontSmall) - S(12);
        DrawMarkedLineW(dc, hint, {}, suggestRect.left + S(8), trackBottom, suggestRect.bottom - S(1), hintRight, fontSmall, fontSmall,
                        suggestPadFocus ? kGold : kDim, kDim);
    }
    RestoreDC(dc, saved);
}

bool OverlayState::SuggestClick(int x, int y) {
    if (!suggestOpen) return false;
    if (suggestDirty && Inside(suggestRect, x, y)) {  // the rows are about to change: show the new ones, open nothing
        FlushSuggest();
        return true;
    }
    for (std::size_t i = 0; i < suggestRows.size(); ++i) {
        if (Inside(suggestRows[i], x, y)) {
            const int hitIndex = suggestRowIndex[i];
            if (hitIndex < 0 || hitIndex >= static_cast<int>(suggest.size())) return true;
            const esrwiki::search::Hit hit = suggest[static_cast<std::size_t>(hitIndex)];  // copied: the jump rebuilds the list
            JumpTo(hit);
            return true;
        }
    }
    if (Inside(suggestRect, x, y)) return true;
    if (Inside(layout.search, x, y)) return false;  // the search box keeps the list
    CloseSuggest();
    return false;
}

// The pointer over the list picks the row under it, so one row is highlighted and Enter opens the row the user last
// pointed at or keyed to.
bool OverlayState::SuggestMove(int x, int y) {
    if (!suggestOpen || !Inside(suggestRect, x, y)) {
        suggestHover = -1;
        return false;
    }
    int row = -1;
    for (std::size_t i = 0; i < suggestRows.size(); ++i)
        if (Inside(suggestRows[i], x, y)) row = suggestRowIndex[i];
    if (row != suggestHover) {
        suggestHover = row;
        if (row >= 0 && row != suggestSel) {
            suggestSel = row;
            InvalidateRect(window, nullptr, FALSE);
        }
    }
    ClearHover();
    return true;
}

// Keys for the list (see the top of this file). False when the key is not for the list.
bool OverlayState::SuggestKey(WPARAM key) {
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (key == VK_ESCAPE) {
        const bool had = suggestOpen || suggestDirty;
        CloseSuggest();
        return had;
    }
    if (key == VK_RETURN && CalcActive() && calcNumFocus >= 0) return false;  // Enter finishes a calculator number
    FlushSuggest();
    if (!suggestOpen) {
        // Down in the search box brings the list back for the text typed.
        if (key == VK_DOWN && GetFocus() == search && !query.empty()) return ReopenSuggest() && !suggest.empty();
        return false;
    }
    if (suggest.empty()) return false;  // only a message is showing
    const int count = static_cast<int>(suggest.size());
    switch (key) {
    case VK_DOWN:
        suggestSel = std::min(suggestSel + 1, count - 1);
        break;
    case VK_UP:
        suggestSel = suggestSel <= 0 ? -1 : suggestSel - 1;
        break;
    case VK_TAB:
        if (shift) suggestSel = suggestSel <= 0 ? count - 1 : suggestSel - 1;
        else suggestSel = suggestSel + 1 >= count ? 0 : suggestSel + 1;
        break;
    case VK_RETURN:
        if (shift) {  // Shift+Enter: the previous match on the page, as with the list closed
            CloseSuggest();
            StepMatch(-1);
            return true;
        }
        {
            const int pick = suggestSel >= 0 && suggestSel < count ? suggestSel : 0;
            const esrwiki::search::Hit hit = suggest[static_cast<std::size_t>(pick)];
            JumpTo(hit);
        }
        return true;
    case VK_NEXT:
        suggestSel = std::min(std::max(suggestSel, -1) + std::max(1, suggestVisible), count - 1);
        break;
    case VK_PRIOR:
        suggestSel = suggestSel <= 0 ? -1 : std::max(0, suggestSel - std::max(1, suggestVisible));
        break;
    default:
        return false;
    }
    SuggestReveal();
    suggestHover = -1;
    InvalidateRect(window, nullptr, FALSE);
    return true;
}

// Scrolls the list so the picked row shows (and the "Did you mean" divider above the first typo hit with it).
void OverlayState::SuggestReveal() {
    if (suggestSel < 0) {
        suggestScroll = 0;
        return;
    }
    const int rows = std::max(1, suggestVisible);
    const int at = SuggestDisplayRow(suggestSel);
    const int first = suggestDivider >= 0 && suggestSel == suggestDivider ? at - 1 : at;
    if (first < suggestScroll) suggestScroll = first;
    else if (at >= suggestScroll + rows) suggestScroll = at - rows + 1;
}

// The wheel over the list. True when it was over the list (it takes the wheel even at the ends).
bool OverlayState::SuggestWheel(int x, int y, int steps) {
    if (!suggestOpen || !Inside(suggestRect, x, y)) return false;
    suggestScroll = std::clamp(suggestScroll - steps, 0, std::max(0, SuggestDisplayCount() - std::max(1, suggestVisible)));
    InvalidateRect(window, nullptr, FALSE);
    return true;
}

// A short note in place of the list's counter ("Filters cleared"), for a moment.
void OverlayState::ShowSearchNote(const std::wstring& note) {
    searchNote = note;
    searchNoteUntil = GetTickCount64() + kSearchNoteMs;
    SetTimer(window, kTimerNote, kSearchNoteMs + 50, nullptr);
    InvalidateRect(window, nullptr, FALSE);
}

// Takes the view to a hit: its tab, its entry, and the card scrolled to the top and framed for a moment. The list and the
// suggestions match the same way, so the search text stays as typed and the hit is in the list it filters. Only a typo or
// letters-in-order hit does not contain the text: then the target's name replaces it. Nothing is changed silently: when a
// filter button would hide the card, or the text has to go to show the target, a note says so.
void OverlayState::JumpTo(const esrwiki::search::Hit& hit) {
    suggestOpen = false;
    suggestPadFocus = false;
    suggestHover = -1;
    suggestSel = -1;
    if (suggestDirty) {
        KillTimer(window, kTimerSuggest);
        suggestDirty = false;
    }
    if (hit.page < 0 || static_cast<std::size_t>(hit.page) >= database->pages.size()) return;
    const auto setText = [&](const std::string& value) {
        suggestMute = true;
        SetWindowTextW(search, Wide(value).c_str());  // EN_CHANGE refilters
        suggestMute = false;
    };
    std::wstring note;
    if (!hit.exact) setText(hit.label);
    SetPage(static_cast<std::size_t>(hit.page));
    Refilter();  // the page's list for the text as it is now (also covers a jump within the same page)
    padZone = PadZone::List;
    if (hit.kind == esrwiki::search::HitKind::Perk && hit.extraSource == kExtraAreas) {
        // An area of the Campaign Progression map: the Welcome page's entry (kept in the list while an area matches), the
        // area's tab, the area selected and ringed for a moment, and scrolled into view once it is laid out.
        const int entryIndex = CampaignEntryIndex(CurrentPage());
        const auto locate = [&]() {
            for (std::size_t i = 0; i < filtered.size(); ++i)
                if (entryIndex >= 0 && filtered[i] == static_cast<std::size_t>(entryIndex)) return i;
            return filtered.size();
        };
        std::size_t position = locate();
        if (position == filtered.size() && !query.empty()) {  // not expected: the area matches the text
            setText(std::string());
            Refilter();
            position = locate();
            note = UiText(L"Search cleared");
        }
        if (position < filtered.size()) {
            SelectFiltered(position);
            CampaignShowArea(CampaignAreaOfHit(hit), true);
        }
    } else if (hit.kind == esrwiki::search::HitKind::Perk) {
        // A perk of the Specializations board: the page (its placeholder entry stays in the list while a perk matches), the
        // perk's tab, and the perk framed for a moment with its tooltip. The board fits the pane, so nothing is scrolled.
        if (!query.empty() && filtered.empty()) {  // not expected: the perk matches the text
            setText(std::string());
            Refilter();
            note = UiText(L"Search cleared");
        }
        if (!filtered.empty()) SelectFiltered(0);
        int tabIndex = -1, perkIndex = -1;
        if (specData) {
            // The hit's place, checked against the board's data by name (new data can have come since the list was made).
            const auto nameAt = [&](int tabAt, int perkAt) {
                if (tabAt < 0 || perkAt < 0 || static_cast<std::size_t>(tabAt) >= specData->tabs.size()) return false;
                const std::vector<spec::Perk>& perks = specData->tabs[static_cast<std::size_t>(tabAt)].perks;
                return static_cast<std::size_t>(perkAt) < perks.size() &&
                       esrwiki::search::StripColorCodes(perks[static_cast<std::size_t>(perkAt)].name) == hit.label;
            };
            if (nameAt(hit.extraGroup, hit.extraItem)) {
                tabIndex = hit.extraGroup;
                perkIndex = hit.extraItem;
            }
            for (std::size_t t = 0; tabIndex < 0 && t < specData->tabs.size(); ++t)
                for (std::size_t i = 0; tabIndex < 0 && i < specData->tabs[t].perks.size(); ++i)
                    if (nameAt(static_cast<int>(t), static_cast<int>(i))) {
                        tabIndex = static_cast<int>(t);
                        perkIndex = static_cast<int>(i);
                    }
        }
        if (tabIndex >= 0) {
            SpecSelectTab(tabIndex);
            specFlashTab = tabIndex;
            specFlashPerk = perkIndex;
            specFlashUntil = GetTickCount64() + 1800;
            SetTimer(window, kTimerFlash, 1900, nullptr);
        }
    } else if (hit.entry < 0) {
        // A tab: its list stays filtered by the text when it has entries for it; otherwise the whole tab shows.
        if (!query.empty() && filtered.empty()) {
            setText(std::string());
            Refilter();
            note = UiText(L"Search cleared");
        }
    } else if (static_cast<std::size_t>(hit.entry) < CurrentPage().entries.size()) {
        const auto locate = [&]() {
            for (std::size_t i = 0; i < filtered.size(); ++i)
                if (filtered[i] == static_cast<std::size_t>(hit.entry)) return i;
            return filtered.size();
        };
        std::size_t position = locate();
        if (position == filtered.size() && !query.empty()) {
            // Not expected (the list keeps every entry the suggestions find), but if it happens the text goes, with a note.
            setText(std::string());
            Refilter();
            position = locate();
            note = UiText(L"Search cleared");
        }
        if (position < filtered.size()) {
            SelectFiltered(position);
            PageView& view = CurrentView();
            if (hit.block >= 0) {
                // The card must not be hidden by a filter button: those are reset only when one of them hides it.
                const Entry& entry = CurrentPage().entries[static_cast<std::size_t>(hit.entry)];
                const std::vector<std::size_t> order =
                    ArrangeBlocks(entry, view.socketFilter, ActiveFilter(entry, &Block::tier, view.tierFilter),
                                  ActiveFilter(entry, &Block::family, view.familyFilter), view.sort, query);
                if (std::find(order.begin(), order.end(), static_cast<std::size_t>(hit.block)) == order.end()) {
                    view.socketFilter = 0;
                    view.tierFilter.clear();
                    view.familyFilter.clear();
                    if (note.empty()) note = UiText(L"Filters cleared");
                }
            }
            jumpPending = true;
            jumpEntry = static_cast<std::size_t>(hit.entry);
            jumpBlock = hit.block;
        }
    }
    if (!note.empty()) ShowSearchNote(note);
    InvalidateRect(window, nullptr, FALSE);
}

// After a layout: if a jump is waiting for its card, scroll to it and frame it.
void OverlayState::ApplyJump(int total, int visible) {
    if (!jumpPending) return;
    PageView& view = CurrentView();
    if (view.selected >= filtered.size() || filtered[view.selected] != jumpEntry) { jumpPending = false; return; }
    jumpPending = false;
    if (jumpBlock < 0) return;
    for (const BlockSpan& span : blockSpans) {
        if (span.block != jumpBlock) continue;
        view.detailScroll = std::clamp(span.top - S(6), 0, std::max(0, total - visible));
        flashBlock = jumpBlock;
        flashUntil = GetTickCount64() + 1800;
        SetTimer(window, kTimerFlash, 1900, nullptr);
        return;
    }
}

// The gold frame round the card a jump arrived at, for a moment.
void OverlayState::DrawJumpFlash(HDC dc) {
    if (flashBlock < 0) return;
    if (GetTickCount64() > flashUntil) { flashBlock = -1; return; }
    for (const BlockSpan& span : blockSpans) {
        if (span.block != flashBlock) continue;
        const int scroll = CurrentView().detailScroll;
        const int top = bodyTop + span.top - scroll - S(3);
        const int bottom = bodyTop + span.bottom - scroll + S(3);
        const RECT frame = Rect(layout.detail.left + S(8), top, layout.detail.right - S(20), bottom);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, layout.detail.left + 1, clipTop, layout.detail.right - S(18), clipBottom);
        const int thickness = std::max(2, S(2));
        for (int grow = 0; grow < thickness; ++grow)
            FrameColor(dc, Rect(frame.left - grow, frame.top - grow, frame.right + grow, frame.bottom + grow), RGB(255, 226, 140));
        RestoreDC(dc, saved);
        return;
    }
}
