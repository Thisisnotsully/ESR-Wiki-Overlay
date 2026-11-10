// ---------------------------------------------------------------------------------------------------------------
// The Skills page (page id "skills", skills_page.hpp): every class's skills as cards, made from the skills data
// (data\skills-data.json, or the copy under locales\<code> in the loader's language). It is included by overlay.cpp,
// after OverlayState is defined, and uses that file's drawing helpers.
//
// The page is not in wiki-data.txt: PrepareSkillsPage puts it into every database the overlay shows (right after the
// Kill Ledger), as ordinary entries and blocks, so search, the in-page filter, the suggestions, tooltips, highlighting
// and the controller treat it like any other tab. Without skills data there is no page.
//
// The picker strip above the cards (skill level, character level, hard points) works the numbers out again: only the
// blocks' text is written again (RefreshSkillsEntries), so the list, the selection, the scroll position and the card
// under the pointer all stay. The values last while the plugin runs.
// ---------------------------------------------------------------------------------------------------------------

// The phrases of the skills data that its generator leaves in English in every language (skills_page.hpp, PageText::data):
// class titles, the item skills' introduction, the on-striking and on-casting reasons, and the sources' kinds, names and
// values. They all come through here, so their translations only need adding to ui-strings.txt. As for other names from
// the data (UiDataText), a phrase with no translation is not logged as missing.
static std::string SkillsDataText(const std::string& english) { return Narrow(UiDataText(Wide(english))); }

// The overlay's own labels on the page, in UTF-8 (empty when there is no translation: PageText then keeps the English).
static std::string SkillsUiText(const std::string& english) { return UiTextUtf8(english); }

static skills::PageText SkillsPageText() {
    skills::PageText text;
    text.ui = &SkillsUiText;
    text.data = &SkillsDataText;
    return text;
}

bool OverlayState::SkillsActive() const {
    return database != nullptr && !database->pages.empty() && CurrentPage().id == skills::kSkillsPageId;
}

void Overlay::SetSkillsData(std::shared_ptr<const skills::Data> data) {
    if (!impl_ || !data) return;
    {
        std::lock_guard<std::mutex> guard(impl_->skillsLock);
        impl_->skillsPending = std::move(data);
    }
    // Before Start the window's first set-up adopts it (no page swap); afterwards the UI thread does (ApplySkillsData).
    if (impl_->window != nullptr) PostMessageW(impl_->window, kMsgSkillsData, 0, 0);
}

bool OverlayState::AdoptSkillsData() {
    std::shared_ptr<const skills::Data> next;
    {
        std::lock_guard<std::mutex> guard(skillsLock);
        next = std::move(skillsPending);
        skillsPending.reset();
    }
    if (!next) return false;
    skillsLayout = skills::PageLayout{};  // it points into the old data
    skillsEngine.reset();
    skillsData = std::move(next);
    skillsEngine = std::make_unique<skills::Engine>(*skillsData);
    skillsPicker = skillsPickerSet ? skills::ClampPicker(*skillsData, skillsPicker) : skills::DefaultPicker(*skillsData);
    skillsPickerSet = true;
    return true;
}

void OverlayState::ApplySkillsData() {
    if (!AdoptSkillsData() || database == nullptr) return;
    // The page is made again in a copy of the database, handed over like data from the website: the tabs, the views, the
    // search index and the suggestions are then all made again. A database already waiting gets the page instead.
    {
        std::lock_guard<std::mutex> guard(pendingLock);
        if (!pendingDb) pendingDb = std::make_unique<Database>(*database);
    }
    ApplyPendingDatabase();
}

// Puts the Skills page into the database shown, or takes it out when there is no skills data. The database Start was
// given belongs to the plugin, so it is copied first (once); the ones from ReplaceDatabase are the overlay's own.
void OverlayState::PrepareSkillsPage() {
    if (database == nullptr) return;
    const bool hasPage = skills::FindSkillsPage(*database) >= 0;
    if (!skillsData && !hasPage) return;  // nothing to do (the Horadric Cube window, or no skills data)
    if (database != ownedDb.get()) {
        ownedDb = std::make_unique<Database>(*database);
        database = ownedDb.get();
    }
    if (!skillsData || !skillsEngine) {
        skills::RemoveSkillsPage(*ownedDb);
        skillsLayout = skills::PageLayout{};
        return;
    }
    const skills::PageText text = SkillsPageText();
    std::vector<Entry> entries = skills::BuildSkillsEntries(*skillsData, *skillsEngine, skillsPicker, text, skillsLayout);
    if (entries.empty()) {  // empty or odd data: no tab rather than an empty one
        skills::RemoveSkillsPage(*ownedDb);
        skillsLayout = skills::PageLayout{};
        return;
    }
    skills::InsertSkillsPage(*ownedDb, std::move(entries), text);
}

// The picker changed: the cards' text is written again in place.
void OverlayState::SkillsPickerChanged() {
    if (!skillsData || !skillsEngine || database == nullptr || database != ownedDb.get()) return;
    const int at = skills::FindSkillsPage(*database);
    if (at < 0) return;
    Page& page = ownedDb->pages[static_cast<std::size_t>(at)];
    if (!skills::RefreshSkillsEntries(*skillsData, *skillsEngine, skillsPicker, SkillsPageText(), page.entries, skillsLayout)) {
        // Not expected (the page was made from this data): make the page again from scratch.
        DebugLogOnce("skills-refresh", "Skills page: the cards could not be updated in place, the page is made again");
        {
            std::lock_guard<std::mutex> guard(pendingLock);
            if (!pendingDb) pendingDb = std::make_unique<Database>(*database);
        }
        ApplyPendingDatabase();
        return;
    }
    arrangeValid = false;      // a search in the page may keep other cards now
    searchIndexFor = nullptr;  // the cards' text changed: the suggestions' index is made again when it is next needed
    if (!query.empty()) {
        // The cards' text changed, so what the search keeps changed too: the entry list, the counts on the tabs and the
        // selection are worked out again (as Refilter does, but the viewed entry stays selected when it still matches).
        const bool hadSelection = CurrentView().selected < filtered.size();
        const std::size_t keptEntry = hadSelection ? filtered[CurrentView().selected] : 0;
        filtered = FilterPage(pageIndex);
        pagesDirty = true;
        tabMatches.assign(database->pages.size(), 0);
        totalMatches = 0;
        for (std::size_t i = 0; i < database->pages.size(); ++i) {
            tabMatches[i] = i == pageIndex ? filtered.size() : FilterPage(i).size();
            totalMatches += tabMatches[i];
        }
        PageView& view = CurrentView();
        const auto kept = std::find(filtered.begin(), filtered.end(), keptEntry);
        view.selected = hadSelection && kept != filtered.end() ? static_cast<std::size_t>(kept - filtered.begin()) : 0;
        view.listPage = PageOf(view.selected);
    }
    InvalidateRect(window, nullptr, FALSE);
}

// True when the value changed (false at a limit, or without data).
bool OverlayState::SkillsStep(int field, int delta) {
    if (!skillsData || delta == 0) return false;
    const skills::Picker next = skills::StepPicker(*skillsData, skillsPicker, field, delta);
    if (next == skillsPicker) return false;
    skillsPicker = next;
    SkillsPickerChanged();
    return true;
}

// The pad's Left and Right on a picker value; holding the direction speeds it up as on the calculator's numbers.
// False at a limit, where the pad moves on as usual.
bool OverlayState::SkillsPadStep(int field, int dx) {
    const ULONGLONG now = GetTickCount64();
    skillsHold = now - skillsHoldAt > 400 ? 1 : skillsHold + 1;
    skillsHoldAt = now;
    return SkillsStep(field, dx * esrwiki::pad::NumberStep(skillsHold));
}

// The mouse wheel over a picker control changes its value (up: higher).
bool OverlayState::SkillsWheel(int x, int y, int steps) {
    if (!SkillsActive()) return false;
    for (const Chip& chip : layout.chips) {
        if (chip.kind != ChipKind::Picker || !Inside(chip.rect, x, y)) continue;
        SkillsStep(static_cast<int>(chip.value / 3), steps);
        return true;
    }
    return false;
}

std::wstring OverlayState::SkillsPickerValue(int field) const {
    if (field == 0) return std::to_wstring(skillsPicker.skillLevel);
    if (field == 1) return std::to_wstring(skillsPicker.characterLevel);
    return skillsPicker.hardPoints ? std::to_wstring(*skillsPicker.hardPoints) : UiText(L"Default");
}

// The picker strip and the sort row above the cards. Each picker is three chips: - (part 0), the value (part 1) and +
// (part 2), with Chip::value = field * 3 + part. The controller's Left and Right on the value change it.
int OverlayState::BuildSkillsControls(HDC dc, const PageView& view) {
    if (!skillsData) return layout.detail.top + S(16);
    const int left = layout.detail.left + S(16);
    const int top = layout.detail.top + S(16);
    const int rowHeight = std::max(S(26), smallLine + S(10));
    const wchar_t* const labels[] = {L"Skill level:", L"Character level:", L"Hard points:", L"Sort:"};
    int labelWidth = 0;
    for (const wchar_t* text : labels) labelWidth = std::max(labelWidth, MeasureWidth(dc, UiText(text), fontSmall));
    labelWidth += S(12);
    const int chipStart = left + labelWidth;
    // Every value box has the width of the widest value, so the - and + buttons do not move as the numbers change.
    const int widestNumber = MeasureWidth(dc, std::to_wstring(skillsData->levels.max), fontSmall);
    const int widestPoints = MeasureWidth(dc, std::to_wstring(skillsData->levels.synergy), fontSmall);
    const int valueWidth = std::max({S(64), MeasureWidth(dc, UiText(L"Default"), fontSmall) + S(20),
                                     std::max(widestNumber, widestPoints) + S(20)});

    const auto label = [&](int rowY, const wchar_t* text) {
        Text(dc, UiText(text), Rect(left, rowY, left + labelWidth, rowY + rowHeight), kDim, fontSmall,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    };
    const auto box = [&](const RECT& rect, const std::wstring& text, COLORREF fill, COLORREF frame, COLORREF color) {
        CornerShadow(dc, rect, S(2));
        FillPanel(dc, rect, fill, 200);
        FrameColor(dc, rect, frame);
        Text(dc, text, rect, color, fontSmall, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    };

    int y = top;
    const int maxLevel = skillsData->levels.max;
    for (int field = 0; field < 3; ++field) {
        label(y, labels[field]);
        // At a limit the button is dimmed (it does nothing there).
        const bool atLow = field == 0   ? skillsPicker.skillLevel <= 1
                         : field == 1 ? skillsPicker.characterLevel <= 1
                                      : !skillsPicker.hardPoints;
        const bool atHigh = field == 0   ? skillsPicker.skillLevel >= maxLevel
                          : field == 1 ? skillsPicker.characterLevel >= maxLevel
                                       : skillsPicker.hardPoints && *skillsPicker.hardPoints >= skillsData->levels.synergy;
        int x = chipStart;
        const RECT minus = Rect(x, y, x + rowHeight, y + rowHeight);
        x += rowHeight + S(4);
        const RECT value = Rect(x, y, x + valueWidth, y + rowHeight);
        x += valueWidth + S(4);
        const RECT plus = Rect(x, y, x + rowHeight, y + rowHeight);
        const std::size_t base = static_cast<std::size_t>(field) * 3;
        layout.chips.push_back(Chip{minus, ChipKind::Picker, base, ""});
        layout.chips.push_back(Chip{value, ChipKind::Picker, base + 1, ""});
        layout.chips.push_back(Chip{plus, ChipKind::Picker, base + 2, ""});
        box(minus, L"-", kPanel, kBorder, atLow ? kDim : kText);
        box(value, SkillsPickerValue(field), kSearchFill, kGold, kGold);
        box(plus, L"+", kPanel, kBorder, atHigh ? kDim : kText);
        y += rowHeight + S(6);
    }

    // Sort: the trees in the game's order (the default), or by name.
    label(y, labels[3]);
    int x = chipStart;
    const auto sortChip = [&](const std::wstring& text, BlockSort sort, bool selected) {
        const int chipWidth = std::max(S(56), MeasureWidth(dc, text, fontSmall) + S(16));
        const RECT rect = Rect(x, y, x + chipWidth, y + rowHeight);
        layout.chips.push_back(Chip{rect, ChipKind::Sort, static_cast<std::size_t>(sort), ""});
        box(rect, text, selected ? kSelected : kPanel, selected ? kGold : kBorder, selected ? kGold : kText);
        x += chipWidth + S(6);
    };
    const bool byName = view.sort == BlockSort::Name;
    sortChip(UiText(L"Skill tree"), BlockSort::Default, !byName);
    sortChip(UiText(L"A-Z"), BlockSort::Name, byName);
    return y + rowHeight + S(10);
}

// The faces of the cards of `entry` (an entry of the Skills page), or null.
const std::vector<skills::CardFace>* OverlayState::SkillFaces(const Entry& entry) const {
    if (!SkillsActive()) return nullptr;
    const std::vector<Entry>& entries = CurrentPage().entries;
    if (entries.empty() || &entry < entries.data() || &entry >= entries.data() + entries.size()) return nullptr;
    const std::size_t index = static_cast<std::size_t>(&entry - entries.data());
    if (index >= skillsLayout.faces.size() || skillsLayout.faces[index].size() != entry.blocks.size()) return nullptr;
    return &skillsLayout.faces[index];
}

// Skill cards: the name, its tags (Passive, Aura), the required level and the prerequisites, then the first lines at the
// chosen level. The whole card shows the tooltip (every field and line of the block).
int OverlayState::RenderSkillTiles(HDC dc, const Entry& entry, const std::vector<std::size_t>& order, std::size_t begin,
                                   std::size_t end, int left, int width_, int y, bool draw) {
    const int columns = width_ >= S(660) ? 3 : 2;
    const int gap = S(10);
    const int tileWidth = std::max(S(80), (width_ - gap * (columns - 1)) / columns);
    const int pad = S(8);
    const int inner = std::max(S(20), tileWidth - 2 * pad);
    const std::vector<skills::CardFace>* faces = SkillFaces(entry);

    struct Piece {
        std::wstring text;
        COLORREF color;
        HFONT font;
        int height;
        int gapBefore;
    };
    const auto piecesOf = [&](std::size_t index) {
        const Block& block = entry.blocks[index];
        std::vector<Piece> out;
        const auto add = [&](const std::string& text, COLORREF base, HFONT font, int gapBefore) {
            if (text.empty()) return;
            std::string plain;
            COLORREF color = base;
            PlainText(text, plain, color, base);
            const std::wstring wide = Wide(plain);
            out.push_back(Piece{wide, color, font, MeasureHeight(dc, wide, inner, font), out.empty() ? 0 : gapBefore});
        };
        add(block.title, itemNameColor, fontBold, 0);
        if (faces != nullptr && index < faces->size()) {
            const skills::CardFace& face = (*faces)[index];
            add(face.tags, kDim, fontSmall, S(1));
            add(face.requirement, kDim, fontSmall, S(1));
            add(face.prerequisites, kDim, fontSmall, S(1));
            bool first = true;
            for (const std::string& line : face.lines) {
                add(line, kBody, fontSmall, first ? S(6) : S(1));
                first = false;
            }
        }
        return out;
    };
    const auto heightOf = [&](const std::vector<Piece>& pieces) {
        int height = 2 * pad;
        for (const Piece& piece : pieces) height += piece.gapBefore + piece.height;
        return height;
    };

    const std::size_t step = static_cast<std::size_t>(columns);
    for (std::size_t first = begin; first < end; first += step) {
        if (draw && y > clipBottom) break;  // rows below the band are not drawn
        const std::size_t count = std::min<std::size_t>(step, end - first);
        std::vector<std::vector<Piece>> row;
        int rowHeight = 0;
        for (std::size_t c = 0; c < count; ++c) {
            row.push_back(piecesOf(order[first + c]));
            rowHeight = std::max(rowHeight, heightOf(row.back()));
        }
        for (std::size_t c = 0; c < count; ++c) {
            const std::size_t index = order[first + c];
            const Block& block = entry.blocks[index];
            const int tileLeft = left + static_cast<int>(c) * (tileWidth + gap);
            const RECT tile = Rect(tileLeft, y, tileLeft + tileWidth, y + rowHeight);
            if (!draw && !query.empty() && BlockMatches(block, query)) matchTops.push_back(y - matchBase);
            if (!draw) blockSpans.push_back({static_cast<int>(index), y - matchBase, y + rowHeight - matchBase});
            const bool onScreen = tile.bottom > clipTop && tile.top < clipBottom;
            if (!draw || !onScreen) continue;
            CornerShadow(dc, tile, S(2));
            FillPanel(dc, tile, kTile, 150);
            FrameColor(dc, tile, kBorder);
            int textTop = y + pad;
            for (const Piece& piece : row[c]) {
                textTop += piece.gapBefore;
                Text(dc, piece.text, Rect(tileLeft + pad, textTop, tileLeft + pad + inner, textTop + piece.height), piece.color,
                     piece.font, DT_LEFT | DT_WORDBREAK);
                textTop += piece.height;
            }
            // The hover and click area is only the part inside the band, so a tooltip never opens over the picker strip.
            RECT hitRect = tile;
            hitRect.top = std::max(hitRect.top, static_cast<LONG>(clipTop));
            hitRect.bottom = std::min(hitRect.bottom, static_cast<LONG>(clipBottom));
            if (hitRect.bottom <= hitRect.top) continue;
            detailHits.push_back(DetailHit{hitRect, &entry, &block, -1});
            cardHits.push_back(CardHit{hitRect, &entry, &block});
        }
        y += rowHeight + gap;
    }
    return y;
}
