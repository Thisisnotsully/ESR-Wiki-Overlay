// ---------------------------------------------------------------------------------------------------------------
// The Specializations page (page id "specializations"): the website's perk board, drawn in the detail pane in place
// of the entry's cards. Everything on it, the words, the tabs, the perks and where they sit, comes from the website
// page (spec_data.cpp); this file only draws it and answers the mouse and the controller. It is included by
// overlay.cpp, after OverlayState is defined, and uses that file's drawing helpers.
//
// The board is the website's: a title, four tabs (one per specialization) and fifteen perks on the shown tab. A perk
// shows its tooltip when the pointer (or the controller's highlight) is on it. Tabs switch the perks.
//
// Search: the Database has only a placeholder for this page, so the perks are given to the search index as extra records
// (SpecSearchRecords, in the language of the page loaded), made again whenever the data, the database or the language
// change. A search hit on a perk opens this page on its tab and frames the perk for a moment, with its tooltip. While
// text is typed, the page stays in the filtered list when a perk matches, and the perks that do not match are dimmed.
// ---------------------------------------------------------------------------------------------------------------

bool OverlayState::SpecActive() const { return !database->pages.empty() && CurrentPage().id == "specializations"; }

void Overlay::SetSpecializationsData(std::shared_ptr<const spec::Data> data) {
    if (!impl_ || !data) return;
    {
        std::lock_guard<std::mutex> guard(impl_->specLock);
        impl_->specPending = std::move(data);
    }
    // Adopted on the UI thread (ApplySpecData); a window not made yet adopts it on its first look-up or paint.
    if (impl_->window != nullptr) PostMessageW(impl_->window, kMsgSpecData, 0, 0);
}

bool OverlayState::AdoptSpecData() {
    std::shared_ptr<const spec::Data> next;
    {
        std::lock_guard<std::mutex> guard(specLock);
        next = std::move(specPending);
        specPending.reset();
    }
    if (!next) return false;
    specData = std::move(next);
    specHoverTab = specHoverPerk = -1;
    specFlashPerk = -1;  // the indices belong to the old data
    return true;  // the search index and SpecMatches see the new pointer and are made again when next needed
}

void OverlayState::ApplySpecData() {
    if (!AdoptSpecData() || database == nullptr) return;
    if (!query.empty()) {
        // The old suggestions and the filtered list were made from the old perks: look the text up again, keeping the
        // list showing if it was.
        const bool suggestWasOpen = suggestOpen;
        Refilter();
        UpdateSuggest();
        if (!suggestWasOpen) suggestOpen = false;
    }
    InvalidateRect(window, nullptr, FALSE);
}

int OverlayState::SpecPageIndex() const {
    if (database == nullptr) return -1;
    for (std::size_t i = 0; i < database->pages.size(); ++i)
        if (database->pages[i].id == "specializations") return static_cast<int>(i);
    return -1;
}

// One record per perk: its name, "<page> > <specialization>", and what the perk does (the tooltip's "Shift + Click" hint is left out: it is the same on every perk).
std::vector<esrwiki::search::ExtraRecord> OverlayState::SpecSearchRecords() const {
    std::vector<esrwiki::search::ExtraRecord> records;
    const int page = SpecPageIndex();
    if (!specData || page < 0) return records;
    const std::string pageName = esrwiki::search::StripColorCodes(database->pages[static_cast<std::size_t>(page)].title);
    const auto addLines = [](std::vector<std::string>& lines, const std::string& text) {
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t end = std::min(text.find('\n', start), text.size());
            if (end > start) lines.push_back(text.substr(start, end - start));
            start = end + 1;
        }
    };
    for (std::size_t t = 0; t < specData->tabs.size(); ++t) {
        const spec::Tab& tab = specData->tabs[t];
        for (std::size_t i = 0; i < tab.perks.size(); ++i) {
            const spec::Perk& perk = tab.perks[i];
            esrwiki::search::ExtraRecord record;
            record.label = perk.name;
            record.context = pageName + " \xE2\x80\xBA " + esrwiki::search::StripColorCodes(tab.label);
            addLines(record.lines, perk.text);
            record.page = page;
            record.group = static_cast<int>(t);
            record.item = static_cast<int>(i);
            records.push_back(std::move(record));
        }
    }
    return records;
}

// Which perks match the search text, from the index's perk records (the same matching as every other record). Made
// again only when the text, the data or the database changed. It never adopts new data itself: RenderSpecializations and
// DrawSpecTooltip hold a reference into specData while they call it.
const std::vector<std::vector<char>>& OverlayState::SpecMatches() {
    if (specMatchValid && specMatchQuery == query && specMatchData == specData && specMatchDb == database) return specMatch;
    specMatchValid = true;
    specMatchQuery = query;
    specMatchData = specData;
    specMatchDb = database;
    specMatch.clear();
    specMatchCount = 0;
    if (query.empty() || !specData || database == nullptr) return specMatch;
    EnsureSearchIndex();
    specMatch.resize(specData->tabs.size());
    for (std::size_t t = 0; t < specData->tabs.size(); ++t) specMatch[t].assign(specData->tabs[t].perks.size(), 0);
    const std::vector<esrwiki::search::ExtraRecord>& records = searchIndex.Extra();
    for (const std::size_t at : searchIndex.MatchingExtra(query)) {
        if (at >= records.size()) continue;
        const esrwiki::search::ExtraRecord& record = records[at];
        if (record.source != kExtraPerks) continue;  // the campaign map's areas
        if (record.group < 0 || static_cast<std::size_t>(record.group) >= specMatch.size()) continue;
        std::vector<char>& perks = specMatch[static_cast<std::size_t>(record.group)];
        if (record.item < 0 || static_cast<std::size_t>(record.item) >= perks.size()) continue;
        perks[static_cast<std::size_t>(record.item)] = 1;
        ++specMatchCount;
    }
    return specMatch;
}

// The entries of a page that the search text keeps. The board's page has only a placeholder entry, which stays while a
// perk matches, so the board shows and the tab is counted and highlighted as a match.
std::vector<std::size_t> OverlayState::FilterPage(std::size_t index) {
    const Page& page = database->pages[index];
    std::vector<std::size_t> found = FilterEntries(page, query);
    if (found.empty() && !query.empty() && page.id == "specializations") {
        SpecMatches();
        if (specMatchCount > 0)
            for (std::size_t i = 0; i < page.entries.size(); ++i) found.push_back(i);
    }
    // The Campaign Progression entry stays while an area of its map matches (overlay_campaign.inc).
    if (!query.empty() && page.id == "welcome") {
        const int campaignEntry = CampaignEntryIndex(page);
        if (campaignEntry >= 0 && !CampaignMatches().empty()) {
            const std::size_t at = static_cast<std::size_t>(campaignEntry);
            const auto place = std::lower_bound(found.begin(), found.end(), at);
            if (place == found.end() || *place != at) found.insert(place, at);
        }
    }
    return found;
}

bool OverlayState::SpecFlashing() const { return specFlashPerk >= 0 && GetTickCount64() <= specFlashUntil; }

namespace {

std::wstring SpecUpper(std::wstring text) {
    if (!text.empty()) CharUpperBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

}  // namespace

// The tab that is showing (the page's own default until one is picked).
int OverlayState::SpecShownTab() const {
    if (!specData || specData->tabs.empty()) return 0;
    const int last = static_cast<int>(specData->tabs.size()) - 1;
    return std::clamp(specTab < 0 ? specData->defaultTab : specTab, 0, last);
}

void OverlayState::SpecSelectTab(int index) {
    if (!specData || index < 0 || static_cast<std::size_t>(index) >= specData->tabs.size()) return;
    specTab = index;
    specHoverPerk = -1;
    InvalidateRect(window, nullptr, FALSE);
}

int OverlayState::RenderSpecializations(HDC dc, int top, int scroll, bool draw) {
    const RECT area = layout.detail;
    const int left = area.left + S(16);
    const int width_ = area.right - S(22) - left;
    int y = top - scroll;
    const int origin = y;
    if (draw) specHits.clear();

    AdoptSpecData();  // data that came before the window could be told (the index follows on its next look-up)
    const auto line = [&](const std::wstring& text, HFONT font, COLORREF color) {
        const int h = MeasureHeight(dc, text, width_, font);
        if (draw) Text(dc, text, Rect(left, y, left + width_, y + h), color, font, DT_LEFT | DT_WORDBREAK);
        y += h;
    };
    line(specData && !specData->title.empty() ? Wide(specData->title) : UiText(L"Specializations"), fontTitle, kHeaderGold);  // the page's own (localized) title
    y += S(2);
    if (!specData) {
        line(UiText(L"The specializations data is not available."), fontBody, kDim);
        return y - origin;
    }
    const spec::Data& data = *specData;

    // The board, scaled to fit the pane's width (never larger than the page's own size at this screen's scale).
    // It must fit the pane as it is, whole: the width, and the height left under the title (the explanation goes below it).
    const int visibleHeight = layout.detail.bottom - top - S(10);
    const int availableHeight = std::max(S(200), visibleHeight - (y - origin) - S(6));
    const double k = std::max(0.3, std::min({static_cast<double>(width_) / data.width, static_cast<double>(availableHeight) / data.height,
                                             static_cast<double>(dpi) / 96.0}));
    const int boardW = static_cast<int>(data.width * k + 0.5);
    const int boardH = static_cast<int>(data.height * k + 0.5);
    const int bx = left + (width_ - boardW) / 2;
    const int by = y;
    const auto sx = [&](double v) { return bx + static_cast<int>(v * k + 0.5); };
    const auto sy = [&](double v) { return by + static_cast<int>(v * k + 0.5); };
    const auto box = [&](double l, double t, double w, double h) { return Rect(sx(l), sy(t), sx(l + w), sy(t + h)); };
    const int shown = SpecShownTab();

    // While text is typed: the perks that match it (the others are dimmed) and the tabs that have some.
    const std::vector<std::vector<char>>& matches = SpecMatches();
    const bool dimming = specMatchCount > 0 && matches.size() == data.tabs.size();
    const bool flashing = SpecFlashing() && specFlashTab == shown;

    if (draw) {
        auto graphics = std::make_unique<Gdiplus::Graphics>(dc);
        graphics->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics->SetClip(Gdiplus::Rect(area.left + 1, clipTop, area.right - S(18) - area.left - 1, clipBottom - clipTop));

        // the stone background
        FillColor(dc, Rect(bx, by, bx + boardW, by + boardH), RGB(18, 17, 16));
        if (Gdiplus::Bitmap* background = FindImage("spec_background")) {
            graphics->DrawImage(background, Gdiplus::Rect(bx, by, boardW, boardH), 0, 0, static_cast<INT>(background->GetWidth()),
                                static_cast<INT>(background->GetHeight()), Gdiplus::UnitPixel);
        }
        FrameColor(dc, Rect(bx, by, bx + boardW, by + boardH), kBorder);

        // title and the line under the board
        Text(dc, SpecUpper(Wide(data.title)), box(data.titleLeft, data.titleTop, data.titleWidth, data.titleHeight), RGB(226, 186, 104),
             fontBold, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        Text(dc, Wide(data.hint), box(data.hintLeft, data.hintTop, data.hintWidth, data.hintHeight), RGB(222, 200, 150), fontSmall,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        // tabs
        for (std::size_t i = 0; i < data.tabs.size(); ++i) {
            const RECT r = box(data.tabs[i].left, data.tabTop, data.tabWidth, data.tabHeight);
            const bool current = static_cast<int>(i) == shown;
            const bool hot = specHoverTab == static_cast<int>(i);
            const bool matched = dimming && std::find(matches[i].begin(), matches[i].end(), char{1}) != matches[i].end();
            FillPanel(dc, r, hot ? RGB(54, 51, 46) : RGB(39, 37, 35), 255);
            FrameColor(dc, r, current ? RGB(236, 196, 112) : matched ? kGold : hot ? RGB(152, 120, 74) : RGB(118, 92, 56));
            if (current) FrameColor(dc, Rect(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), RGB(120, 98, 56));
            Text(dc, SpecUpper(Wide(data.tabs[i].label)), Rect(r.left + S(4), r.top, r.right - S(4), r.bottom), RGB(222, 200, 150),
                 fontSmall, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            specHits.push_back({r, 0, static_cast<int>(i)});
        }

        // The perks of the shown tab: a dark socket ring, the glow when the perk is hot, then its icon from the tab's sprite sheet.
        const spec::Tab& tab = data.tabs[static_cast<std::size_t>(shown)];
        Gdiplus::Bitmap* sheet = FindImage("spec_" + tab.sheet);
        Gdiplus::Bitmap* glow = FindImage("spec_glow");
        const bool padOnPerk = padActive && !oskOpen && padZone == PadZone::Spec && specPadKind == 1;
        const std::vector<char>* shownMatches = dimming ? &matches[static_cast<std::size_t>(shown)] : nullptr;
        for (std::size_t i = 0; i < tab.perks.size(); ++i) {
            const spec::Perk& perk = tab.perks[i];
            const RECT r = box(perk.x, perk.y, data.perkWidth, data.perkHeight);
            specHits.push_back({r, 1, static_cast<int>(i)});
            const bool flashed = flashing && specFlashPerk == static_cast<int>(i);
            const bool hot = specHoverPerk == static_cast<int>(i) || (specHoverPerk < 0 && padOnPerk && specPadIndex == static_cast<int>(i)) || flashed;
            const bool dimmed = shownMatches != nullptr && i < shownMatches->size() && (*shownMatches)[i] == 0 && !hot;
            const float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
            const float outer = static_cast<float>(44 * k), inner = static_cast<float>(34 * k);
            Gdiplus::LinearGradientBrush ring(Gdiplus::PointF(cx, cy - outer), Gdiplus::PointF(cx, cy + outer), Gdiplus::Color(255, 84, 77, 68),
                                              Gdiplus::Color(255, 28, 26, 24));
            graphics->FillEllipse(&ring, cx - outer, cy - outer, outer * 2, outer * 2);
            Gdiplus::Pen rim(Gdiplus::Color(255, 118, 92, 56), std::max(1.0f, static_cast<float>(1.5 * k)));
            graphics->DrawEllipse(&rim, cx - outer, cy - outer, outer * 2, outer * 2);
            Gdiplus::SolidBrush well(Gdiplus::Color(255, 12, 11, 10));
            graphics->FillEllipse(&well, cx - inner, cy - inner, inner * 2, inner * 2);
            Gdiplus::Pen hole(Gdiplus::Color(255, 0, 0, 0), std::max(1.0f, static_cast<float>(k)));
            graphics->DrawEllipse(&hole, cx - inner, cy - inner, inner * 2, inner * 2);
            if (hot && glow != nullptr) {
                const float g = static_cast<float>(65 * k);
                graphics->DrawImage(glow, Gdiplus::RectF(cx - g, cy - g, g * 2, g * 2), 0, 0, static_cast<Gdiplus::REAL>(glow->GetWidth()),
                                    static_cast<Gdiplus::REAL>(glow->GetHeight()), Gdiplus::UnitPixel);
            }
            if (sheet != nullptr) {
                graphics->DrawImage(sheet, Gdiplus::Rect(r.left, r.top, r.right - r.left, r.bottom - r.top),
                                    static_cast<INT>(perk.icon * data.perkWidth), 0, static_cast<INT>(data.perkWidth),
                                    static_cast<INT>(data.perkHeight), Gdiplus::UnitPixel);
            }
            if (dimmed) {  // not what the search text is looking for
                Gdiplus::SolidBrush shade(Gdiplus::Color(160, 0, 0, 0));
                graphics->FillEllipse(&shade, cx - outer, cy - outer, outer * 2, outer * 2);
            }
            if (flashed) {  // where a search jump arrived: the gold of the card frame (DrawJumpFlash), round the socket
                Gdiplus::Pen gold(Gdiplus::Color(255, 255, 226, 140), std::max(2.0f, static_cast<float>(3 * k)));
                const float ringR = outer + static_cast<float>(3 * k);
                graphics->DrawEllipse(&gold, cx - ringR, cy - ringR, ringR * 2, ringR * 2);
            }
        }
    }
    y = by + boardH + S(14);
    // The page's explanation, below the board. Its lines come with D2 colour codes for the specialization names.
    for (const std::string& text : data.intro) {
        const std::vector<RunePiece> pieces = LinePieces(text, kDim);
        const int h = LayoutPieces(dc, pieces, kDim, Rect(left, y, left + width_, y), fontSmall, DT_LEFT | DT_WORDBREAK, false);
        if (draw) LayoutPieces(dc, pieces, kDim, Rect(left, y, left + width_, y + h), fontSmall, DT_LEFT | DT_WORDBREAK, true);
        y += h + S(2);
    }
    y += S(10);

    return y + S(8) - origin;
}

// ---- mouse ----------------------------------------------------------------------------------------------------------

// The pointer moved over the detail pane. Returns true on the Specializations page (the page has no other hover).
bool OverlayState::SpecMove(int x, int y) {
    if (!SpecActive()) return false;
    int tab = -1, perk = -1;
    RECT rect{};
    if (y >= clipTop && y <= clipBottom) {
        for (const SpecHit& hit : specHits) {
            if (!Inside(hit.rect, x, y)) continue;
            if (hit.kind == 0) tab = hit.index;
            else { perk = hit.index; rect = hit.rect; }
            break;
        }
    }
    if (tab != specHoverTab || perk != specHoverPerk) {
        specHoverTab = tab;
        specHoverPerk = perk;
        specHoverRect = rect;
        InvalidateRect(window, nullptr, FALSE);
    }
    return true;
}

bool OverlayState::SpecClick(int x, int y) {
    if (!SpecActive() || !Inside(layout.detail, x, y) || y < clipTop) return false;
    for (const SpecHit& hit : specHits) {
        if (!Inside(hit.rect, x, y)) continue;
        if (hit.kind == 0) SpecSelectTab(hit.index);
        return true;  // a perk has no click action on the website either
    }
    return false;
}

// ---- tooltip --------------------------------------------------------------------------------------------------------

// The tooltip of the perk the pointer is on, or else the one the controller highlights. Drawn last, over everything.
void OverlayState::DrawSpecTooltip(HDC dc) {
    if (!SpecActive() || !specData || oskOpen) return;
    const spec::Data& data = *specData;
    int perkIndex = specHoverPerk;
    RECT target = specHoverRect;
    if (perkIndex < 0 && padActive && padZone == PadZone::Spec && specPadKind == 1) {
        perkIndex = specPadIndex;
        for (const SpecHit& hit : specHits)
            if (hit.kind == 1 && hit.index == perkIndex) target = hit.rect;
    } else if (perkIndex >= 0) {
        for (const SpecHit& hit : specHits)
            if (hit.kind == 1 && hit.index == perkIndex) target = hit.rect;
    } else if (SpecFlashing() && specFlashTab == SpecShownTab()) {
        perkIndex = specFlashPerk;  // the perk a search jump arrived at, until the pointer or the controller picks another
        target = RECT{};
        for (const SpecHit& hit : specHits)
            if (hit.kind == 1 && hit.index == perkIndex) target = hit.rect;
        if (target.right <= target.left) return;  // not drawn yet
    }
    const spec::Tab& tab = data.tabs[static_cast<std::size_t>(SpecShownTab())];
    if (perkIndex < 0 || static_cast<std::size_t>(perkIndex) >= tab.perks.size()) return;
    if (target.bottom < clipTop || target.top > clipBottom) return;  // scrolled out of the pane
    const spec::Perk& perk = tab.perks[static_cast<std::size_t>(perkIndex)];

    struct Row { std::wstring text; COLORREF color; HFONT font; int gapBefore; };
    std::vector<Row> rows;
    rows.push_back({Wide(perk.name), RGB(232, 196, 112), fontBold, 0});
    if (!perk.text.empty()) rows.push_back({Wide(perk.text), RGB(235, 235, 235), fontBody, S(6)});
    if (!perk.hint.empty()) rows.push_back({Wide(perk.hint), RGB(140, 140, 140), fontSmall, S(10)});

    RECT client{};
    GetClientRect(window, &client);
    const int tipWidth = std::min(S(360), static_cast<int>(client.right) - S(16));
    const int inner = tipWidth - S(24);
    std::vector<int> heights;
    int total = S(16);
    for (const Row& row : rows) {
        heights.push_back(MeasureHeight(dc, row.text, inner, row.font));
        total += heights.back() + row.gapBefore;
    }

    // Beside the perk and clear of the drawn cursor, as the item tooltips do.
    const int cursorRight = std::max(gCursorDrawWidth.load() - gCursorHotX.load(), 0) + S(6);
    const int cursorLeft = gCursorHotX.load() + S(6);
    int left = target.right + cursorRight;
    if (left + tipWidth > client.right - S(8)) {
        const int leftSide = target.left - cursorLeft - tipWidth;
        left = leftSide >= S(8) ? leftSide : std::max(S(8), static_cast<int>(client.right) - S(8) - tipWidth);
    }
    int topY = target.top;
    if (topY + total > client.bottom - S(8)) topY = client.bottom - total - S(8);
    int minTop = S(8);  // below the search box and its buttons, which are drawn over a tip
    for (const RECT* above : {&layout.search, &layout.clearQuery, &layout.matchPrev, &layout.matchNext})
        minTop = std::max(minTop, static_cast<int>(above->bottom) + S(6));
    topY = std::max(minTop, topY);

    const RECT tipBox = Rect(left, topY, left + tipWidth, topY + std::min(total, static_cast<int>(client.bottom) - topY - S(8)));
    CornerShadow(dc, tipBox, S(2));
    FillPanel(dc, tipBox, RGB(10, 9, 8), 236);
    FrameColor(dc, tipBox, RGB(118, 92, 56));
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, tipBox.left, tipBox.top, tipBox.right, tipBox.bottom);
    int cursorY = topY + S(8);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        cursorY += rows[i].gapBefore;
        Text(dc, rows[i].text, Rect(left + S(12), cursorY, left + S(12) + inner, cursorY + heights[i]), rows[i].color, rows[i].font,
             DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        cursorY += heights[i];
        if (i == 0) FillColor(dc, Rect(left + S(12), cursorY + S(2), left + S(12) + inner, cursorY + S(3)), RGB(78, 62, 40));
    }
    RestoreDC(dc, saved);
}

// ---- the controller -------------------------------------------------------------------------------------------------

void OverlayState::PadEnterSpec() {
    if (specHits.empty()) return;
    padZone = PadZone::Spec;
    specHoverPerk = -1;
    specPadKind = 1;
    specPadIndex = 0;
    bool hasPerk = false;
    for (const SpecHit& hit : specHits)
        if (hit.kind == 1) hasPerk = true;
    if (!hasPerk) { specPadKind = 0; specPadIndex = SpecShownTab(); }
    SpecPadReveal();
}

bool OverlayState::SpecPadRect(RECT& out) const {
    for (const SpecHit& hit : specHits)
        if (hit.kind == specPadKind && hit.index == specPadIndex) { out = hit.rect; return true; }
    return false;
}

// Scrolls the page so the highlighted tab or perk is in view (the rectangle is from the last paint).
void OverlayState::SpecPadReveal() {
    RECT r{};
    if (!SpecPadRect(r)) return;
    const int margin = S(8);
    int delta = 0;
    if (r.top < clipTop + margin) delta = r.top - (clipTop + margin);
    else if (r.bottom > layout.detail.bottom - margin) delta = r.bottom - (layout.detail.bottom - margin);
    if (delta == 0) return;
    PageView& view = CurrentView();
    view.detailScroll = std::clamp(view.detailScroll + delta, 0, std::max(0, scrollTotal - scrollVisible));
    InvalidateRect(window, nullptr, FALSE);
}

// D-pad: to the nearest tab or perk in that direction. Landing on a tab shows it. Left of the first column: the categories.
void OverlayState::SpecPadMove(int dx, int dy) {
    if (!SpecActive() || specHits.empty()) { padZone = PadZone::List; return; }
    specHoverPerk = -1;
    std::vector<esrwiki::pad::Box> boxes;
    int current = -1;
    for (std::size_t i = 0; i < specHits.size(); ++i) {
        const SpecHit& hit = specHits[i];
        boxes.push_back({hit.rect.left, hit.rect.top, hit.rect.right, hit.rect.bottom});
        if (hit.kind == specPadKind && hit.index == specPadIndex) current = static_cast<int>(i);
    }
    if (current < 0) { specPadKind = specHits.front().kind; specPadIndex = specHits.front().index; SpecPadReveal(); return; }
    const int next = esrwiki::pad::Neighbor(boxes, current, dx, dy);
    if (next < 0) {
        if (dx < 0) padZone = PadZone::List;
        return;
    }
    specPadKind = specHits[static_cast<std::size_t>(next)].kind;
    specPadIndex = specHits[static_cast<std::size_t>(next)].index;
    if (specPadKind == 0) SpecSelectTab(specPadIndex);
    SpecPadReveal();
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::SpecPadConfirm() {
    if (specPadKind == 0) SpecSelectTab(specPadIndex);  // a perk has no action to confirm
}
