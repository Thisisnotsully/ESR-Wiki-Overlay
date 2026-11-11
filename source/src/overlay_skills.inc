// ---------------------------------------------------------------------------------------------------------------
// The Skills page (page id "skills"): the class's skill trees and the details of one skill, laid out like the reference
// Skills page of the runeword browser. It is included by overlay.cpp, after OverlayState is defined, and uses that
// file's drawing helpers. The model (trees, header facts, the Values table, the item sources) is skills_tree.hpp.
//
// The page is not in wiki-data.txt: PrepareSkillsPage puts it into every database the overlay shows (right after the
// Kill Ledger) as one entry per class (and one for the item skills) with one block per skill (skills_page.hpp). Those
// blocks are not drawn: they are what the search index, the in-page filter and the suggestions look at, so a skill is
// found by its name, description, class, tree, Passive / Aura and the items that grant it. A hit on a skill selects it
// in its tree (the tree's tab, the tile framed for a moment, scrolled into view); while text is typed the tiles that do
// not match are dimmed.
//
// Layout: at 1080p the pane is split, the tree on the left (about 45%: the tree tabs, then a grid of three columns with
// the prerequisite lines behind the tiles) and the selected skill on the right, which scrolls on its own. A narrow pane
// stacks the details under the tree and the pane scrolls as a whole. The item skills have a list instead of a tree.
//
// What is chosen (the tree and the skill per class, the skill level, the character level and the synergy points) lasts
// while the plugin runs. The tree layout and the values are worked out again only when one of them changes.
// ---------------------------------------------------------------------------------------------------------------

// The phrases of the skills data that its generator leaves in English in every language (skills_page.hpp, PageText::data):
// class titles, the item skills' introduction, the notes, the on-striking and on-casting reasons, and the sources' kinds,
// names and values. They all come through here, so their translations only need adding to ui-strings.txt. As for other
// names from the data (UiDataText), a phrase with no translation is not logged as missing.
static std::string SkillsDataText(const std::string& english) { return Narrow(UiDataText(Wide(english))); }

// The overlay's own labels for the hidden blocks, in UTF-8 (empty when there is no translation: PageText keeps the English).
static std::string SkillsUiText(const std::string& english) { return UiTextUtf8(english); }

static skills::PageText SkillsPageText() {
    skills::PageText text;
    text.ui = &SkillsUiText;
    text.data = &SkillsDataText;
    return text;
}

namespace {

const wchar_t kSkDot[] = L" \u00B7 ";
const wchar_t kSkTimes[] = L" \u00D7";
const wchar_t kSkDash[] = L"\u2013";

std::wstring SkLevelText(int level) { return std::to_wstring(level) + (skills::BeyondReach(level) ? L"*" : L""); }

// Text from the data without D2 colour codes.
std::wstring SkPlain(const std::string& text) {
    std::string plain;
    COLORREF unused = kBody;
    PlainText(text, plain, unused, kBody);
    return Wide(plain);
}

}  // namespace

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
    // Everything below points into the old data.
    skillsLayout = skills::PageLayout{};
    skillsCache = SkillsCache{};
    skillsHits.clear();
    skillsLitMap.clear();
    skillsLitDb = nullptr;
    skillsFlash = nullptr;
    skillsEngine.reset();
    skillsData = std::move(next);
    skillsEngine = std::make_unique<skills::Engine>(*skillsData);
    const int top = std::max(1, skillsData->levels.max);
    skillsLevel = std::clamp(skillsLevel > 0 ? skillsLevel : skillsData->levels.skill, 1, top);
    skillsCharacterLevel = std::clamp(skillsCharacterLevel > 0 ? skillsCharacterLevel : skillsData->levels.character, 1, top);
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
// given belongs to the plugin, so it is copied first (once); the ones from ReplaceDatabase are the overlay's own. The
// blocks are made at the data's default levels: they are only searched, never drawn, so they do not follow the levels.
void OverlayState::PrepareSkillsPage() {
    if (database == nullptr) return;
    const bool hasPage = skills::FindSkillsPage(*database) >= 0;
    if (!skillsData && !hasPage) return;  // nothing to do (the Horadric Cube window, or no skills data)
    if (database != ownedDb.get()) {
        ownedDb = std::make_unique<Database>(*database);
        database = ownedDb.get();
    }
    skillsCache = SkillsCache{};  // the entries are made again
    skillsLitMap.clear();
    skillsLitDb = nullptr;
    if (!skillsData || !skillsEngine) {
        skills::RemoveSkillsPage(*ownedDb);
        skillsLayout = skills::PageLayout{};
        return;
    }
    const skills::PageText text = SkillsPageText();
    std::vector<Entry> entries =
        skills::BuildSkillsEntries(*skillsData, *skillsEngine, skills::DefaultPicker(*skillsData), text, skillsLayout);
    if (entries.empty()) {  // empty or odd data: no tab rather than an empty one
        skills::RemoveSkillsPage(*ownedDb);
        skillsLayout = skills::PageLayout{};
        return;
    }
    skills::InsertSkillsPage(*ownedDb, std::move(entries), text);
}

// ---- the model behind the page --------------------------------------------------------------------------------------

// The index of the entry in the detail pane, or -1.
int OverlayState::SkillsShownEntry() {
    if (!skillsData || !SkillsActive()) return -1;
    const PageView& view = CurrentView();
    if (filtered.empty() || view.selected >= filtered.size()) return -1;
    const std::size_t entryIndex = filtered[view.selected];
    if (entryIndex >= skillsLayout.skills.size()) return -1;
    return static_cast<int>(entryIndex);
}

// The class of that entry (the item skills are the class "other"), or null.
const skills::Class* OverlayState::SkillsShownClass() {
    const int entryIndex = SkillsShownEntry();
    if (entryIndex < 0) return nullptr;
    for (const skills::Skill* skill : skillsLayout.skills[static_cast<std::size_t>(entryIndex)])
        if (skill != nullptr) return skillsData->FindClass(skill->cls);
    return nullptr;
}

// The skill a tile or list row shows (value of a kSkTile hit), or null.
const skills::Skill* OverlayState::SkillsTileSkill(int value) const {
    if (value < 0) return nullptr;
    const std::size_t at = static_cast<std::size_t>(value);
    if (skillsCache.items) return at < skillsCache.list.size() ? skillsCache.list[at] : nullptr;
    return at < skillsCache.grid.cells.size() ? skillsCache.grid.cells[at].skill : nullptr;
}

int OverlayState::SkillsTileOf(const skills::Skill* skill) const {
    if (skill == nullptr) return -1;
    if (skillsCache.items) {
        const auto found = std::find(skillsCache.list.begin(), skillsCache.list.end(), skill);
        return found == skillsCache.list.end() ? -1 : static_cast<int>(found - skillsCache.list.begin());
    }
    return skillsCache.grid.IndexOf(skill);
}

// The hidden block of `skill` in the entry shown, or null.
const Block* OverlayState::SkillsBlockOf(const skills::Skill* skill) {
    const int entryIndex = SkillsShownEntry();
    if (entryIndex < 0 || skill == nullptr) return nullptr;
    const std::vector<const skills::Skill*>& shape = skillsLayout.skills[static_cast<std::size_t>(entryIndex)];
    const std::vector<Entry>& entries = CurrentPage().entries;
    if (static_cast<std::size_t>(entryIndex) >= entries.size()) return nullptr;
    const Entry& entry = entries[static_cast<std::size_t>(entryIndex)];
    for (std::size_t i = 0; i < shape.size() && i < entry.blocks.size(); ++i)
        if (shape[i] == skill) return &entry.blocks[i];
    return nullptr;
}

// While text is typed: false for a skill whose block does not match it (its tile is dimmed). True without text. The
// answers are kept until the text or the database changes (a paint asks for every tile).
bool OverlayState::SkillsLit(const skills::Skill* skill) {
    if (query.empty()) return true;
    if (skillsLitQuery != query || skillsLitDb != database) {
        skillsLitMap.clear();
        skillsLitQuery = query;
        skillsLitDb = database;
    }
    const auto found = skillsLitMap.find(skill);
    if (found != skillsLitMap.end()) return found->second;
    const Block* block = SkillsBlockOf(skill);
    const bool lit = block != nullptr && BlockMatches(*block, query);
    skillsLitMap.emplace(skill, lit);
    return lit;
}

// The tree (or the list) of `cls`, the selected skill, and its facts, values and sources, made again only when the
// class, the tree, the skill, the levels or the synergy points changed.
void OverlayState::SkillsEnsure(const skills::Class& cls) {
    const skills::Data& data = *skillsData;
    SkillsChoice& choice = skillsChoice[cls.id];
    const bool items = cls.trees.empty();
    int tree = 0;
    if (!items) {
        tree = cls.trees.front().n;
        for (const skills::Tree& t : cls.trees)
            if (t.n == choice.tree) tree = t.n;
        choice.tree = tree;
    }
    if (skillsCache.data != &data || skillsCache.cls != cls.id || skillsCache.tree != tree) {
        SkillsCache fresh;
        fresh.data = &data;
        fresh.cls = cls.id;
        fresh.tree = tree;
        fresh.items = items;
        if (items) fresh.list = cls.id == "other" ? skills::ItemSkillList(data) : data.SkillsOfClass(cls.id);
        else fresh.grid = skills::BuildTree(data, cls.id, tree);
        const bool otherClass = skillsCache.cls != cls.id;
        skillsCache = std::move(fresh);
        if (otherClass) {
            skillsDetailScroll = 0;
            skillsListScroll = 0;
        }
    }
    // The selected skill: the one chosen for this class when it is still there, else the first of the tree (or list).
    const skills::Skill* selected = choice.skill.empty() ? nullptr : data.FindByKey(choice.skill);
    if (SkillsTileOf(selected) < 0) {
        selected = nullptr;
        if (items) {
            if (!skillsCache.list.empty()) selected = skillsCache.list.front();
        } else {
            const skills::TreeCell* first = nullptr;
            for (const skills::TreeCell& cell : skillsCache.grid.cells)
                if (first == nullptr || cell.row < first->row || (cell.row == first->row && cell.col < first->col)) first = &cell;
            if (first != nullptr) selected = first->skill;
        }
        if (selected != nullptr) choice.skill = selected->key;
    }
    if (selected == nullptr) {
        skillsCache.skill = nullptr;
        skillsCache.built = false;
        return;
    }
    if (skillsCache.built && skillsCache.skill == selected && skillsCache.level == skillsLevel &&
        skillsCache.characterLevel == skillsCharacterLevel && skillsCache.synergy == skillsSynergy) {
        if (skillsAllLevels && !skillsCache.allBuilt) {
            skillsCache.all = skills::AllLevels(data, *skillsEngine, SkillsContext(*selected), skillsCache.table, 1, data.levels.max);
            skillsCache.allBuilt = true;
        }
        return;
    }
    skillsCache.skill = selected;
    skillsCache.level = skillsLevel;
    skillsCache.characterLevel = skillsCharacterLevel;
    skillsCache.synergy = skillsSynergy;
    const skills::Context ctx = SkillsContext(*selected);
    skillsCache.facts = skills::BuildHeaderFacts(data, *skillsEngine, ctx);
    skillsCache.table = skills::BuildValueTable(data, *skillsEngine, ctx);
    skillsCache.sources = skills::GroupSources(*selected);
    skillsCache.allBuilt = false;
    skillsCache.all.clear();
    if (skillsAllLevels) {
        skillsCache.all = skills::AllLevels(data, *skillsEngine, ctx, skillsCache.table, 1, data.levels.max);
        skillsCache.allBuilt = true;
    }
    skillsCache.built = true;
}

skills::Context OverlayState::SkillsContext(const skills::Skill& skill) const {
    skills::Context ctx;
    ctx.skill = &skill;
    ctx.skillLevel = skillsLevel;
    ctx.characterLevel = skillsCharacterLevel;
    ctx.synergyPoints = skillsSynergy;
    return ctx;
}

// Shows `skill` (in its tree, on its class's entry when that is the one shown). A search jump also frames its tile.
void OverlayState::SkillsSelect(const skills::Skill& skill, bool flash) {
    SkillsChoice& choice = skillsChoice[skill.cls];
    if (skill.tree > 0) choice.tree = skill.tree;
    choice.skill = skill.key;
    skillsDetailScroll = 0;
    skillsReveal = true;
    if (flash) {
        skillsFlash = &skill;
        skillsFlashUntil = GetTickCount64() + 1800;
        SetTimer(window, kTimerFlash, 1900, nullptr);
    }
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::SkillsSelectTree(int tree) {
    const skills::Class* cls = SkillsShownClass();
    if (cls == nullptr) return;
    SkillsChoice& choice = skillsChoice[cls->id];
    if (choice.tree == tree) return;
    choice.tree = tree;
    choice.skill.clear();  // the new tree's first skill
    skillsHoverKind = -1;
    skillsDetailScroll = 0;
    InvalidateRect(window, nullptr, FALSE);
}

// A search jump to a skill's block (JumpTo): handled before the page is laid out, so the tree and the tile are there.
void OverlayState::SkillsTakeJump() {
    if (!jumpPending || !skillsData) return;
    const PageView& view = CurrentView();
    if (view.selected >= filtered.size() || filtered[view.selected] != jumpEntry) return;  // ApplyJump drops it
    jumpPending = false;
    if (jumpBlock < 0 || jumpEntry >= skillsLayout.skills.size()) return;
    const std::vector<const skills::Skill*>& shape = skillsLayout.skills[jumpEntry];
    const std::size_t at = static_cast<std::size_t>(jumpBlock);
    if (at < shape.size() && shape[at] != nullptr) SkillsSelect(*shape[at], true);
}

// Enter / the match buttons on this page: the next (or previous) matching skill of the class, in the order of the trees
// and their rows. False when there is none that way, so the search moves on to the next entry.
bool OverlayState::SkillsStepMatch(int direction) {
    const skills::Class* cls = SkillsShownClass();
    if (cls == nullptr || query.empty()) return false;
    std::vector<const skills::Skill*> order;
    if (cls->trees.empty()) {
        order = cls->id == "other" ? skills::ItemSkillList(*skillsData) : skillsData->SkillsOfClass(cls->id);
    } else {
        for (const skills::Tree& tree : cls->trees) {
            const skills::TreeLayout grid = skills::BuildTree(*skillsData, cls->id, tree.n);
            std::vector<const skills::TreeCell*> cells;
            for (const skills::TreeCell& cell : grid.cells) cells.push_back(&cell);
            std::sort(cells.begin(), cells.end(), [](const skills::TreeCell* a, const skills::TreeCell* b) {
                return a->row != b->row ? a->row < b->row : a->col < b->col;
            });
            for (const skills::TreeCell* cell : cells) order.push_back(cell->skill);
        }
    }
    const auto here = std::find(order.begin(), order.end(), skillsCache.skill);
    const long long count = static_cast<long long>(order.size());
    long long at = here == order.end() ? (direction > 0 ? -1 : count) : static_cast<long long>(here - order.begin());
    for (at += direction; at >= 0 && at < count; at += direction) {
        const skills::Skill* skill = order[static_cast<std::size_t>(at)];
        if (skill != nullptr && SkillsLit(skill)) {
            SkillsSelect(*skill, true);
            return true;
        }
    }
    return false;
}

// One value moved by `delta` (field: 0 skill level, 1 character level, 2 + i synergy i). True when it changed.
bool OverlayState::SkillsStepField(int field, int delta) {
    if (!skillsData || delta == 0) return false;
    const int top = std::max(1, skillsData->levels.max);
    if (field == 0 || field == 1) {
        int& value = field == 0 ? skillsLevel : skillsCharacterLevel;
        const int next = std::clamp(value + delta, 1, top);
        if (next == value) return false;
        value = next;
    } else {
        const std::size_t index = static_cast<std::size_t>(field - 2);
        if (index >= skillsCache.table.synergies.size()) return false;
        const skills::SynergyEntry& entry = skillsCache.table.synergies[index];
        if (entry.skill == nullptr || !entry.adjustable) return false;
        const auto found = skillsSynergy.find(entry.skill->id);
        const int now = found == skillsSynergy.end() ? 0 : found->second;
        const int next = std::clamp(now + delta, 0, std::max(0, entry.maxPoints));
        if (next == now) return false;
        if (next == 0) skillsSynergy.erase(entry.skill->id);
        else skillsSynergy[entry.skill->id] = next;
    }
    InvalidateRect(window, nullptr, FALSE);
    return true;
}

// A click (or A on the controller) on a hit.
void OverlayState::SkillsActivate(int kind, int value) {
    switch (kind) {
    case kSkTab: SkillsSelectTree(value); break;
    case kSkTile:
        if (const skills::Skill* skill = SkillsTileSkill(value)) {
            SkillsSelect(*skill, false);
            skillsReveal = false;  // it is under the pointer already
        }
        break;
    case kSkStep:
        if (value % 3 != 1) SkillsStepField(value / 3, value % 3 == 0 ? -1 : 1);
        break;
    case kSkQuick:
        skillsLevel = std::clamp(value, 1, std::max(1, skillsData ? skillsData->levels.max : 1));
        break;
    case kSkMaxAll:
    case kSkReset:
        if (skillsCache.skill != nullptr && skillsData) {
            for (const skills::SynergyInput& input : skills::SynergySkills(*skillsData, *skillsCache.skill)) {
                if (input.skill == nullptr) continue;
                if (kind == kSkMaxAll && input.maxPoints > 0) skillsSynergy[input.skill->id] = input.maxPoints;
                else skillsSynergy.erase(input.skill->id);
            }
        }
        break;
    case kSkSubTab: skillsSubTab = std::clamp(value, 0, 2); break;
    case kSkAllLevels: skillsAllLevels = !skillsAllLevels; break;
    default: break;
    }
    InvalidateRect(window, nullptr, FALSE);
}

// ---- drawing --------------------------------------------------------------------------------------------------------

Gdiplus::Bitmap* OverlayState::SkillsIcon(const skills::Skill& skill) {
    if (!skillsIconsLoaded) {
        skillsIconsLoaded = true;
        std::ifstream file(dataDir + L"\\skill-icons.txt", std::ios::binary);
        if (file) {
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            skillsIcons.Parse(text);
        }
        if (skillsIcons.Size() == 0) DebugLogOnce("skill-icons", "Skills tab: skill-icons.txt is missing or empty, the tiles have no icons");
    }
    const std::string* stem = skillsIcons.Find(skill.key);
    return stem == nullptr ? nullptr : FindImage(*stem);
}

void OverlayState::SkillsDrawIcon(HDC dc, Gdiplus::Graphics* graphics, const skills::Skill& skill, const RECT& r) {
    FillColor(dc, r, RGB(8, 6, 4));
    Gdiplus::Bitmap* icon = SkillsIcon(skill);
    if (icon != nullptr && graphics != nullptr) {
        graphics->DrawImage(icon, Gdiplus::Rect(r.left, r.top, r.right - r.left, r.bottom - r.top), 0, 0,
                            static_cast<INT>(icon->GetWidth()), static_cast<INT>(icon->GetHeight()), Gdiplus::UnitPixel);
    }
    FrameColor(dc, r, kBorder);
}

// A hit, cut to `view` and to the detail band. `full` keeps the whole box, for scrolling it into view.
void OverlayState::SkillsAddHit(const RECT& r, int kind, int value, int zone, const RECT& view) {
    RECT cut = r;
    cut.left = std::max(cut.left, view.left);
    cut.right = std::min(cut.right, view.right);
    cut.top = std::max({cut.top, view.top, static_cast<LONG>(clipTop)});
    cut.bottom = std::min({cut.bottom, view.bottom, static_cast<LONG>(clipBottom)});
    if (cut.right <= cut.left || cut.bottom <= cut.top) return;
    skillsHits.push_back(SkillsHit{cut, r, kind, value, zone});
}

bool OverlayState::SkillsHot(int kind, int value) const { return skillsHoverKind == kind && skillsHoverValue == value; }

// A small box: a - / + button, a value, a chip or a tab.
void OverlayState::SkillsBox(HDC dc, const RECT& r, const std::wstring& text, bool selected, bool hot, bool dimText, HFONT font) {
    CornerShadow(dc, r, S(2));
    FillPanel(dc, r, selected ? kSelected : hot ? RGB(46, 34, 20) : kPanel, 220);
    FrameColor(dc, r, selected ? kGold : hot ? RGB(176, 136, 74) : kBorder);
    Text(dc, text, Rect(r.left + S(3), r.top, r.right - S(3), r.bottom), dimText ? kDim : (selected ? kGold : kText), font,
         DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

// Draws `paint` into an off-screen copy and copies back only `clip` (cut to the detail band): a DC clip does not hold once
// GDI+ has drawn into the DC (see Paint), and the scrolled panels must not spill over the rest of the pane.
void OverlayState::SkillsClipped(HDC dc, RECT clip, const std::function<void(HDC, Gdiplus::Graphics&)>& paint) {
    clip.top = std::max(clip.top, static_cast<LONG>(clipTop));
    clip.bottom = std::min(clip.bottom, static_cast<LONG>(clipBottom));
    clip.left = std::max(clip.left, static_cast<LONG>(layout.detail.left + 1));
    clip.right = std::min(clip.right, static_cast<LONG>(layout.detail.right - S(18)));
    if (clip.right <= clip.left || clip.bottom <= clip.top) return;
    RECT client{};
    GetClientRect(window, &client);
    const int clientW = static_cast<int>(client.right - client.left), clientH = static_cast<int>(client.bottom - client.top);
    if (clientW <= 0 || clientH <= 0) return;
    if (skillsClipDc == nullptr || skillsClipWidth != clientW || skillsClipHeight != clientH) {
        ReleaseSkillsClip();
        skillsClipDc = CreateCompatibleDC(dc);
        if (skillsClipDc != nullptr) skillsClipBitmap = CreateCompatibleBitmap(dc, clientW, clientH);
        if (skillsClipDc == nullptr || skillsClipBitmap == nullptr) {
            ReleaseSkillsClip();
            return;
        }
        skillsClipPrevious = SelectObject(skillsClipDc, skillsClipBitmap);
        skillsClipWidth = clientW;
        skillsClipHeight = clientH;
    }
    const HDC copy = skillsClipDc;
    const int w = static_cast<int>(clip.right - clip.left), h = static_cast<int>(clip.bottom - clip.top);
    BitBlt(copy, clip.left, clip.top, w, h, dc, clip.left, clip.top, SRCCOPY);
    {
        Gdiplus::Graphics graphics(copy);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetClip(Gdiplus::Rect(clip.left, clip.top, w, h));
        paint(copy, graphics);
    }
    BitBlt(dc, clip.left, clip.top, w, h, copy, clip.left, clip.top, SRCCOPY);
}

void OverlayState::ReleaseSkillsClip() {
    if (skillsClipDc != nullptr) {
        if (skillsClipPrevious != nullptr) SelectObject(skillsClipDc, skillsClipPrevious);
        DeleteDC(skillsClipDc);
    }
    if (skillsClipBitmap != nullptr) DeleteObject(skillsClipBitmap);
    skillsClipDc = nullptr;
    skillsClipBitmap = nullptr;
    skillsClipPrevious = nullptr;
    skillsClipWidth = 0;
    skillsClipHeight = 0;
}

// A thin scroll bar at the right edge of a panel that scrolls on its own (the wheel and the controller move it).
void OverlayState::SkillsScrollbar(HDC dc, const RECT& view, int total, int scroll, int bar) {
    const int visible = view.bottom - view.top;
    skillsBarTrack[bar] = RECT{};
    if (total <= visible || visible <= 0) return;
    const RECT track = Rect(view.right - S(5), view.top, view.right - S(1), view.bottom);
    const int thumbH = std::max(S(24), visible * visible / total);
    const int thumbTop = track.top + static_cast<int>(static_cast<long long>(visible - thumbH) * scroll / std::max(1, total - visible));
    skillsBarTrack[bar] = Rect(view.right - S(16), view.top, view.right, view.bottom);  // wider than it shows: easy to grab
    skillsBarThumb[bar] = Rect(track.left, thumbTop, track.right, thumbTop + thumbH);
    skillsBarTotal[bar] = total;
    FillPanel(dc, track, kSearchFill, 200);
    FillColor(dc, skillsBarThumb[bar], skillsBarDragging == bar ? kGold : kBorder);
}

// The mouse on a scroll bar: the thumb is dragged, the track above or below it pages.
bool OverlayState::SkillsBarDown(int x, int y) {
    if (!SkillsActive() || !skillsWide) return false;
    for (int bar = 0; bar < 2; ++bar) {
        const RECT& track = skillsBarTrack[bar];
        if (track.right <= track.left || !Inside(track, x, y)) continue;
        const int visible = track.bottom - track.top;
        int& scroll = bar == 0 ? skillsListScroll : skillsDetailScroll;
        const int maxScroll = bar == 0 ? skillsListMax : skillsDetailMax;
        if (y >= skillsBarThumb[bar].top && y < skillsBarThumb[bar].bottom) {
            skillsBarDragging = bar;
            skillsBarStartY = y;
            skillsBarStartScroll = scroll;
            SetCapture(window);
        } else {
            scroll = std::clamp(scroll + (y < skillsBarThumb[bar].top ? -visible : visible), 0, maxScroll);
        }
        InvalidateRect(window, nullptr, FALSE);
        return true;
    }
    return false;
}

void OverlayState::SkillsBarDrag(int y) {
    const int bar = skillsBarDragging;
    if (bar < 0 || bar > 1) return;
    const RECT& track = skillsBarTrack[bar];
    const int visible = track.bottom - track.top;
    const int thumbH = skillsBarThumb[bar].bottom - skillsBarThumb[bar].top;
    const int span = visible - thumbH;
    const int maxScroll = bar == 0 ? skillsListMax : skillsDetailMax;
    if (span <= 0 || maxScroll <= 0) return;
    const long long target = skillsBarStartScroll + static_cast<long long>(y - skillsBarStartY) * maxScroll / span;
    (bar == 0 ? skillsListScroll : skillsDetailScroll) = static_cast<int>(std::clamp<long long>(target, 0, maxScroll));
    InvalidateRect(window, nullptr, FALSE);
}

// The tile height for a tree of `rows` rows in `availH` pixels: the full tile (icon, two name lines, the level line), else a
// compact one (the level moves onto the icon), else it does not fit (`fits` false: the page stacks instead).
void OverlayState::SkillsTileSize(int availH, int rows, int& tileH, bool& compact, bool& fits) const {
    const int gap = S(10);
    const int full = std::max(S(44), 3 * smallLine + S(10));
    const int least = std::max(S(40), 2 * smallLine + S(8));
    const auto needs = [&](int h) { return rows * h + (rows - 1) * gap; };
    compact = false;
    fits = true;
    tileH = full;
    if (needs(full) <= availH) return;
    compact = true;
    tileH = std::min(full, std::max(least, (availH - (rows - 1) * gap) / std::max(1, rows)));
    if (needs(least) > availH) fits = false;
}

// The page: the class title, the tree (or the item list) and the selected skill. Returns the height the pane scrolls by:
// split, the pane does not scroll (the details panel does); stacked, everything does.
int OverlayState::RenderSkills(HDC dc, const Entry& entry, int top, int scroll, bool draw) {
    const RECT area = layout.detail;
    const int left = area.left + S(16);
    const int right = area.right - S(22);
    const int paneW = right - left;
    if (draw) {
        skillsHits.clear();
        skillsBarTrack[0] = skillsBarTrack[1] = RECT{};
    } else SkillsTakeJump();
    const skills::Class* cls = SkillsShownClass();
    bool wide = paneW >= S(560);  // the tree and the details need about this much side by side
    {
        static int lastPane = -1;
        if (lastPane != paneW) {
            lastPane = paneW;
            DebugLog("skills pane: " + std::to_string(paneW) + " wide, needs " + std::to_string(S(560)) + " for side by side, dpi " +
                     std::to_string(dpi));
        }
    }
    int y = top - (wide ? 0 : scroll);
    int origin = y;
    const int titleH = titleLine + S(8);
    if (draw) Text(dc, UiDataText(Wide(entry.title)), Rect(left, y, right, y + titleH), kHeaderGold, fontTitle, DT_LEFT | DT_SINGLELINE);
    y += titleH + S(6);
    if (cls == nullptr || !skillsEngine) {
        if (draw) Text(dc, UiText(L"Nothing selected"), Rect(left, y, right, y + bodyLine + S(6)), kDim, fontBody, DT_LEFT | DT_SINGLELINE);
        return y + bodyLine + S(6) - origin;
    }
    SkillsEnsure(*cls);
    if (wide && !skillsCache.items) {
        // Split only when the tree also fits the height: tabs, then the rows at their least spacing.
        int tileH = 0;
        bool compactTiles = false, tilesFit = true;
        const int gridTop = y + std::max(bodyLine + S(14), 2 * smallLine + S(10)) + S(14);
        SkillsTileSize(static_cast<int>(area.bottom) - S(10) - gridTop, std::max(1, skillsCache.grid.rows), tileH, compactTiles, tilesFit);
        {
            static std::string lastLayout;
            const std::string now = "skills layout: pane " + std::to_string(paneW) + " wide, tree area " +
                                    std::to_string(static_cast<int>(area.bottom) - S(10) - gridTop) + " high, " +
                                    std::to_string(skillsCache.grid.rows) + " rows, tile " + std::to_string(tileH) +
                                    (compactTiles ? " compact" : "") + (tilesFit ? ", side by side" : ", stacked (too tall)") +
                                    ", dpi " + std::to_string(dpi);
            if (now != lastLayout) {
                lastLayout = now;
                DebugLog(now);
            }
        }
        if (!tilesFit) {
            wide = false;
            y -= scroll;
            origin -= scroll;
        }
    }
    skillsWide = wide;
    const RECT band = Rect(area.left + 1, clipTop, area.right - S(18), clipBottom);

    const int gap = S(16);
    const int treeW = wide ? std::max(S(300), paneW * 50 / 100) : paneW;
    const int bodyTopY = y;
    const int treeBottom = RenderSkillsTree(dc, *cls, left, treeW, y, wide, draw);
    if (!wide) {
        int detailTop = treeBottom + S(14);
        if (draw) FillColor(dc, Rect(left, detailTop, right, detailTop + 1), kBorder);
        detailTop += S(12);
        std::unique_ptr<Gdiplus::Graphics> graphics;  // for the icon
        if (draw) {
            graphics = std::make_unique<Gdiplus::Graphics>(dc);
            graphics->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics->SetClip(Gdiplus::Rect(band.left, band.top, band.right - band.left, band.bottom - band.top));
        }
        const int end = SkillsDetails(dc, graphics.get(), *cls, left, paneW, detailTop, draw, band);
        if (skillsReveal) {  // a jump or a step to a match: SkillsApplyReveal scrolls the pane to the tile after this layout
            const int tile = SkillsTileOf(skillsCache.skill);
            if (tile >= 0) {
                skillsRevealTop = skillsTileTop - origin;
                skillsRevealBottom = skillsTileBottom - origin;
            }
        }
        return end + S(10) - origin;
    }
    skillsReveal = false;  // split: the tree does not scroll (the item list scrolls itself, in RenderSkillsTree)

    // The details panel, right of the tree: it scrolls on its own when it is taller than the pane.
    const int detailX = left + treeW + gap;
    const RECT view = Rect(detailX, bodyTopY, right + S(4), area.bottom - S(8));
    skillsDetailView = view;
    const int contentW = view.right - view.left - S(14);
    const int contentH = SkillsDetails(dc, nullptr, *cls, detailX, contentW, 0, false, view);
    const int visible = view.bottom - view.top;
    skillsDetailMax = std::max(0, contentH - visible);
    skillsDetailScroll = std::clamp(skillsDetailScroll, 0, skillsDetailMax);
    if (draw) {
        SkillsClipped(dc, view, [&](HDC target, Gdiplus::Graphics& graphics) {
            SkillsDetails(target, &graphics, *cls, detailX, contentW, view.top - skillsDetailScroll, true, view);
        });
        SkillsScrollbar(dc, view, contentH, skillsDetailScroll, 1);
    }
    return std::max(0, static_cast<int>(area.bottom) - S(10) - top);  // as tall as the pane: it does not scroll
}

// After a stacked layout: the tile a jump or a match step selected is scrolled into view.
void OverlayState::SkillsApplyReveal(int total, int visible) {
    if (!skillsReveal || !SkillsActive()) return;
    skillsReveal = false;
    if (skillsWide || skillsRevealBottom <= skillsRevealTop) return;
    PageView& view = CurrentView();
    int target = view.detailScroll;
    if (skillsRevealTop < target + S(8)) target = skillsRevealTop - S(8);
    else if (skillsRevealBottom > target + visible - S(8)) target = skillsRevealBottom - visible + S(8);
    view.detailScroll = std::clamp(target, 0, std::max(0, total - visible));
}

// The tree tabs and the grid of tiles (or the item skills' list). Returns the bottom.
int OverlayState::RenderSkillsTree(HDC dc, const skills::Class& cls, int x, int w, int y, bool wide, bool draw) {
    const RECT area = layout.detail;
    const RECT band = Rect(area.left + 1, clipTop, area.right - S(18), clipBottom);
    const ULONGLONG now = GetTickCount64();
    const bool flashing = skillsFlash != nullptr && now <= skillsFlashUntil;
    skillsTileTop = skillsTileBottom = 0;

    if (skillsCache.items) {
        // The item skills: a list of icon and name; split, it scrolls on its own.
        const std::vector<const skills::Skill*>& list = skillsCache.list;
        const int rowH = std::max(S(34), boldLine + S(14));
        const int iconSize = rowH - S(8);
        const int total = static_cast<int>(list.size()) * rowH;
        const RECT view = Rect(x, y, x + w, wide ? static_cast<int>(area.bottom) - S(8) : y + total);
        const int visible = view.bottom - view.top;
        int scroll = 0;
        if (wide) {
            skillsListView = view;
            skillsListMax = std::max(0, total - visible);
            const int selectedRow = SkillsTileOf(skillsCache.skill);
            if (skillsReveal && selectedRow >= 0) {
                const int rowTop = selectedRow * rowH;
                if (rowTop < skillsListScroll) skillsListScroll = rowTop;
                else if (rowTop + rowH > skillsListScroll + visible) skillsListScroll = rowTop + rowH - visible;
            }
            skillsListScroll = std::clamp(skillsListScroll, 0, skillsListMax);
            scroll = skillsListScroll;
        } else {
            skillsListView = RECT{};
        }
        const auto paintRows = [&](HDC target, Gdiplus::Graphics* graphics) {
            for (std::size_t i = 0; i < list.size(); ++i) {
                const int rowTop = y - scroll + static_cast<int>(i) * rowH;
                if (rowTop + rowH < view.top || rowTop > view.bottom) continue;
                const skills::Skill& skill = *list[i];
                const RECT r = Rect(x, rowTop + S(1), x + w - (wide ? S(10) : 0), rowTop + rowH - S(1));
                const bool selected = &skill == skillsCache.skill;
                const bool hot = SkillsHot(kSkTile, static_cast<int>(i));
                FillPanel(target, r, selected ? kSelected : hot ? RGB(46, 34, 20) : kTile, selected ? 220 : 150);
                if (selected || hot) FrameColor(target, r, selected ? kGold : RGB(176, 136, 74));
                const RECT iconRect = Rect(r.left + S(4), r.top + (r.bottom - r.top - iconSize) / 2, r.left + S(4) + iconSize,
                                           r.top + (r.bottom - r.top - iconSize) / 2 + iconSize);
                SkillsDrawIcon(target, graphics, skill, iconRect);
                const bool lit = SkillsLit(&skill);
                Text(target, SkPlain(skill.name), Rect(iconRect.right + S(10), r.top, r.right - S(6), r.bottom),
                     lit ? (selected ? kGold : kText) : kDim, fontBody, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                if (!lit && !selected) FillPanel(target, r, RGB(0, 0, 0), 110);
                if (flashing && skillsFlash == &skill) {
                    for (int grow = 0; grow < std::max(2, S(2)); ++grow)
                        FrameColor(target, Rect(r.left - grow, r.top - grow, r.right + grow, r.bottom + grow), RGB(255, 226, 140));
                }
            }
        };
        for (std::size_t i = 0; i < list.size(); ++i) {
            const int rowTop = y - scroll + static_cast<int>(i) * rowH;
            if (list[i] == skillsCache.skill) {
                skillsTileTop = rowTop;
                skillsTileBottom = rowTop + rowH;
            }
            if (draw) SkillsAddHit(Rect(x, rowTop, x + w - (wide ? S(10) : 0), rowTop + rowH), kSkTile, static_cast<int>(i), 0,
                                   wide ? view : band);
        }
        if (draw) {
            if (wide) {
                SkillsClipped(dc, view, [&](HDC target, Gdiplus::Graphics& graphics) { paintRows(target, &graphics); });
                SkillsScrollbar(dc, view, total, scroll, 0);
            } else {
                Gdiplus::Graphics graphics(dc);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                graphics.SetClip(Gdiplus::Rect(band.left, band.top, band.right - band.left, band.bottom - band.top));
                paintRows(dc, &graphics);
            }
        }
        if (wide) skillsReveal = false;
        return view.bottom;
    }

    // The tree tabs.
    const std::size_t treeCount = cls.trees.size();
    const int tabGap = S(6);
    // Narrow tabs: the small font, the title on two lines when it needs them.
    const bool narrowTabs = w < S(520);
    HFONT tabFont = narrowTabs ? fontSmall : fontBody;
    const int tabH = narrowTabs ? 2 * smallLine + S(10) : bodyLine + S(14);
    const int tabW = (w - tabGap * static_cast<int>(treeCount - 1)) / static_cast<int>(treeCount);
    for (std::size_t i = 0; i < treeCount; ++i) {
        const skills::Tree& tree = cls.trees[i];
        const int tabLeft = x + static_cast<int>(i) * (tabW + tabGap);
        const RECT r = Rect(tabLeft, y, tabLeft + tabW, y + tabH);
        if (!draw) continue;
        SkillsAddHit(r, kSkTab, tree.n, 0, band);
        const bool current = tree.n == skillsCache.tree;
        bool matched = false;
        if (!query.empty() && !current) {
            for (const skills::Skill* skill : skillsData->SkillsOfTree(cls.id, tree.n))
                if (SkillsLit(skill)) { matched = true; break; }
        }
        const bool hot = SkillsHot(kSkTab, tree.n);
        CornerShadow(dc, r, S(2));
        FillPanel(dc, r, current ? kSelected : matched ? kHighlightFill : hot ? RGB(46, 34, 20) : kPanel, 210);
        FrameColor(dc, r, current || matched ? kGold : hot ? RGB(176, 136, 74) : kBorder);
        const std::wstring tabTitle = SkPlain(SkillsDataText(tree.title));
        const int tabTextW = tabW - S(10);
        const int tabTextH = std::min(MeasureHeight(dc, tabTitle, tabTextW, tabFont), narrowTabs ? 2 * smallLine : bodyLine);
        const int tabTextTop = static_cast<int>(r.top) + (tabH - S(4) - tabTextH) / 2;
        Text(dc, tabTitle, Rect(r.left + S(5), tabTextTop, r.right - S(5), tabTextTop + tabTextH), kHeaderGold, tabFont,
             DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);
        if (current) FillColor(dc, Rect(r.left + S(6), r.bottom - S(4), r.right - S(6), r.bottom - S(2)), kGold);
    }
    y += tabH + S(14);

    // The grid: three columns, the data's rows. Split, the rows spread over the pane's height (within limits).
    const skills::TreeLayout& grid = skillsCache.grid;
    const int columns = skills::kTreeColumns;
    const int colGap = wide ? S(8) : S(14);
    const int tileW = (w - colGap * (columns - 1)) / columns;
    const int rows = std::max(1, grid.rows);
    int tileH = std::max(S(44), 3 * smallLine + S(10));  // the name (two small lines at most) and the level
    bool compact = false;
    if (wide) {
        bool fits = true;
        SkillsTileSize(static_cast<int>(area.bottom) - S(10) - y, rows, tileH, compact, fits);
    }
    const int iconSize = std::clamp(tileW * 30 / 100, S(24), std::min(tileH - S(8), S(40)));
    int rowGap = S(24);
    if (wide && rows > 1) {
        const int room = static_cast<int>(area.bottom) - S(10) - y - rows * tileH;
        rowGap = std::clamp(room / (rows - 1), S(10), S(56));
    }
    const auto cellRect = [&](const skills::TreeCell& cell) {
        const int l = x + cell.col * (tileW + colGap);
        const int t = y + cell.row * (tileH + rowGap);
        return Rect(l, t, l + tileW, t + tileH);
    };
    const int gridBottom = y + rows * tileH + (rows - 1) * rowGap;
    const int selectedCell = SkillsTileOf(skillsCache.skill);
    if (selectedCell >= 0) {
        const RECT r = cellRect(grid.cells[static_cast<std::size_t>(selectedCell)]);
        skillsTileTop = r.top;
        skillsTileBottom = r.bottom;
    }
    if (!draw) return gridBottom;

    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetClip(Gdiplus::Rect(band.left, band.top, band.right - band.left, band.bottom - band.top));

    // The prerequisite lines, behind the tiles: down from the prerequisite, across in the gap above the skill, down into
    // it. The lines of the selected skill are gold.
    for (const skills::TreeLink& link : grid.links) {
        if (link.from >= grid.cells.size() || link.to >= grid.cells.size()) continue;
        const RECT a = cellRect(grid.cells[link.from]);
        const RECT b = cellRect(grid.cells[link.to]);
        const bool mine = static_cast<int>(link.from) == selectedCell || static_cast<int>(link.to) == selectedCell;
        Gdiplus::Pen pen(mine ? Gdiplus::Color(255, 224, 182, 96) : Gdiplus::Color(210, 122, 92, 48),
                         static_cast<Gdiplus::REAL>(std::max(1, S(2))));
        const Gdiplus::REAL ax = static_cast<Gdiplus::REAL>(a.left + a.right) / 2, bx = static_cast<Gdiplus::REAL>(b.left + b.right) / 2;
        if (b.top >= a.bottom) {
            const Gdiplus::REAL midY = static_cast<Gdiplus::REAL>(b.top) - static_cast<Gdiplus::REAL>(rowGap) / 2;
            const Gdiplus::PointF points[] = {{ax, static_cast<Gdiplus::REAL>(a.bottom)}, {ax, midY}, {bx, midY},
                                              {bx, static_cast<Gdiplus::REAL>(b.top)}};
            graphics.DrawLines(&pen, points, 4);
        } else if (b.bottom <= a.top) {
            graphics.DrawLine(&pen, ax, static_cast<Gdiplus::REAL>(a.top), bx, static_cast<Gdiplus::REAL>(b.bottom));
        } else {  // the same row: side to side
            const Gdiplus::REAL cy = static_cast<Gdiplus::REAL>(a.top + a.bottom) / 2;
            const bool rightward = b.left >= a.right;
            graphics.DrawLine(&pen, static_cast<Gdiplus::REAL>(rightward ? a.right : a.left), cy,
                              static_cast<Gdiplus::REAL>(rightward ? b.left : b.right), cy);
        }
    }

    // The tiles: icon, name (up to two lines) and the required level.
    for (std::size_t i = 0; i < grid.cells.size(); ++i) {
        const skills::TreeCell& cell = grid.cells[i];
        if (cell.skill == nullptr) continue;
        const skills::Skill& skill = *cell.skill;
        const RECT r = cellRect(cell);
        SkillsAddHit(r, kSkTile, static_cast<int>(i), 0, band);
        if (r.bottom < clipTop || r.top > clipBottom) continue;
        const bool selected = static_cast<int>(i) == selectedCell;
        const bool hot = SkillsHot(kSkTile, static_cast<int>(i));
        const bool lit = SkillsLit(&skill);
        CornerShadow(dc, r, S(2));
        FillPanel(dc, r, selected ? kSelected : hot ? RGB(46, 34, 20) : kTile, 220);
        FrameColor(dc, r, selected ? kGold : hot ? RGB(176, 136, 74) : kBorder);
        if (selected) FrameColor(dc, Rect(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), kGold);
        const RECT iconRect = Rect(r.left + S(7), r.top + (tileH - iconSize) / 2, r.left + S(7) + iconSize, r.top + (tileH - iconSize) / 2 + iconSize);
        SkillsDrawIcon(dc, &graphics, skill, iconRect);
        const int textLeft = iconRect.right + S(8);
        const int textW = std::max(S(20), static_cast<int>(r.right) - S(6) - textLeft);
        const std::wstring name = SkPlain(skill.name);
        HFONT nameFont = fontSmall;  // one size for every tile, so the names read alike; two lines at most
        const int nameH = std::min(MeasureHeight(dc, name, textW, nameFont), 2 * smallLine);
        const int required = skill.requiredLevel > 0 ? skill.requiredLevel : 1;
        const std::wstring level = UiFormat(L"Lvl {0}", {std::to_wstring(required)});
        const int block = compact ? nameH : nameH + S(2) + smallLine;
        const int textTop = static_cast<int>(r.top) + (tileH - block) / 2;
        Text(dc, name, Rect(textLeft, textTop, textLeft + textW, textTop + nameH), lit ? kGold : kDim, nameFont,
             DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);
        if (compact) {  // no room for the level line: the number sits on the icon's corner
            const RECT badge = Rect(iconRect.right - S(20), iconRect.bottom - smallLine, iconRect.right, iconRect.bottom);
            FillPanel(dc, badge, RGB(0, 0, 0), 190);
            Text(dc, std::to_wstring(required), badge, kBody, fontSmall, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        } else {
            Text(dc, level, Rect(textLeft, textTop + nameH + S(2), textLeft + textW, textTop + nameH + S(2) + smallLine), kDim, fontSmall,
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (!lit && !hot) FillPanel(dc, r, RGB(0, 0, 0), 140);  // not what the search text is looking for
        if (flashing && skillsFlash == &skill) {  // where a search jump arrived: the gold of the card frame (DrawJumpFlash)
            for (int grow = 1; grow <= std::max(2, S(3)); ++grow)
                FrameColor(dc, Rect(r.left - grow, r.top - grow, r.right + grow, r.bottom + grow), RGB(255, 226, 140));
        }
    }
    return gridBottom;
}

// The selected skill: the header, then Values / Items / About. Returns the bottom. Hits are only made when drawing, cut
// to `view`. `graphics` draws the icon (null: no icon, as when only measuring).
int OverlayState::SkillsDetails(HDC dc, Gdiplus::Graphics* graphics, const skills::Class& cls, int x, int w, int y, bool draw,
                                const RECT& view) {
    const skills::Skill* skillPtr = skillsCache.skill;
    if (skillPtr == nullptr || !skillsCache.built) {
        const int h = bodyLine + S(6);
        if (draw) Text(dc, UiText(L"Nothing selected"), Rect(x, y, x + w, y + h), kDim, fontBody, DT_LEFT | DT_SINGLELINE);
        return y + h;
    }
    const skills::Skill& skill = *skillPtr;
    const skills::Data& data = *skillsData;
    const skills::HeaderFacts& facts = skillsCache.facts;
    const skills::ValueTable& table = skillsCache.table;
    const auto onScreen = [&](int top, int bottom) { return draw && bottom >= view.top && top <= view.bottom; };
    // A wrapped paragraph; returns its height.
    const auto paragraph = [&](const std::wstring& text, int px, int pw, int py, COLORREF color, HFONT font) {
        if (text.empty()) return 0;
        const int h = MeasureHeight(dc, text, pw, font);
        if (onScreen(py, py + h)) Text(dc, text, Rect(px, py, px + pw, py + h), color, font, DT_LEFT | DT_WORDBREAK);
        return h;
    };

    // ---- header: icon, name, class and tree, the facts line, tags
    const int iconSize = S(56);
    const int textX = x + iconSize + S(12);
    const int textW = std::max(S(40), w - iconSize - S(12));
    const std::wstring name = SkPlain(skill.name);
    const int nameH = MeasureHeight(dc, name, textW, fontTitle);
    std::wstring where = SkPlain(SkillsDataText(cls.title));
    if (!facts.treeTitle.empty()) where += kSkDot + SkPlain(SkillsDataText(facts.treeTitle));
    std::vector<std::wstring> tags;
    if (facts.passive) tags.push_back(UiText(L"Passive"));
    if (facts.aura) tags.push_back(UiText(L"Aura"));
    std::wstring factsLine;
    const auto addFact = [&](const std::wstring& part) {
        if (part.empty()) return;
        if (!factsLine.empty()) factsLine += kSkDot;
        factsLine += part;
    };
    if (facts.requiredLevel > 0) addFact(UiFormat(L"Req {0}", {std::to_wstring(facts.requiredLevel)}));
    if (facts.maxLevel > 0) addFact(UiFormat(L"Max {0}", {std::to_wstring(facts.maxLevel)}));
    if (facts.hasMana) {
        if (facts.manaLinear && facts.manaPerLevel != 0) {
            const std::string per = (facts.manaPerLevel > 0 ? "+" : "") + skills::FormatNumber(facts.manaPerLevel);
            addFact(UiFormat(L"Mana {0} ({1} per level)", {Wide(skills::FormatNumber(facts.manaAtLevel1)), Wide(per)}));
        } else {
            addFact(UiFormat(L"Mana {0}", {Wide(skills::FormatNumber(facts.manaAtChosen))}));
        }
    }
    const int whereH = MeasureHeight(dc, where, textW, fontSmall);
    const int factsH = MeasureHeight(dc, factsLine, textW, fontSmall);
    int textBlock = nameH + S(2) + whereH + (factsLine.empty() ? 0 : S(2) + factsH);
    const int tagH = smallLine + S(6);
    if (!tags.empty()) textBlock += S(6) + tagH;
    const int headerH = std::max(iconSize, textBlock);
    if (onScreen(y, y + headerH)) {
        const RECT iconRect = Rect(x, y, x + iconSize, y + iconSize);
        if (graphics != nullptr) SkillsDrawIcon(dc, graphics, skill, iconRect);
        int ty = y;
        Text(dc, name, Rect(textX, ty, textX + textW, ty + nameH), kHeaderGold, fontTitle, DT_LEFT | DT_WORDBREAK);
        ty += nameH + S(2);
        Text(dc, where, Rect(textX, ty, textX + textW, ty + whereH), kDim, fontSmall, DT_LEFT | DT_WORDBREAK);
        ty += whereH;
        if (!factsLine.empty()) {
            ty += S(2);
            Text(dc, factsLine, Rect(textX, ty, textX + textW, ty + factsH), kText, fontSmall, DT_LEFT | DT_WORDBREAK);
            ty += factsH;
        }
        if (!tags.empty()) {
            ty += S(6);
            int tx = textX;
            for (const std::wstring& tag : tags) {
                const int tw = MeasureWidth(dc, tag, fontSmall) + S(14);
                const RECT r = Rect(tx, ty, tx + tw, ty + tagH);
                FillPanel(dc, r, kSelected, 200);
                FrameColor(dc, r, kBorder);
                Text(dc, tag, r, kGold, fontSmall, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                tx += tw + S(6);
            }
        }
    }
    y += headerH + S(10);

    // ---- description, prerequisites, the on-striking / on-casting notes, the paired bonus, hard points only
    y += paragraph(SkPlain(skill.description), x, w, y, kBody, fontBody);
    if (!skill.description.empty()) y += S(8);
    const auto field = [&](const std::wstring& label, const std::wstring& value) {
        y += paragraph(UiFormat(L"{0}: {1}", {label, value}), x, w, y, kDim, fontSmall) + S(3);
    };
    if (!skill.prerequisites.empty()) {
        std::wstring list;
        for (const std::string& prerequisite : skill.prerequisites)
            list = list.empty() ? SkPlain(prerequisite) : UiFormat(L"{0}, {1}", {list, SkPlain(prerequisite)});
        field(UiText(L"Requires"), list);
    }
    const auto procText = [&](int flag, const std::string& reason) {
        const std::wstring answer = UiText(flag == 1 ? L"Yes" : L"No");
        return reason.empty() ? answer : UiFormat(L"{0}. {1}", {answer, SkPlain(SkillsDataText(reason))});
    };
    if (skill.onStriking >= 0) field(UiText(L"Procs on striking"), procText(skill.onStriking, skill.onStrikingReason));
    if (skill.onCast >= 0) field(UiText(L"Procs on casting"), procText(skill.onCast, skill.onCastReason));
    for (const skills::PairedBonus& bonus : facts.pairedBonuses) {
        y += S(4);
        y += paragraph(SkPlain(bonus.header), x, w, y, kGold, fontSmall);
        y += paragraph(SkPlain(bonus.text), x + S(10), w - S(10), y, kBody, fontSmall) + S(3);
    }
    if (facts.hardPointsOnly) {
        y += S(4);
        y += paragraph(UiText(L"Hard points only: +skills from items do not count."), x, w, y, RGB(214, 150, 92), fontSmall) + S(3);
    }
    y += S(10);

    // ---- the sub-tabs
    std::size_t sourceCount = 0;
    for (const skills::SourceGroup& group : skillsCache.sources) sourceCount += group.count;
    const std::wstring subLabels[3] = {UiText(L"Values"), UiFormat(L"Items {0}", {std::to_wstring(sourceCount)}), UiText(L"About")};
    const int subH = bodyLine + S(12);
    {
        int tx = x;
        for (int i = 0; i < 3; ++i) {
            const int tw = std::max(S(70), MeasureWidth(dc, subLabels[i], fontBody) + S(26));
            const RECT r = Rect(tx, y, tx + tw, y + subH);
            if (draw) {
                SkillsAddHit(r, kSkSubTab, i, 1, view);
                if (onScreen(r.top, r.bottom)) {
                    const bool current = skillsSubTab == i;
                    SkillsBox(dc, r, subLabels[i], current, SkillsHot(kSkSubTab, i), false, fontBody);
                    if (current) FillColor(dc, Rect(r.left + S(6), r.bottom - S(4), r.right - S(6), r.bottom - S(2)), kGold);
                }
            }
            tx += tw + S(6);
        }
    }
    y += subH;
    if (onScreen(y, y + 1)) FillColor(dc, Rect(x, y + S(1), x + w, y + S(2)), kBorder);
    y += S(12);

    const int boxH = std::max(S(24), smallLine + S(10));
    const auto header = [&](const std::wstring& text) {
        const int h = MeasureHeight(dc, text, w, fontBold);
        if (onScreen(y, y + h)) Text(dc, text, Rect(x, y, x + w, y + h), kHeaderGold, fontBold, DT_LEFT | DT_WORDBREAK);
        y += h + S(4);
    };

    if (skillsSubTab == 0) {
        // ---- Values: the level boxes, the synergy inputs, the constant lines, the table
        const int top = std::max(1, data.levels.max);
        const std::wstring levelLabels[2] = {UiText(L"Skill level:"), UiText(L"Character level:")};
        const int labelW = std::max(MeasureWidth(dc, levelLabels[0], fontSmall), MeasureWidth(dc, levelLabels[1], fontSmall)) + S(10);
        const int valueW = MeasureWidth(dc, std::to_wstring(top), fontSmall) + S(22);
        // - value + boxes; returns the right edge.
        const auto stepper = [&](int bx, int by, int fieldId, const std::wstring& valueText, int boxW, bool atLow, bool atHigh) {
            const RECT minus = Rect(bx, by, bx + boxH, by + boxH);
            const RECT value = Rect(minus.right + S(3), by, minus.right + S(3) + boxW, by + boxH);
            const RECT plus = Rect(value.right + S(3), by, value.right + S(3) + boxH, by + boxH);
            if (draw) {
                const int base = fieldId * 3;
                SkillsAddHit(minus, kSkStep, base, 1, view);
                SkillsAddHit(value, kSkStep, base + 1, 1, view);
                SkillsAddHit(plus, kSkStep, base + 2, 1, view);
                if (onScreen(by, by + boxH)) {
                    SkillsBox(dc, minus, L"-", false, SkillsHot(kSkStep, base), atLow, fontSmall);
                    CornerShadow(dc, value, S(2));
                    FillPanel(dc, value, kSearchFill, 220);
                    FrameColor(dc, value, SkillsHot(kSkStep, base + 1) ? RGB(255, 226, 140) : kGold);
                    Text(dc, valueText, value, kGold, fontSmall, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                    SkillsBox(dc, plus, L"+", false, SkillsHot(kSkStep, base + 2), atHigh, fontSmall);
                }
            }
            return static_cast<int>(plus.right);
        };
        const auto levelRow = [&](int fieldId, int value, bool chips) {
            if (onScreen(y, y + boxH))
                Text(dc, levelLabels[fieldId], Rect(x, y, x + labelW, y + boxH), kDim, fontSmall, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            int cx = stepper(x + labelW, y, fieldId, std::to_wstring(value), valueW, value <= 1, value >= top) + S(12);
            if (chips) {
                // The quick levels: on the same row when they fit, else on the next one.
                std::vector<int> widths;
                int need = 0;
                for (const int level : table.levels) {
                    widths.push_back(std::max(S(34), MeasureWidth(dc, SkLevelText(level), fontSmall) + S(14)));
                    need += widths.back() + S(4);
                }
                int cy = y;
                if (cx + need > x + w) {
                    cy = y + boxH + S(6);
                    cx = x + labelW;
                    y += boxH + S(6);
                }
                for (std::size_t i = 0; i < table.levels.size(); ++i) {
                    const int level = table.levels[i];
                    const RECT r = Rect(cx, cy, cx + widths[i], cy + boxH);
                    if (draw) {
                        SkillsAddHit(r, kSkQuick, level, 1, view);
                        if (onScreen(cy, cy + boxH)) SkillsBox(dc, r, SkLevelText(level), level == skillsLevel, SkillsHot(kSkQuick, level), false, fontSmall);
                    }
                    cx += widths[i] + S(4);
                }
            }
            y += boxH + S(6);
        };
        levelRow(0, skillsLevel, true);
        levelRow(1, skillsCharacterLevel, false);
        y += S(8);

        // Receives bonuses from: each synergy's line with its hard points (or levels), Max all and Reset.
        if (!table.synergies.empty() || !table.synergyNotes.empty()) {
            bool anyAdjustable = false;
            for (const skills::SynergyEntry& entry : table.synergies) anyAdjustable = anyAdjustable || entry.adjustable;
            const std::wstring bonusTitle = UiText(L"Receives bonuses from");
            const std::wstring maxAll = UiText(L"Max all"), reset = UiText(L"Reset");
            const int maxW = MeasureWidth(dc, maxAll, fontSmall) + S(18), resetW = MeasureWidth(dc, reset, fontSmall) + S(18);
            const int chipsW = anyAdjustable ? maxW + resetW + S(6) : 0;
            const int titleW = std::max(S(40), w - chipsW - S(10));
            const int titleH = std::max(boldLine, anyAdjustable ? boxH : 0);
            if (onScreen(y, y + titleH))
                Text(dc, bonusTitle, Rect(x, y, x + titleW, y + titleH), kHeaderGold, fontBold, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            if (anyAdjustable && draw) {
                const RECT maxRect = Rect(x + w - chipsW, y, x + w - chipsW + maxW, y + boxH);
                const RECT resetRect = Rect(maxRect.right + S(6), y, maxRect.right + S(6) + resetW, y + boxH);
                SkillsAddHit(maxRect, kSkMaxAll, 0, 1, view);
                SkillsAddHit(resetRect, kSkReset, 0, 1, view);
                if (onScreen(y, y + boxH)) {
                    SkillsBox(dc, maxRect, maxAll, false, SkillsHot(kSkMaxAll, 0), false, fontSmall);
                    SkillsBox(dc, resetRect, reset, false, SkillsHot(kSkReset, 0), false, fontSmall);
                }
            }
            y += titleH + S(6);
            const int pointsW = MeasureWidth(dc, std::to_wstring(top), fontSmall) + S(22);
            const int boxesW = 2 * boxH + pointsW + S(6);
            for (std::size_t i = 0; i < table.synergies.size(); ++i) {
                const skills::SynergyEntry& entry = table.synergies[i];
                std::wstring text = SkPlain(entry.text);
                if (text.empty() && entry.skill != nullptr) text = SkPlain(entry.skill->name);
                const int lineW = entry.adjustable ? std::max(S(40), w - boxesW - S(14)) : w;
                const int textH = MeasureHeight(dc, text, lineW - S(10), fontSmall);
                const int h = std::max(textH, entry.adjustable ? boxH : 0);
                if (onScreen(y, y + h)) {
                    FillPanel(dc, Rect(x, y - S(2), x + w, y + h + S(2)), kTile, 110);
                    const int textTop = y + (h - textH) / 2;  // a one-line text beside its boxes sits in their middle
                    Text(dc, text, Rect(x + S(6), textTop, x + S(6) + lineW - S(10), textTop + textH), kBody, fontSmall, DT_LEFT | DT_WORDBREAK);
                }
                if (entry.adjustable)
                    stepper(x + w - boxesW, y + (h - boxH) / 2, 2 + static_cast<int>(i), std::to_wstring(entry.points), pointsW,
                            entry.points <= 0, entry.points >= entry.maxPoints);
                y += h + S(6);
            }
            for (const std::string& note : table.synergyNotes) y += paragraph(SkPlain(note), x, w, y, kDim, fontSmall) + S(3);
            y += S(8);
        }

        // At level N: the lines that are the same at every sampled level.
        if (!table.constants.empty()) {
            header(UiFormat(L"At level {0}", {std::to_wstring(skillsLevel)}));
            for (const skills::ConstantLine& line : table.constants) y += paragraph(SkPlain(line.text), x + S(6), w - S(6), y, kBody, fontSmall) + S(2);
            y += S(10);
        }

        // The table: label, the value at the chosen level (gold), and the sampled levels (dim).
        if (!table.rows.empty()) {
            const std::size_t samples = table.levels.size();
            const std::wstring chosenHead = UiFormat(L"Lvl {0}", {SkLevelText(skillsLevel)});
            int chosenW = MeasureWidth(dc, chosenHead, fontSmall);
            int sampleW = 0;
            for (const int level : table.levels) sampleW = std::max(sampleW, MeasureWidth(dc, UiFormat(L"Lvl {0}", {SkLevelText(level)}), fontSmall));
            for (const skills::ValueRow& row : table.rows) {
                if (!row.splitOk) continue;
                chosenW = std::max(chosenW, MeasureWidth(dc, Wide(row.chosen.value), fontBold));
                for (const skills::ValueCell& cell : row.samples) sampleW = std::max(sampleW, MeasureWidth(dc, Wide(cell.value), fontSmall));
            }
            chosenW = std::min(chosenW + S(14), w * 30 / 100);
            sampleW = std::min(sampleW + S(10), w * 16 / 100);
            int labelColW = w - chosenW - static_cast<int>(samples) * sampleW;
            const bool twoLines = labelColW < w * 32 / 100;  // too narrow: the samples go under the value
            if (twoLines) labelColW = w - chosenW;
            const int sampleX = twoLines ? x + S(12) : x + labelColW + chosenW;
            const int sampleColW = twoLines ? (w - S(12)) / std::max<int>(1, static_cast<int>(samples)) : sampleW;
            const auto cellText = [](const skills::ValueCell& cell) { return cell.present ? SkPlain(cell.value) : std::wstring(kSkDash); };
            // The header row.
            const int headH = smallLine + S(6);
            if (onScreen(y, y + headH * (twoLines ? 2 : 1))) {
                Text(dc, chosenHead, Rect(x + labelColW, y, x + labelColW + chosenW, y + headH), kGold, fontSmall, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                const int sy = twoLines ? y + headH : y;
                for (std::size_t i = 0; i < samples; ++i) {
                    const int sx = sampleX + static_cast<int>(i) * sampleColW;
                    Text(dc, UiFormat(L"Lvl {0}", {SkLevelText(table.levels[i])}), Rect(sx, sy, sx + sampleColW - S(4), sy + headH), kDim, fontSmall,
                         DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                }
            }
            y += headH * (twoLines ? 2 : 1);
            if (onScreen(y, y + 1)) FillColor(dc, Rect(x, y, x + w, y + 1), kBorder);
            y += S(4);
            bool shade = false;
            for (const skills::ValueRow& row : table.rows) {
                const bool split = row.splitOk;
                const std::wstring label = split ? SkPlain(row.label) : SkPlain(row.chosen.text);
                const int cellLabelW = split ? labelColW - S(10) : w - S(10);
                const int labelH = MeasureHeight(dc, label, cellLabelW, split ? fontSmall : fontBold);
                const int lineH = split ? std::max(labelH, boldLine) : labelH;
                const bool samplesBelow = twoLines || !split;
                const int h = lineH + (samplesBelow ? S(2) + smallLine : 0);
                if (onScreen(y, y + h + S(4))) {
                    if (shade) FillPanel(dc, Rect(x, y - S(2), x + w, y + h + S(2)), kTile, 120);
                    Text(dc, label, Rect(x + S(4), y, x + S(4) + cellLabelW, y + labelH), split ? kBody : kGold, split ? fontSmall : fontBold, DT_LEFT | DT_WORDBREAK);
                    if (split)
                        Text(dc, cellText(row.chosen), Rect(x + labelColW, y, x + labelColW + chosenW - S(4), y + lineH), kGold, fontBold,
                             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    const int sy = samplesBelow ? y + lineH + S(2) : y + (lineH - smallLine) / 2;
                    const int sx0 = samplesBelow ? x + S(12) : sampleX;
                    const int scw = samplesBelow ? (w - S(12)) / std::max<int>(1, static_cast<int>(samples)) : sampleColW;
                    for (std::size_t i = 0; i < row.samples.size() && i < samples; ++i) {
                        const int sx = sx0 + static_cast<int>(i) * scw;
                        Text(dc, cellText(row.samples[i]), Rect(sx, sy, sx + scw - S(4), sy + smallLine + S(2)), kDim, fontSmall,
                             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                }
                y += h + S(5);
                shade = !shade;
            }
            bool beyond = false;
            for (const int level : table.levels) beyond = beyond || skills::BeyondReach(level);
            if (beyond || skills::BeyondReach(skillsLevel)) y += paragraph(UiText(L"* beyond what a character can reach"), x, w, y, kDim, fontSmall) + S(4);
            y += S(6);

            // All levels: every level from 1 to the cap, one row each.
            const std::wstring allText = UiText(L"All levels");
            const RECT allRect = Rect(x, y, x + MeasureWidth(dc, allText, fontSmall) + S(22), y + boxH);
            if (draw) {
                SkillsAddHit(allRect, kSkAllLevels, 0, 1, view);
                if (onScreen(allRect.top, allRect.bottom)) SkillsBox(dc, allRect, allText, skillsAllLevels, SkillsHot(kSkAllLevels, 0), false, fontSmall);
            }
            y += boxH + S(8);
            if (skillsAllLevels && skillsCache.allBuilt && skillsCache.all.size() == table.rows.size()) {
                const std::size_t columns = table.rows.size();
                const int levelColW = MeasureWidth(dc, std::to_wstring(top) + L"*", fontSmall) + S(14);
                const int colW = std::max(S(30), (w - levelColW) / std::max<int>(1, static_cast<int>(columns)));
                int headRowH = 0;
                for (const skills::ValueRow& row : table.rows) {
                    const std::wstring label = SkPlain(row.splitOk ? row.label : row.prefix);
                    headRowH = std::max(headRowH, std::min(MeasureHeight(dc, label, colW - S(6), fontSmall), 3 * smallLine));
                }
                headRowH = std::max(headRowH, smallLine) + S(4);
                if (onScreen(y, y + headRowH)) {
                    Text(dc, UiText(L"Lvl"), Rect(x, y, x + levelColW, y + headRowH), kDim, fontSmall, DT_LEFT | DT_SINGLELINE | DT_BOTTOM);
                    for (std::size_t c = 0; c < columns; ++c) {
                        const int cx = x + levelColW + static_cast<int>(c) * colW;
                        const std::wstring label = SkPlain(table.rows[c].splitOk ? table.rows[c].label : table.rows[c].prefix);
                        Text(dc, label, Rect(cx, y, cx + colW - S(6), y + headRowH), kDim, fontSmall, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);
                    }
                }
                y += headRowH;
                if (onScreen(y, y + 1)) FillColor(dc, Rect(x, y, x + w, y + 1), kBorder);
                y += S(3);
                const int rowH = smallLine + S(4);
                const std::size_t levelCount = skillsCache.all.empty() ? 0 : skillsCache.all.front().size();
                for (std::size_t l = 0; l < levelCount; ++l) {
                    const int level = static_cast<int>(l) + 1;
                    if (onScreen(y, y + rowH)) {
                        const bool chosen = level == skillsLevel;
                        if (chosen) FillPanel(dc, Rect(x, y, x + w, y + rowH), kSelected, 200);
                        else if (l % 2 == 1) FillPanel(dc, Rect(x, y, x + w, y + rowH), kTile, 110);
                        Text(dc, SkLevelText(level), Rect(x + S(4), y, x + levelColW, y + rowH), chosen ? kGold : kDim, fontSmall,
                             DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                        for (std::size_t c = 0; c < columns; ++c) {
                            if (l >= skillsCache.all[c].size()) continue;
                            const int cx = x + levelColW + static_cast<int>(c) * colW;
                            Text(dc, cellText(skillsCache.all[c][l]), Rect(cx, y, cx + colW - S(6), y + rowH), chosen ? kGold : kBody, fontSmall,
                                 DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                        }
                    }
                    y += rowH;
                }
                y += S(8);
            }
        }
    } else if (skillsSubTab == 1) {
        // ---- Items: the sources grouped by kind, the value on the right
        if (skillsCache.sources.empty()) {
            y += paragraph(UiText(L"No item grants this skill."), x, w, y, kDim, fontBody) + S(6);
        }
        for (const skills::SourceGroup& group : skillsCache.sources) {
            header(UiFormat(L"{0} ({1})", {SkPlain(SkillsDataText(group.kindText)), std::to_wstring(group.count)}));
            for (const skills::Source* source : group.items) {
                std::wstring value = SkPlain(SkillsDataText(source->v));
                if (source->c > 1) value += kSkTimes + std::to_wstring(source->c);
                const std::wstring itemName = SkPlain(SkillsDataText(source->n));
                const int valueW = std::min(MeasureWidth(dc, value, fontSmall), w * 45 / 100);
                const int nameW = std::max(S(40), w - valueW - S(20));
                const int valueH = value.empty() ? 0 : MeasureHeight(dc, value, valueW + S(2), fontSmall);
                const int h = std::max(MeasureHeight(dc, itemName, nameW, fontSmall), valueH);
                if (onScreen(y, y + h)) {
                    Text(dc, itemName, Rect(x + S(6), y, x + S(6) + nameW, y + h), kBody, fontSmall, DT_LEFT | DT_WORDBREAK);
                    if (!value.empty())
                        Text(dc, value, Rect(x + w - valueW - S(2), y, x + w, y + h), kDim, fontSmall, DT_RIGHT | DT_WORDBREAK);
                }
                y += h + S(3);
            }
            y += S(8);
        }
    } else {
        // ---- About: the class introduction, the data's notes, the * levels
        if (!cls.intro.empty()) y += paragraph(SkPlain(SkillsDataText(cls.intro)), x, w, y, kBody, fontBody) + S(8);
        if (!data.notes.empty()) {
            header(UiText(L"Notes"));
            for (const std::string& note : data.notes) y += paragraph(SkPlain(SkillsDataText(note)), x, w, y, kBody, fontSmall) + S(6);
        }
        y += paragraph(UiFormat(L"Skill levels above {0} are marked *: no character can reach them, but the numbers are still worked out.",
                                {std::to_wstring(skills::kReachableSkillLevel)}),
                       x, w, y, kDim, fontSmall) + S(6);
    }
    return y + S(6);
}

// ---- mouse ----------------------------------------------------------------------------------------------------------

bool OverlayState::SkillsMove(int x, int y) {
    if (!SkillsActive()) return false;
    int kind = -1, value = 0;
    for (const SkillsHit& hit : skillsHits) {
        if (!Inside(hit.rect, x, y)) continue;
        kind = hit.kind;
        value = hit.value;
        break;
    }
    if (kind != skillsHoverKind || value != skillsHoverValue) {
        skillsHoverKind = kind;
        skillsHoverValue = value;
        InvalidateRect(window, nullptr, FALSE);
    }
    return true;
}

bool OverlayState::SkillsClick(int x, int y) {
    if (!SkillsActive() || !Inside(layout.detail, x, y)) return false;
    if (SkillsBarDown(x, y)) return true;
    for (const SkillsHit& hit : skillsHits) {
        if (!Inside(hit.rect, x, y)) continue;
        SkillsActivate(hit.kind, hit.value);
        return true;
    }
    return false;
}

// The wheel: over a - / value / + box it changes that value (up: higher); over the details panel or the item list (split)
// it scrolls them. False where the pane itself scrolls (stacked).
bool OverlayState::SkillsWheel(int x, int y, int steps) {
    if (!SkillsActive() || !Inside(layout.detail, x, y)) return false;
    for (const SkillsHit& hit : skillsHits) {
        if (hit.kind != kSkStep || !Inside(hit.rect, x, y)) continue;
        SkillsStepField(hit.value / 3, steps);
        return true;
    }
    if (!skillsWide) return false;
    if (Inside(skillsDetailView, x, y)) {
        skillsDetailScroll = std::clamp(skillsDetailScroll - steps * S(60), 0, skillsDetailMax);
    } else if (Inside(skillsListView, x, y)) {
        skillsListScroll = std::clamp(skillsListScroll - steps * S(60), 0, skillsListMax);
    }
    skillsHoverKind = -1;  // the box under a still pointer changed
    InvalidateRect(window, nullptr, FALSE);
    return true;
}

// ---- the controller -------------------------------------------------------------------------------------------------

// What the D-pad can land on: every hit but the - and + buttons (Left and Right on the value do what they do).
static bool SkillsPadTarget(int kind, int value) { return !(kind == kSkStep && value % 3 != 1); }

bool OverlayState::SkillsPadFocusTile() {
    const int tile = SkillsTileOf(skillsCache.skill);
    for (const SkillsHit& hit : skillsHits) {
        if (hit.kind == kSkTile && hit.value == tile) {
            skillsPadKind = kSkTile;
            skillsPadValue = tile;
            return true;
        }
    }
    for (const SkillsHit& hit : skillsHits) {
        if (!SkillsPadTarget(hit.kind, hit.value)) continue;
        skillsPadKind = hit.kind;
        skillsPadValue = hit.value;
        return true;
    }
    return false;
}

void OverlayState::PadEnterSkills() {
    if (skillsHits.empty()) return;
    padZone = PadZone::Skills;
    skillsHoverKind = -1;
    SkillsPadFocusTile();
    SkillsPadReveal();
}

const OverlayState::SkillsHit* OverlayState::SkillsPadHit() const {
    for (const SkillsHit& hit : skillsHits)
        if (hit.kind == skillsPadKind && hit.value == skillsPadValue) return &hit;
    return nullptr;
}

bool OverlayState::SkillsPadRect(RECT& out) const {
    const SkillsHit* hit = SkillsPadHit();
    if (hit == nullptr) return false;
    out = hit->rect;
    return true;
}

// Scrolls the highlighted box into view: the details panel or the item list (split), or the pane (stacked).
void OverlayState::SkillsPadReveal() {
    const SkillsHit* hit = SkillsPadHit();
    if (hit == nullptr) return;
    const RECT& r = hit->full;
    const int margin = S(8);
    if (skillsWide) {
        const bool inList = hit->zone == 0 && skillsCache.items;
        const RECT& view = inList ? skillsListView : skillsDetailView;
        if (hit->zone == 0 && !inList) return;  // the tree does not scroll
        int& scroll = inList ? skillsListScroll : skillsDetailScroll;
        const int maxScroll = inList ? skillsListMax : skillsDetailMax;
        int delta = 0;
        if (r.top < view.top + margin) delta = r.top - (view.top + margin);
        else if (r.bottom > view.bottom - margin) delta = r.bottom - (view.bottom - margin);
        if (delta == 0) return;
        scroll = std::clamp(scroll + delta, 0, maxScroll);
    } else {
        int delta = 0;
        if (r.top < clipTop + margin) delta = r.top - (clipTop + margin);
        else if (r.bottom > layout.detail.bottom - margin) delta = r.bottom - (layout.detail.bottom - margin);
        if (delta == 0) return;
        PageView& view = CurrentView();
        view.detailScroll = std::clamp(view.detailScroll + delta, 0, std::max(0, scrollTotal - scrollVisible));
    }
    InvalidateRect(window, nullptr, FALSE);
}

// D-pad: to the nearest box that way (between the tiles, into the details and back). On a value Left and Right change
// it (held, faster), as on the calculator's numbers. Up or Down with nothing more that way scrolls the panel on.
void OverlayState::SkillsPadMove(int dx, int dy) {
    if (!SkillsActive() || skillsHits.empty()) { padZone = PadZone::List; return; }
    skillsHoverKind = -1;
    if (dx != 0 && dy == 0 && skillsPadKind == kSkStep && skillsPadValue % 3 == 1) {
        const ULONGLONG now = GetTickCount64();
        skillsHold = now - skillsHoldAt > 400 ? 1 : skillsHold + 1;
        skillsHoldAt = now;
        if (SkillsStepField(skillsPadValue / 3, dx * esrwiki::pad::NumberStep(skillsHold))) return;
    }
    std::vector<esrwiki::pad::Box> boxes;
    std::vector<std::size_t> which;
    int current = -1;
    for (std::size_t i = 0; i < skillsHits.size(); ++i) {
        const SkillsHit& hit = skillsHits[i];
        if (!SkillsPadTarget(hit.kind, hit.value)) continue;
        if (hit.kind == skillsPadKind && hit.value == skillsPadValue) current = static_cast<int>(boxes.size());
        boxes.push_back({hit.rect.left, hit.rect.top, hit.rect.right, hit.rect.bottom});
        which.push_back(i);
    }
    if (boxes.empty()) { padZone = PadZone::List; return; }
    if (current < 0) {
        SkillsPadFocusTile();
        SkillsPadReveal();
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    const SkillsHit from = skillsHits[which[static_cast<std::size_t>(current)]];
    int next = esrwiki::pad::Neighbor(boxes, current, dx, dy);
    if (next < 0 && dx != 0 && skillsWide) {
        // Sideways out of the tree: the nearest box of the details at about that height; out of the details: the tile.
        if (dx > 0 && from.zone == 0) {
            const int centre = (from.rect.top + from.rect.bottom) / 2;
            int best = -1, bestDistance = 0;
            for (std::size_t b = 0; b < which.size(); ++b) {
                const SkillsHit& hit = skillsHits[which[b]];
                if (hit.zone != 1) continue;
                const int distance = std::abs((hit.rect.top + hit.rect.bottom) / 2 - centre);
                if (best < 0 || distance < bestDistance) { best = static_cast<int>(b); bestDistance = distance; }
            }
            next = best;
        } else if (dx < 0 && from.zone == 1) {
            SkillsPadFocusTile();
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
    }
    if (next < 0) {
        if (dy != 0) {  // nothing more that way: scroll on, so what is below (or above) comes into view
            if (skillsWide && from.zone == 1) skillsDetailScroll = std::clamp(skillsDetailScroll + dy * S(120), 0, skillsDetailMax);
            else if (skillsWide && skillsCache.items) skillsListScroll = std::clamp(skillsListScroll + dy * S(60), 0, skillsListMax);
            else if (!skillsWide) {
                PageView& view = CurrentView();
                view.detailScroll = std::clamp(view.detailScroll + dy * S(120), 0, std::max(0, scrollTotal - scrollVisible));
            }
        } else if (dx < 0) {
            padZone = PadZone::List;
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    const SkillsHit& to = skillsHits[which[static_cast<std::size_t>(next)]];
    skillsPadKind = to.kind;
    skillsPadValue = to.value;
    SkillsPadReveal();
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::SkillsPadConfirm() {
    if (SkillsPadHit() == nullptr) return;
    if (skillsPadKind == kSkStep) return;  // a value: Left and Right change it
    SkillsActivate(skillsPadKind, skillsPadValue);
    if (skillsPadKind == kSkTab) skillsPadFollow = true;  // on to the new tree's first tile once it is laid out
}

// B: from the details back to the selected tile; false on the tree (the categories come next).
bool OverlayState::SkillsPadBack() {
    const SkillsHit* hit = SkillsPadHit();
    if (hit == nullptr || hit->zone == 0) return false;
    SkillsPadFocusTile();
    SkillsPadReveal();
    return true;
}

// LB / RB on the page's boxes: the previous / next tree.
bool OverlayState::SkillsPadStepTab(int direction) {
    if (padZone != PadZone::Skills || !SkillsActive()) return false;
    const skills::Class* cls = SkillsShownClass();
    if (cls == nullptr || cls->trees.empty()) return false;
    const int count = static_cast<int>(cls->trees.size());
    int at = 0;
    for (int i = 0; i < count; ++i)
        if (cls->trees[static_cast<std::size_t>(i)].n == skillsCache.tree) at = i;
    SkillsSelectTree(cls->trees[static_cast<std::size_t>(((at + direction) % count + count) % count)].n);
    skillsPadFollow = true;
    return true;
}

// After a paint: the highlight goes to the selected tile when a tree change asked for it.
void OverlayState::SkillsPadAfterPaint() {
    if (!skillsPadFollow) return;
    skillsPadFollow = false;
    if (padZone == PadZone::Skills) SkillsPadFocusTile();
}
