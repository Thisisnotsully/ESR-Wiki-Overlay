// ---------------------------------------------------------------------------------------------------------------
// The Drop Finder panel (its own window; dropMode in overlay.cpp). It shows the website's drop calculator: a setup
// column (monster, zone, difficulty, monster type, terror zone, players, Magic Find, Item Quantity, item) with a
// picker for the monster, zone and item, and two tables: what the monster drops, and what drops the item. The chances
// come from drop_finder_model.cpp; this file only lays the panel out, draws it and takes its input. Typing goes to the
// window's search edit control, which is moved over the field being edited. Included by overlay.cpp after
// OverlayState is defined, and uses that file's drawing helpers.
// ---------------------------------------------------------------------------------------------------------------

namespace {

// What the open picker or number field edits.
constexpr int kPickNone = 0, kPickMonster = 1, kPickZone = 2, kPickItem = 3, kPickPlayers = 4, kPickMagicFind = 5,
              kPickItemQuantity = 6, kPickFilter = 7;
// What a click on a panel hit area does.
constexpr int kHitSwitch = 0, kHitClose = 1, kHitField = 2, kHitStep = 3, kHitCandidate = 4, kHitList = 5,
              kHitTableDrops = 6, kHitTableWho = 7, kHitSortDrops = 8, kHitSortWho = 9, kHitFilter = 10, kHitRowDrops = 11, kHitRowWho = 12;
// The steppable setup rows (kHitStep index = row * 2 + (0 back, 1 forward)).
constexpr int kStepDifficulty = 0, kStepType = 1, kStepTerror = 2;

const std::map<std::string, std::string>& DropStrings(const drop::DropFinderData* data) {
    static const std::map<std::string, std::string> none;
    return data != nullptr ? data->strings : none;
}

std::wstring DropUi(const std::map<std::string, std::string>& strings, const char* key, const char* fallback) {
    const auto found = strings.find(key);
    return Wide(found == strings.end() ? std::string(fallback) : found->second);
}

std::wstring ReplaceFirst(std::wstring text, const std::wstring& what, const std::wstring& with) {
    const std::size_t at = text.find(what);
    if (at != std::wstring::npos) text.replace(at, what.size(), with);
    return text;
}

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

bool ContainsText(const std::wstring& haystack, const std::wstring& needle) {
    return needle.empty() || Lower(haystack).find(Lower(needle)) != std::wstring::npos;
}

// A chance as a percentage: "12.34%" from 1 percent up, "0.0123%" below that, "-" for none.
std::wstring ChanceText(double p) {
    if (p <= 0) return L"-";
    char buffer[48];
    if (p >= 0.01) {
        std::snprintf(buffer, sizeof(buffer), "%.2f%%", p * 100);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.4f%%", p * 100);
    }
    return Wide(buffer);
}

// Orders for the two tables: column 0 name, then chance and quest. Returns <0, 0 or >0.
int CompareDropRows(const drop::DropRow& a, const drop::DropRow& b, int column) {
    if (column == 1) return a.chance < b.chance ? -1 : a.chance > b.chance ? 1 : 0;
    if (column == 2) return a.quest < b.quest ? -1 : a.quest > b.quest ? 1 : 0;
    return a.name.compare(b.name);
}

int CompareWhoRows(const drop::WhoRow& a, const drop::WhoRow& b, int column) {
    if (column == 1) return a.zone - b.zone;
    if (column == 2) return a.diff - b.diff;
    if (column == 3) return a.type - b.type;
    if (column == 4) return a.chance < b.chance ? -1 : a.chance > b.chance ? 1 : 0;
    if (column == 5) return a.quest < b.quest ? -1 : a.quest > b.quest ? 1 : 0;
    return a.monster.name.compare(b.monster.name);
}

struct DropColumn {
    std::wstring title;
    int width;  // 0 takes the space left over
    bool right;
};

}  // namespace

void OverlayState::DrawSwitchButton(HDC dc, const RECT& rect, const std::wstring& label, bool current) {
    if (current) {
        // The panel the button is on: the dark selected fill with gold lettering, so it reads as the active one.
        FillPanel(dc, rect, kSelected, 235);
        FrameColor(dc, rect, kGold);
        Text(dc, label, rect, kHeaderGold, fontBold, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }
    // Like the launcher's gold PLAY button: a gold fill with dark lettering.
    FillPanel(dc, rect, RGB(196, 152, 66), 235);
    FrameColor(dc, rect, RGB(250, 226, 160));
    FrameColor(dc, Rect(rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1), RGB(122, 88, 28));
    Text(dc, label, rect, RGB(58, 40, 14), fontBold, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

// The other panel opens while this one stays up, and this one hides once the other has painted, so the dimmed
// backdrop and the cursor never drop out in between.
void OverlayState::BeginSwitch(const std::function<void()>& onSwitch) {
    open = false;
    EndDrag();
    ClearHover();
    if (onSwitch) {
        handingOff = true;
        gHandoffActive = true;
        gHandoffReady = false;
        handoffStarted = GetTickCount64();
        handoffSettled = false;
        gHandoffForeground = previousForeground;
        onSwitch();
        SetTimer(window, kTimerHandoff, kHandoffPollMs, nullptr);
    } else {
        Hide();
    }
}

void OverlayState::ApplyPendingDrop() {
    std::shared_ptr<const drop::DropFinderData> next;
    {
        std::lock_guard<std::mutex> guard(dropLock);
        next = std::move(dropPending);
    }
    if (!next) return;
    dropData = next;
    dropEngine = std::make_unique<drop::Engine>(next->data);
    dropMonsters = drop::MonsterList(next->data, next->names);
    dropEveryMonster = drop::EveryMonster(next->data, next->names);
    dropItems = drop::ItemList(next->data, next->names);
    dropDirty = true;
    if (window != nullptr) InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::DropRecompute() {
    if (!dropDirty) return;
    dropDirty = false;
    dropRows.clear();
    dropWho.clear();
    if (!dropData || !dropEngine) return;
    if (dropHasMonster) {
        dropRows = drop::DropsOf(*dropEngine, dropData->names, dropMonster, dropSettings, dropSetup);
    }
    if (dropHasItem) {
        dropWho = drop::WhoDrops(*dropEngine, dropData->data, dropData->names, dropSettings, dropSetup.terror, dropItem.key);
    }
}

// The candidates the open picker lists. A zone candidate is the zone index plus one, so 0 is "No fixed zone";
// a monster or item candidate is its index in the list.
// Whether a zone spawns the monster at this difficulty, and whether it spawns any monster at all.
static bool ZoneHasMonster(const drop::Zone& zone, int diff, int monster) {
    for (const auto& group : zone.groups) {
        for (int m : group[static_cast<std::size_t>(diff)]) {
            if (m == monster) return true;
        }
    }
    return false;
}

static bool ZoneHasAnyMonster(const drop::Zone& zone, int diff) {
    for (const auto& group : zone.groups) {
        if (!group[static_cast<std::size_t>(diff)].empty()) return true;
    }
    return false;
}

void OverlayState::ComputeDropCandidates() {
    dropCandidates.clear();
    if (!dropData) return;
    const drop::Names& names = dropData->names;
    const auto& strings = dropData->strings;
    const auto& zones = dropData->data.zones;
    const bool zoneChosen = dropSetup.zone >= 0 && static_cast<std::size_t>(dropSetup.zone) < zones.size();
    if (dropPicker == kPickMonster) {
        // With a zone chosen, only the monsters that zone spawns at this difficulty, as the website lists them.
        for (std::size_t i = 0; i < dropMonsters.size(); ++i) {
            const drop::MonsterEntry& m = dropMonsters[i];
            if (zoneChosen && (m.superunique || !ZoneHasMonster(zones[static_cast<std::size_t>(dropSetup.zone)], dropSetup.diff, m.index))) {
                continue;
            }
            if (ContainsText(Wide(m.name), dropText)) dropCandidates.push_back(static_cast<int>(i));
        }
    } else if (dropPicker == kPickZone) {
        if (ContainsText(DropUi(strings, "dfNoZone", "No fixed zone"), dropText)) dropCandidates.push_back(0);
        // Only zones that spawn monsters at this difficulty, as the website lists them.
        for (std::size_t i = 0; i < names.zones.size() && i < zones.size(); ++i) {
            if (!ZoneHasAnyMonster(zones[i], dropSetup.diff)) continue;
            if (ContainsText(Wide(names.zones[i]), dropText)) dropCandidates.push_back(static_cast<int>(i) + 1);
        }
    } else if (dropPicker == kPickItem) {
        for (std::size_t i = 0; i < dropItems.size(); ++i) {
            if (ContainsText(Wide(dropItems[i].name), dropText)) dropCandidates.push_back(static_cast<int>(i));
        }
    }
}

// A zone's name, with its act added when more than one zone shares the name, as the website does.
// -1 is no fixed zone.
static std::wstring ZoneLabel(const drop::DropFinderData& data, int zone) {
    if (zone < 0 || static_cast<std::size_t>(zone) >= data.names.zones.size()) {
        return DropUi(data.strings, "dfNoZone", "No fixed zone");
    }
    const std::string& name = data.names.zones[static_cast<std::size_t>(zone)];
    int same = 0;
    for (const std::string& other : data.names.zones) {
        if (other == name) ++same;
    }
    std::wstring label = Wide(name);
    if (same > 1 && static_cast<std::size_t>(zone) < data.data.zones.size()) {
        const int act = data.data.zones[static_cast<std::size_t>(zone)].act;
        if (act >= 0 && static_cast<std::size_t>(act) < data.names.acts.size()) {
            label += L" (" + Wide(data.names.acts[static_cast<std::size_t>(act)]) + L")";
        }
    }
    return label;
}

std::wstring OverlayState::DropCandidateName(int value) const {
    if (!dropData) return L"";
    if (dropPicker == kPickMonster) return Wide(dropMonsters[static_cast<std::size_t>(value)].name);
    if (dropPicker == kPickZone) return ZoneLabel(*dropData, value - 1);
    return Wide(dropItems[static_cast<std::size_t>(value)].name);
}

void OverlayState::SetDropPicker(int picker) {
    dropPicker = picker;
    dropScrollList = 0;
    dropText.clear();
    if (picker == kPickPlayers) dropText = std::to_wstring(dropSettings.players);
    if (picker == kPickMagicFind) dropText = std::to_wstring(dropSettings.mf);
    if (picker == kPickItemQuantity) dropText = std::to_wstring(dropSettings.iq);
    if (IsWindow(search)) {
        SetWindowTextW(search, dropText.c_str());
        SendMessageW(search, EM_SETSEL, dropText.size(), dropText.size());  // the caret goes to the end
        SetFocus(search);
    }
}

void OverlayState::CloseDropPicker() {
    dropPicker = kPickNone;
    dropText.clear();
    dropCandidates.clear();
    if (IsWindow(search)) {
        ShowWindow(search, SW_HIDE);
        SetWindowTextW(search, L"");
    }
}

void OverlayState::ChooseDropCandidate(int value) {
    if (dropPicker == kPickMonster) {
        dropMonster = dropMonsters[static_cast<std::size_t>(value)];
        dropHasMonster = true;
    } else if (dropPicker == kPickZone) {
        dropSetup.zone = value - 1;
    } else if (dropPicker == kPickItem) {
        dropItem = dropItems[static_cast<std::size_t>(value)];
        dropHasItem = true;
    }
    dropDirty = true;
    CloseDropPicker();
}

void OverlayState::StepDrop(int index) {
    const int row = index / 2;
    const int step = index % 2 == 1 ? 1 : -1;
    if (row == kStepDifficulty) {
        dropSetup.diff = (dropSetup.diff + step + 3) % 3;
        if (dropPicker == kPickMonster || dropPicker == kPickZone) ComputeDropCandidates();  // the lists follow the difficulty
    } else if (row == kStepType) {
        dropSetup.type = (dropSetup.type + step + 4) % 4;
    } else if (row == kStepTerror) {
        dropSetup.terror = (dropSetup.terror + step + 3) % 3;
    }
    dropDirty = true;
}

void OverlayState::DropTextChanged() {
    if (!IsWindow(search) || dropPicker == kPickNone) return;
    const int length = GetWindowTextLengthW(search);
    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(search, buffer.data(), length + 1);
    dropText = buffer.data();
    if (dropPicker == kPickFilter) {
        // A column's search: the text narrows that column at once.
        dropFilters[static_cast<std::size_t>(dropFilterTarget)] = dropText;
        dropScrollRows = 0;
        dropScrollWho = 0;
        if (window != nullptr) InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (dropPicker == kPickPlayers || dropPicker == kPickMagicFind || dropPicker == kPickItemQuantity) {
        // A number field: digits, with a leading minus for Magic Find.
        std::wstring digits;
        for (wchar_t c : dropText) {
            if ((c >= L'0' && c <= L'9') || (c == L'-' && digits.empty())) digits += c;
        }
        const int value = digits.empty() || digits == L"-" ? 0 : static_cast<int>(std::wcstol(digits.c_str(), nullptr, 10));
        if (dropPicker == kPickPlayers) {
            dropSettings.players = std::clamp(value, 1, 8);
        } else if (dropPicker == kPickMagicFind) {
            dropSettings.mf = std::clamp(value, -100, 400);
        } else {
            dropSettings.iq = std::clamp(value, 0, 400);
        }
        dropDirty = true;
    }
    dropScrollList = 0;
    ComputeDropCandidates();
    if (window != nullptr) InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::DropEnter() {
    if (dropPicker == kPickNone) return;
    if (dropPicker == kPickPlayers || dropPicker == kPickMagicFind || dropPicker == kPickItemQuantity ||
        dropPicker == kPickFilter) {
        CloseDropPicker();
    } else {
        ComputeDropCandidates();
        if (!dropCandidates.empty()) ChooseDropCandidate(dropCandidates.front());
    }
    if (window != nullptr) InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::DropClick(int x, int y) {
    for (const DropHit& hit : dropHits) {
        // The list and the tables are backgrounds: the candidates and the rows inside them are the click targets.
        if (hit.kind == kHitList || hit.kind == kHitTableDrops || hit.kind == kHitTableWho) continue;
        if (!Inside(hit.rect, x, y)) continue;
        switch (hit.kind) {
            case kHitSwitch: {
                const SwitchButton& button = switches[static_cast<std::size_t>(hit.index)];
                if (button.current) return;
                if (button.available && !button.available()) return;
                BeginSwitch(button.onSwitch);
                return;
            }
            case kHitClose: Hide(); return;
            case kHitField: SetDropPicker(hit.index); break;
            case kHitStep: StepDrop(hit.index); break;
            case kHitCandidate: ChooseDropCandidate(hit.index); break;
            case kHitSortDrops:
                if (dropSortRows == hit.index) {
                    dropSortRowsDesc = !dropSortRowsDesc;
                } else {
                    dropSortRows = hit.index;
                    dropSortRowsDesc = hit.index != 0;  // names A to Z first, numbers largest first
                }
                dropScrollRows = 0;
                break;
            case kHitRowDrops: {
                // A drop row names an item by its key; gold and other non-item rows have no item to pick.
                if (hit.index < 0 || hit.index >= static_cast<int>(dropShownRows.size())) return;
                const std::string key = dropShownRows[static_cast<std::size_t>(hit.index)].key;
                for (const drop::ItemEntry& item : dropItems) {
                    if (item.key == key) {
                        dropItem = item;
                        dropHasItem = true;
                        dropDirty = true;
                        break;
                    }
                }
                break;
            }
            case kHitRowWho: {
                // A Dropped by row picks its monster, at the difficulty and type that row is for.
                if (hit.index < 0 || hit.index >= static_cast<int>(dropShownWho.size())) return;
                const drop::WhoRow& row = dropShownWho[static_cast<std::size_t>(hit.index)];
                dropMonster = row.monster;
                dropHasMonster = true;
                dropSetup.diff = row.diff;
                dropSetup.type = row.type;
                dropSetup.zone = row.monster.superunique ? -1 : row.zone;  // the website opens the monster in that zone
                dropDirty = true;
                break;
            }
            case kHitFilter:
                dropPicker = kPickFilter;
                dropFilterTarget = hit.index;
                dropText = dropFilters[static_cast<std::size_t>(hit.index)];
                if (IsWindow(search)) {
                    SetWindowTextW(search, dropText.c_str());
                    SendMessageW(search, EM_SETSEL, dropText.size(), dropText.size());  // the caret goes to the end
                    SetFocus(search);
                }
                break;
            case kHitSortWho:
                if (dropSortWho == hit.index) {
                    dropSortWhoDesc = !dropSortWhoDesc;
                } else {
                    dropSortWho = hit.index;
                    dropSortWhoDesc = hit.index != 0;
                }
                dropScrollWho = 0;
                break;
            default: break;
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (dropPicker != kPickNone) CloseDropPicker();  // a click outside the picker closes it
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::DropWheel(int x, int y, int steps) {
    for (const DropHit& hit : dropHits) {
        if (!Inside(hit.rect, x, y)) continue;
        if (hit.kind == kHitField && hit.index == dropPicker &&
            (dropPicker == kPickPlayers || dropPicker == kPickMagicFind || dropPicker == kPickItemQuantity)) {
            // The open Players, Magic Find or Item Quantity field: the wheel steps its number up or down.
            if (dropPicker == kPickPlayers) {
                dropSettings.players = std::clamp(dropSettings.players + steps, 1, 8);
                dropText = std::to_wstring(dropSettings.players);
            } else if (dropPicker == kPickMagicFind) {
                dropSettings.mf = std::clamp(dropSettings.mf + steps, -100, 400);
                dropText = std::to_wstring(dropSettings.mf);
            } else {
                dropSettings.iq = std::clamp(dropSettings.iq + steps, 0, 400);
                dropText = std::to_wstring(dropSettings.iq);
            }
            if (IsWindow(search)) {
                SetWindowTextW(search, dropText.c_str());
                SendMessageW(search, EM_SETSEL, dropText.size(), dropText.size());
            }
            dropDirty = true;
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
        if (hit.kind == kHitList) {
            dropScrollList = std::max(0, dropScrollList - steps);
        } else if (hit.kind == kHitTableDrops) {
            dropScrollRows = std::max(0, dropScrollRows - steps);
        } else if (hit.kind == kHitTableWho) {
            dropScrollWho = std::max(0, dropScrollWho - steps);
        } else {
            continue;
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
}

void OverlayState::DropPaint(HDC target) {
    RECT client{};
    GetClientRect(window, &client);
    const int clientWidth = static_cast<int>(client.right - client.left);
    const int clientHeight = static_cast<int>(client.bottom - client.top);
    const HDC dc = CreateCompatibleDC(target);
    const HBITMAP bitmap = CreateCompatibleBitmap(target, clientWidth, clientHeight);
    const HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    DropRecompute();
    ComputeDropCandidates();
    dropHits.clear();
    const auto& ui = DropStrings(dropData.get());

    const int margin = kFrameSlice + S(10);  // the wiki's margin, which clears the background picture's frame
    const int gap = S(8);
    const int rowH = S(30);
    const int right = clientWidth - margin;
    const int bottom = clientHeight - margin;

    // Header: the title, then the switch buttons, then the close button.
    const int headerTop = kFrameSlice + S(8);
    const int headerH = S(28);
    // The body starts under the header, with the picture's header line between them, as the wiki panel draws it.
    const int bodyTop = headerTop + headerH + gap * 2;
    layout.separatorY = headerTop + headerH + gap / 2;
    EnsureBackgroundCache(target, clientWidth, clientHeight);
    BitBlt(dc, 0, 0, clientWidth, clientHeight, backgroundDc, 0, 0, SRCCOPY);

    const RECT close = Rect(right - S(32), headerTop, right, headerTop + headerH);
    FillPanel(dc, close, kPanel, 200);
    FrameColor(dc, close, kBorder);
    Text(dc, L"X", close, kText, fontBold, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    dropHits.push_back({close, kHitClose, 0});
    int switchRight = close.left - gap;
    std::vector<RECT> switchRects(switches.size());
    for (std::size_t i = switches.size(); i-- > 0;) {
        switchRects[i] = Rect(switchRight - S(140), headerTop, switchRight, headerTop + headerH);
        switchRight = switchRects[i].left - gap;
    }
    for (std::size_t i = 0; i < switches.size(); ++i) {
        DrawSwitchButton(dc, switchRects[i], switches[i].label, switches[i].current);
        dropHits.push_back({switchRects[i], kHitSwitch, static_cast<int>(i)});
    }
    Text(dc, DropUi(ui, "dfTitle", "Drop Finder"), Rect(margin, headerTop, switchRight, headerTop + headerH), kHeaderGold,
         fontTitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // Setup column: one row per setting. The open picker's field is outlined, and the search edit sits over it.
    const int leftRight = margin + (right - margin) * 36 / 100;
    const int labelW = S(120);
    const int stepW = S(26);
    RECT openField{};
    bool haveOpenField = false;
    struct Row {
        const char* label;
        const char* fallback;
        int kind;   // a picker or number field (kPick*), or -1 for a steppable row
        int step;   // steppable rows: kStepDifficulty, kStepType or kStepTerror
    };
    const Row rows[] = {
        {"dfMonster", "Monster", kPickMonster, -1},
        {"dfZone", "Zone", kPickZone, -1},
        {"dfDifficulty", "Difficulty", -1, kStepDifficulty},
        {"dfMonsterType", "Monster type", -1, kStepType},
        {"dfTerror", "Terror Zone", -1, kStepTerror},
        {"dfPlayers", "Players", kPickPlayers, -1},
        {"dfMagicFind", "Magic Find (%)", kPickMagicFind, -1},
        {"dfItemQuantity", "Item Quantity (%)", kPickItemQuantity, -1},
        {"dfFindItem", "Find an item", kPickItem, -1},
    };
    static const char* const kDifficulty[] = {"dfNormal", "dfNightmare", "dfHell"};
    static const char* const kTypes[] = {"dfRegular", "dfChampion", "dfUnique", "dfMinion"};
    static const char* const kTerror[] = {"dfTerrorNone", "dfTerrorized", "dfHerald"};
    static const char* const kTypeFallback[] = {"Regular", "Champion", "Unique", "Minion"};
    static const char* const kTerrorFallback[] = {"None", "Terrorized", "Herald"};
    static const char* const kDifficultyFallback[] = {"Normal", "Nightmare", "Hell"};

    int y = bodyTop;
    for (std::size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); ++r) {
        const Row& row = rows[r];
        const RECT label = Rect(margin, y, margin + labelW, y + rowH);
        Text(dc, DropUi(ui, row.label, row.fallback), label, kText, fontBody, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        const int valueLeft = margin + labelW + gap;
        RECT box = Rect(valueLeft, y + S(2), leftRight, y + rowH - S(2));
        std::wstring value;
        bool active = false;
        if (row.step >= 0) {
            // A steppable row: < value >
            const RECT back = Rect(box.left, box.top, box.left + stepW, box.bottom);
            const RECT forward = Rect(box.right - stepW, box.top, box.right, box.bottom);
            FillPanel(dc, back, kPanel, 200);
            FrameColor(dc, back, kBorder);
            Text(dc, L"<", back, kText, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            FillPanel(dc, forward, kPanel, 200);
            FrameColor(dc, forward, kBorder);
            Text(dc, L">", forward, kText, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            dropHits.push_back({back, kHitStep, row.step * 2});
            dropHits.push_back({forward, kHitStep, row.step * 2 + 1});
            const RECT middle = Rect(back.right, box.top, forward.left, box.bottom);
            std::wstring text;
            if (row.step == kStepDifficulty) {
                text = DropUi(ui, kDifficulty[dropSetup.diff], kDifficultyFallback[dropSetup.diff]);
            } else if (row.step == kStepType) {
                text = DropUi(ui, kTypes[dropSetup.type], kTypeFallback[dropSetup.type]);
            } else {
                text = DropUi(ui, kTerror[dropSetup.terror], kTerrorFallback[dropSetup.terror]);
            }
            FillPanel(dc, middle, kSearchFill, 200);
            Text(dc, text, middle, kText, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        } else {
            FillPanel(dc, box, kSearchFill, 200);
            active = dropPicker == row.kind;
            if (row.kind == kPickMonster) {
                if (dropHasMonster) {
                    value = Wide(dropMonster.name);
                } else {
                    value = DropUi(ui, "dfMonsterPlaceholder", "Type a monster name");
                }
            } else if (row.kind == kPickZone) {
                const bool named = dropData && dropSetup.zone >= 0 &&
                                   static_cast<std::size_t>(dropSetup.zone) < dropData->names.zones.size();
                value = named ? Wide(dropData->names.zones[static_cast<std::size_t>(dropSetup.zone)])
                              : DropUi(ui, "dfNoZone", "No fixed zone");
            } else if (row.kind == kPickItem) {
                value = dropHasItem ? Wide(dropItem.name) : DropUi(ui, "dfItemPlaceholder", "Type an item name");
            } else if (row.kind == kPickPlayers) {
                value = std::to_wstring(dropSettings.players);
            } else if (row.kind == kPickMagicFind) {
                value = std::to_wstring(dropSettings.mf);
            } else {
                value = std::to_wstring(dropSettings.iq);
            }
            if (!active) {
                Text(dc, value, Rect(box.left + S(4), box.top, box.right - S(4), box.bottom), kText, fontBody,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            dropHits.push_back({box, kHitField, row.kind});
        }
        FrameColor(dc, box, active ? kGold : kBorder);
        if (active) {
            openField = box;
            haveOpenField = true;
        }
        y += rowH + S(4);
    }

    // The picker list under the setup rows (monster, zone and item pickers only).
    const bool listPicker = dropPicker == kPickMonster || dropPicker == kPickZone || dropPicker == kPickItem;
    const int listTop = y + gap;
    const int listBottom = bottom - S(40);
    if (listPicker && listBottom > listTop + S(40)) {
        const RECT list = Rect(margin, listTop, leftRight, listBottom);
        FillPanel(dc, list, kPanel, 200);
        FrameColor(dc, list, kBorder);
        dropHits.push_back({list, kHitList, 0});
        const int rowHeight = S(22);
        const int visible = std::max(1, static_cast<int>(list.bottom - list.top) / rowHeight);
        const int count = static_cast<int>(dropCandidates.size());
        dropScrollList = std::clamp(dropScrollList, 0, std::max(0, count - visible));
        if (count == 0) {
            Text(dc, DropUi(ui, "dfNoResults", "Nothing matches."), list, kDim, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        for (int k = 0; k < visible && dropScrollList + k < count; ++k) {
            const int value = dropCandidates[static_cast<std::size_t>(dropScrollList + k)];
            const RECT cell = Rect(list.left + 1, list.top + 1 + k * rowHeight, list.right - 1, list.top + (k + 1) * rowHeight);
            FillPanel(dc, cell, kTile, 150);
            // A zone row shows its act at the right, as the website does.
            const bool zoneRow = dropPicker == kPickZone && value > 0 && dropData;
            const int nameRight = zoneRow ? cell.right - S(64) : cell.right - S(6);
            Text(dc, DropCandidateName(value), Rect(cell.left + S(6), cell.top, nameRight, cell.bottom), kText,
                 fontBody, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (zoneRow) {
                const int act = dropData->data.zones[static_cast<std::size_t>(value - 1)].act;
                if (act >= 0 && static_cast<std::size_t>(act) < dropData->names.acts.size()) {
                    Text(dc, Wide(dropData->names.acts[static_cast<std::size_t>(act)]),
                         Rect(nameRight, cell.top, cell.right - S(6), cell.bottom), kDim, fontSmall,
                         DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                }
            }
            dropHits.push_back({cell, kHitCandidate, value});
        }
    }
    // Right column: "Drops" for the monster on top, "Dropped by" for the item below.
    const int tablesLeft = leftRight + gap * 2;
    const int tablesBottom = bottom - S(40);
    const int split = bodyTop + (tablesBottom - bodyTop) / 2;

    const auto drawTable = [&](const RECT& box, const std::wstring& title, const std::vector<DropColumn>& columns,
                               const std::vector<std::vector<std::wstring>>& cells, int& scroll, int hitKind,
                               const std::wstring& empty, int sortColumn, bool sortDesc, int sortKind, int filterBase) {
        FillPanel(dc, box, kPanel, 185);
        FrameColor(dc, box, kBorder);
        const int lineH = rowH;
        Text(dc, title, Rect(box.left + S(8), box.top, box.right - S(8), box.top + lineH), kHeaderGold, fontBold,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        const int inner = static_cast<int>(box.right - box.left) - S(16);
        int fixed = 0;
        for (const DropColumn& column : columns) fixed += column.width;
        std::vector<RECT> cellRects;
        int x = box.left + S(8);
        for (const DropColumn& column : columns) {
            const int cellWidth = column.width > 0 ? S(column.width) : std::max(S(60), inner - S(fixed));
            cellRects.push_back(Rect(x, box.top + lineH, x + cellWidth, box.top + 2 * lineH));
            x += cellWidth;
        }
        for (std::size_t c = 0; c < columns.size(); ++c) {
            const bool sorted = static_cast<int>(c) == sortColumn;
            const std::wstring label = columns[c].title + (sorted ? (sortDesc ? L"  v" : L"  ^") : L"");
            Text(dc, label, cellRects[c], sorted ? kHeaderGold : kDim, fontSmall,
                 (columns[c].right ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            dropHits.push_back({cellRects[c], sortKind, static_cast<int>(c)});  // a click sorts by this column
        }
        // The per-column search row under the header: a box per column, typed into with the search edit.
        for (std::size_t c = 0; c < columns.size(); ++c) {
            const RECT cell = cellRects[c];
            const RECT box2 = Rect(cell.left, cell.top + lineH, cell.right - S(6), cell.bottom + lineH);
            const int target = filterBase + static_cast<int>(c);
            FillPanel(dc, box2, kSearchFill, 200);
            if (dropPicker == kPickFilter && dropFilterTarget == target) {
                FrameColor(dc, box2, kGold);
                openField = box2;  // the search edit is moved over it
                haveOpenField = true;
            } else {
                FrameColor(dc, box2, kBorder);
                const std::wstring& text = dropFilters[static_cast<std::size_t>(target)];
                Text(dc, text.empty() ? DropUi(ui, "dfSearch", "Search...") : text,
                     Rect(box2.left + S(4), box2.top, box2.right - S(4), box2.bottom), text.empty() ? kDim : kText, fontSmall,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            dropHits.push_back({box2, kHitFilter, target});
        }
        const int rowsTop = box.top + 3 * lineH;
        const int visible = std::max(0, static_cast<int>(box.bottom - rowsTop) / lineH);
        const int total = static_cast<int>(cells.size());
        scroll = std::clamp(scroll, 0, std::max(0, total - visible));
        if (total == 0) {
            Text(dc, empty, Rect(box.left, rowsTop, box.right, box.bottom), kDim, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        for (int k = 0; k < visible && scroll + k < total; ++k) {
            const auto& row = cells[static_cast<std::size_t>(scroll + k)];
            const int top = rowsTop + k * lineH;
            const RECT band = Rect(box.left + 1, top, box.right - 1, top + lineH);
            FillPanel(dc, band, k % 2 == 0 ? kTile : kPanel, 150);
            // A click on a row picks what it names: an item in Drops, a monster in Dropped by.
            dropHits.push_back({band, hitKind == kHitTableDrops ? kHitRowDrops : kHitRowWho, scroll + k});
            int cx = box.left + S(8);
            for (std::size_t c = 0; c < columns.size() && c < row.size(); ++c) {
                const int cellWidth = columns[c].width > 0 ? S(columns[c].width) : std::max(S(60), inner - S(fixed));
                const RECT cell = Rect(cx, top, cx + cellWidth - S(6), top + lineH);
                Text(dc, row[c], cell, kText, fontBody,
                     (columns[c].right ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                cx += cellWidth;
            }
        }
        dropHits.push_back({box, hitKind, 0});
    };

    const std::vector<DropColumn> dropColumns = {{DropUi(ui, "dfColItem", "Item"), 0, false},
                                                 {DropUi(ui, "dfColChance", "Chance per kill"), 130, true},
                                                 {DropUi(ui, "dfColQuest", "Quest drop"), 110, true}};
    std::vector<std::vector<std::wstring>> dropCells;
    std::wstring dropTitle = DropUi(ui, "dfDropsTitle", "Drops: %s");
    dropTitle = dropHasMonster ? ReplaceFirst(dropTitle, L"%s", Wide(dropMonster.name)) : DropUi(ui, "dfEmpty", "Pick a monster or an item to start.");
    const std::wstring dropEmpty = dropHasMonster ? DropUi(ui, "dfNoTC", "This monster has no treasure class for this setup, so it drops nothing.")
                                                  : DropUi(ui, "dfEmpty", "Pick a monster or an item to start.");
    // The per-column searches narrow each table; the sort then orders what is left.
    std::vector<drop::DropRow> shownDrops;
    for (const drop::DropRow& row : dropRows) {
        const std::wstring quest = row.quest > 0 ? ChanceText(row.quest) : L"-";
        if (ContainsText(Wide(row.name), dropFilters[0]) && ContainsText(ChanceText(row.chance), dropFilters[1]) &&
            ContainsText(quest, dropFilters[2])) {
            shownDrops.push_back(row);
        }
    }
    static const char* const kWhoDifficulty[] = {"dfNormal", "dfNightmare", "dfHell"};
    static const char* const kWhoType[] = {"dfRegular", "dfChampion", "dfUnique", "dfMinion"};
    std::vector<drop::WhoRow> shownWho;
    for (const drop::WhoRow& row : dropWho) {
        const std::wstring quest = row.quest > 0 ? ChanceText(row.quest) : L"-";
        const std::wstring typeText = row.monster.superunique ? DropUi(ui, "dfSuperUnique", "Super Unique")
                                                              : DropUi(ui, kWhoType[row.type], kTypeFallback[row.type]);
        if (ContainsText(Wide(row.monster.name), dropFilters[8]) &&
            ContainsText(ZoneLabel(*dropData, row.zone), dropFilters[9]) &&
            ContainsText(DropUi(ui, kWhoDifficulty[row.diff], kDifficultyFallback[row.diff]), dropFilters[10]) &&
            ContainsText(typeText, dropFilters[11]) && ContainsText(ChanceText(row.chance), dropFilters[12]) &&
            ContainsText(quest, dropFilters[13])) {
            shownWho.push_back(row);
        }
    }
    std::stable_sort(shownDrops.begin(), shownDrops.end(), [&](const drop::DropRow& a, const drop::DropRow& b) {
        const int c = CompareDropRows(a, b, dropSortRows);
        if (c != 0) return dropSortRowsDesc ? c > 0 : c < 0;
        return a.name < b.name;
    });
    std::stable_sort(shownWho.begin(), shownWho.end(), [&](const drop::WhoRow& a, const drop::WhoRow& b) {
        const int c = CompareWhoRows(a, b, dropSortWho);
        if (c != 0) return dropSortWhoDesc ? c > 0 : c < 0;
        return a.monster.name < b.monster.name;
    });
    const std::wstring noResults = DropUi(ui, "dfNoResults", "Nothing matches.");
    dropShownRows = shownDrops;
    dropShownWho = shownWho;

    dropCells.clear();
    for (const drop::DropRow& row : shownDrops) {
        dropCells.push_back({Wide(row.name), ChanceText(row.chance), row.quest > 0 ? ChanceText(row.quest) : L"-"});
    }
    drawTable(Rect(tablesLeft, bodyTop, right, split - gap / 2), dropTitle, dropColumns, dropCells, dropScrollRows,
              kHitTableDrops, shownDrops.empty() && !dropRows.empty() ? noResults : dropEmpty, dropSortRows,
              dropSortRowsDesc, kHitSortDrops, 0);

    const std::vector<DropColumn> whoColumns = {{DropUi(ui, "dfColMonster", "Monster"), 170, false},
                                                {DropUi(ui, "dfColZone", "Zone"), 0, false},
                                                {DropUi(ui, "dfDifficulty", "Difficulty"), 100, false},
                                                {DropUi(ui, "dfMonsterType", "Monster type"), 110, false},
                                                {DropUi(ui, "dfColChance", "Chance per kill"), 130, true},
                                                {DropUi(ui, "dfColQuest", "Quest drop"), 110, true}};
    std::wstring whoTitle = DropUi(ui, "dfWhoTitle", "Dropped by: %s");
    whoTitle = dropHasItem ? ReplaceFirst(whoTitle, L"%s", Wide(dropItem.name)) : DropUi(ui, "dfEmpty", "Pick a monster or an item to start.");
    const std::wstring whoEmpty = dropHasItem ? DropUi(ui, "dfNoResults", "Nothing matches.")
                                              : DropUi(ui, "dfEmpty", "Pick a monster or an item to start.");
    std::vector<std::vector<std::wstring>> whoCells;
    for (const drop::WhoRow& row : shownWho) {
        const std::wstring zoneText =
            ZoneLabel(*dropData, row.zone) +
            (row.zoneCount > 1 ? L"  " + ReplaceFirst(DropUi(ui, "dfZoneCount", "%s zones"), L"%s", std::to_wstring(row.zoneCount))
                               : std::wstring());
        whoCells.push_back({Wide(row.monster.name), zoneText, DropUi(ui, kWhoDifficulty[row.diff], kDifficultyFallback[row.diff]),
                            row.monster.superunique ? DropUi(ui, "dfSuperUnique", "Super Unique")
                                                    : DropUi(ui, kWhoType[row.type], kTypeFallback[row.type]),
                            ChanceText(row.chance), row.quest > 0 ? ChanceText(row.quest) : L"-"});
    }
    drawTable(Rect(tablesLeft, split + gap / 2, right, tablesBottom), whoTitle, whoColumns, whoCells, dropScrollWho,
              kHitTableWho, shownWho.empty() && !dropWho.empty() ? noResults : whoEmpty, dropSortWho, dropSortWhoDesc,
              kHitSortWho, 8);

    // The note along the bottom.
    Text(dc, DropUi(ui, "dfNote", "Chances are per kill."), Rect(margin, bottom - S(30), right, bottom), kDim, fontSmall,
         DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    // The search edit is the text box of the open picker or number field.
    if (IsWindow(search)) {
        if (haveOpenField) {
            MoveWindow(search, openField.left + S(4), openField.top + S(2), openField.right - openField.left - S(8),
                       openField.bottom - openField.top - S(4), TRUE);
            ShowWindow(search, SW_SHOW);
        } else {
            ShowWindow(search, SW_HIDE);
        }
    }

    BitBlt(target, 0, 0, clientWidth, clientHeight, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}
