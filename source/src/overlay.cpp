// Win32 window for the ESR wiki overlay: tab row per wiki page, search box,
// paged list of entry titles, and a scrollable detail pane. Items are tiles
// (three per row, six per row on the compact rune page) under their section
// headers, with stats in the tooltip. Search narrows the items shown, matching
// their tooltip text too. While open, a full-screen blocker takes all mouse input
// and the panel keeps keyboard focus, so the game sees neither.
//
// Sizes are written at 96 DPI and scaled by DPI (S()) and by window size
// (uiScale for fonts). The window is sized from the game's client area whenever
// it is shown or the game window moves.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <windowsx.h>
#include <shellapi.h>  // ShellExecuteW: a web link in the wiki text opens in the browser
// GDI+ needs the COM types (IStream, PROPID) that objidl.h declares. Include it first.
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "overlay.hpp"
#include "debug_log.hpp"
#include "cursor_art.hpp"
#include "gamepad.hpp"
#include "pad_logic.hpp"
#include "search_index.hpp"
#include "skills_page.hpp"
#include "skills_tree.hpp"
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <map>

#include "wiki_model.hpp"
#include "campaign_labels.hpp"

namespace esrwiki {
static std::string NarrowForLog(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

namespace {
std::map<std::wstring, std::wstring> gUiStrings;  // English text -> the loader language's text
std::set<std::wstring> gUiValues;  // the translations themselves (text already in the language is not 'missing')
std::string gKeyboardLocale;  // the loader's language code; the on-screen keyboard's layout follows it
std::atomic<int> gPadHandoffKind{0};  // the controller kind (esrwiki::pad::Kind) the outgoing panel was showing help for
std::atomic<ULONGLONG> gPadHandoffAt{0};  // when a panel was switched with the controller (the next panel keeps the help up)
}

void SetKeyboardLocale(const std::string& code) { gKeyboardLocale = code; }

std::size_t LoadUiStrings(const std::wstring& path) {
    gUiStrings.clear();
    gUiValues.clear();
    if (path.empty()) return 0;
    std::ifstream file(path, std::ios::binary);
    if (!file) return 0;
    const auto toWide = [](const std::string& utf8) {
        if (utf8.empty()) return std::wstring();
        const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring out(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
        return out;
    };
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t tab = line.find('\t');
        if (tab == 0 || tab == std::string::npos) continue;
        gUiStrings[toWide(line.substr(0, tab))] = toWide(line.substr(tab + 1));
        gUiValues.insert(toWide(line.substr(tab + 1)));
    }
    return gUiStrings.size();
}

std::wstring UiText(const std::wstring& english) {
    const auto found = gUiStrings.find(english);
    if (found != gUiStrings.end()) return found->second;
    if (!gUiStrings.empty()) {  // a language is loaded: say which labels it has no text for
        bool plainEnglish = true;
        for (const wchar_t c : english) {
            if (c > 127) plainEnglish = false;
        }
        if (gUiValues.count(english) != 0) plainEnglish = false;  // already a translation (text built from localized data)
        if (plainEnglish) DebugLogOnce("ui-missing:" + NarrowForLog(english), "no translation for UI text: " + NarrowForLog(english));
    }
    return english;
}

// UiText for names that come from the page data (entry titles, field labels): the data is already localized where
// the website is, so a name with no entry here is normal and is not logged.
static std::wstring UiDataText(const std::wstring& text) {
    const auto found = gUiStrings.find(text);
    return found != gUiStrings.end() ? found->second : text;
}

std::wstring UiFormat(const std::wstring& english, const std::vector<std::wstring>& args) {
    std::wstring text = UiText(english);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring token = L"{" + std::to_wstring(i) + L"}";
        for (std::size_t at = text.find(token); at != std::wstring::npos; at = text.find(token, at + args[i].size())) {
            text.replace(at, token.size(), args[i]);
        }
    }
    return text;
}

// The counts the wiki data builders write under an entry heading ("215 items", "4448 affixes, 16 item types").
static std::wstring TranslateCounts(const std::wstring& text) {
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= L'0' && text[digits] <= L'9') ++digits;
    if (digits == 0) return UiText(text);
    const std::wstring first = text.substr(0, digits), rest = text.substr(digits);
    if (rest == L" items" || rest == L" item") return UiFormat(L"{0}" + rest, {first});
    const std::size_t comma = rest.find(L", ");
    if (rest.rfind(L" affixes", 0) == 0 && comma != std::wstring::npos) {
        std::size_t d2 = comma + 2, end = d2;
        while (end < rest.size() && rest[end] >= L'0' && rest[end] <= L'9') ++end;
        if (end > d2 && rest.compare(end, std::wstring::npos, L" item types") == 0)
            return UiFormat(L"{0} affixes, {1} item types", {first, rest.substr(d2, end - d2)});
    }
    return UiText(text);
}

namespace {

constexpr int kIdSearch = 1001;
constexpr UINT kMsgToggle = WM_APP + 1;
constexpr UINT kMsgShutdown = WM_APP + 2;
constexpr UINT kMsgOpen = WM_APP + 3;
constexpr UINT kMsgReload = WM_APP + 4;  // swap in a new database (see Overlay::ReplaceDatabase)
constexpr UINT_PTR kTimerFocus = 1;
constexpr UINT_PTR kTimerHandoff = 2;
constexpr UINT_PTR kTimerFlash = 4;         // ends the frame round the card a search jump arrived at
constexpr UINT_PTR kTimerPad = 3;           // polls the controller while the wiki is showing
constexpr UINT_PTR kTimerSuggest = 5;       // looks the search suggestions up once the typing stops
constexpr UINT_PTR kTimerNote = 6;          // ends a note under the search box ("Filters cleared")
constexpr UINT kPadInterval = 16;
constexpr UINT kMsgOpenOsk = WM_APP + 12;   // the search box was clicked while a controller is in use
constexpr UINT kMsgSpecData = WM_APP + 13;  // new Specializations data is waiting (see Overlay::SetSpecializationsData)
constexpr UINT kMsgCampaignData = WM_APP + 14;  // new Campaign Progression map data is waiting (Overlay::SetCampaignData)
constexpr UINT kMsgMapsData = WM_APP + 15;  // a new official Maps page is waiting (Overlay::SetMapsPageData)
constexpr UINT kMsgSkillsData = WM_APP + 16;  // new Skills page data is waiting (Overlay::SetSkillsData)
// The sources of the search index's extra records (search::Index::SetExtra): each is replaced on its own.
constexpr int kExtraPerks = 0;  // the Specializations board's perks (overlay_spec.inc)
constexpr int kExtraAreas = 1;  // the Campaign Progression map's areas (overlay_campaign.inc)
constexpr UINT kHandoffPollMs = 8;
constexpr ULONGLONG kHandoffTimeoutMs = 600;
// Foreground window to return to, handed from one overlay to the other when switching panels.
HWND gHandoffForeground = nullptr;
// While a panel switch is in progress the incoming panel's backdrop starts invisible and is raised
// only when the outgoing one goes, so the two never dim the game twice over.
std::atomic<bool> gHandoffActive{false};
// Set by the incoming panel once it is visible and painted; the outgoing panel hides only after that.
std::atomic<bool> gHandoffReady{false};
std::atomic<HWND> gHandoffBlocker{nullptr};
constexpr UINT kMsgCursorShow = WM_APP + 10;  // to the cursor thread
constexpr UINT kMsgCursorHide = WM_APP + 11;
constexpr UINT kTimerInterval = 150;
// The background picture has its own frame. These are the sizes of its parts, in the picture's own pixels:
// the frame on the left, right and bottom, and the header band on top, whose last row is a light line.
constexpr int kFrameSlice = 22;
constexpr int kHeaderSlice = 128;

constexpr BYTE kBlockerAlpha = 1;  // practically clear; keep above 0 so the layer still catches clicks meant for the game

void ReleaseHandoffBlocker() {
    gHandoffActive = false;
    const HWND next = gHandoffBlocker.exchange(nullptr);
    if (next != nullptr && IsWindow(next)) SetLayeredWindowAttributes(next, 0, kBlockerAlpha, LWA_ALPHA);
}

// Palette taken from the Eastern Sun Resurrected launcher: near-black warm brown panels, thin bronze borders,
// gold headings, cream text and a muted tan for labels.
constexpr COLORREF kBackground = RGB(12, 8, 5);
constexpr COLORREF kPanel = RGB(24, 17, 11);
constexpr COLORREF kTile = RGB(20, 14, 9);
constexpr COLORREF kSearchFill = RGB(38, 28, 18);
constexpr COLORREF kSelected = RGB(64, 46, 22);
constexpr COLORREF kBorder = RGB(122, 92, 48);
constexpr COLORREF kGold = RGB(224, 182, 96);  // headers and accents: launcher gold
constexpr COLORREF kHeaderGold = RGB(208, 166, 82);  // tab titles, category titles and section headings
constexpr COLORREF kText = RGB(244, 236, 220);
constexpr COLORREF kDim = RGB(168, 152, 124);
constexpr COLORREF kSnippetText = RGB(206, 192, 166);  // the card text under a search suggestion, and its messages
constexpr COLORREF kBody = RGB(228, 218, 198);

std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
}

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), length, nullptr,
                        nullptr);
    return out;
}

// D2 color codes mapped to the item-database palette (beige uniques, green set
// lines, blue magic, gray stats, red requirements).
COLORREF CodeColor(char code) {
    switch (code) {
    case '0': return RGB(235, 235, 235);  // white
    case '1': return RGB(255, 90, 90);    // red, requirements
    case '2': return RGB(90, 220, 90);    // green
    case '3': return RGB(110, 140, 255);  // blue, magic properties
    case '4': return RGB(201, 185, 150);  // beige, uniques and runewords
    case '5': return RGB(150, 150, 150);  // gray, base lines
    case '6': return RGB(130, 130, 130);  // black in D2, lifted for a dark theme
    case '7': return RGB(190, 170, 120);  // tan, rune names
    case '8': return RGB(255, 150, 60);   // orange
    case '9': return RGB(240, 240, 110);  // yellow
    case ';': return RGB(190, 120, 235);  // purple
    case ':': return RGB(110, 210, 110);  // dark green
    case 'C': return RGB(110, 230, 110);  // set items
    case 'H': return RGB(112, 104, 80);   // website headings (#706850)
    case 'R': return RGB(255, 0, 0);      // website red, ability names
    case 'P': return RGB(128, 0, 128);    // website purple, rewards
    case 'O': return RGB(255, 165, 0);    // website orange
    case 'G': return RGB(0, 128, 0);      // website green
    case 'D': return RGB(139, 0, 0);      // website dark red
    case 'B': return RGB(72, 80, 184);    // website blue (#4850B8)
    case 'K': return RGB(241, 229, 172);  // website beige (#F1E5AC)
    case 'L': return RGB(0, 0, 255);      // website blue (#0000FF)
    case 'Y': return RGB(128, 128, 128);  // website gray (#808080)
    case 'W': return RGB(255, 255, 255);  // website white
    case 'N': return kBody;               // back to the default body colour
    default: return kBody;
    }
}

struct PageView {
    std::size_t selected = 0;  // index into the filtered list
    std::size_t listPage = 0;  // page of the result list being shown
    int detailScroll = 0;
    std::size_t socketFilter = 0;  // runewords: 0 shows all, otherwise only this many runes
    std::string tierFilter;    // uniques: "" shows all tiers, otherwise Normal, Exceptional or Elite
    std::string familyFilter;  // uniques: "" shows all weapon types, otherwise one family such as "Axes"
    BlockSort sort = BlockSort::Default;
};

// A filter or sort button above the detail pane.
enum class ChipKind { Socket, Sort, Tier, Family };
struct Chip {
    RECT rect{};
    ChipKind kind = ChipKind::Socket;
    std::size_t value = 0;  // socket count or BlockSort
    std::string text;       // tier or family name for those filters
};

struct Layout {
    std::vector<RECT> tabs;  // one per page, in database order
    RECT title{};      // the window title, in the header band
    int separatorY = 0;  // where the background picture's header line is drawn
    RECT close{};
    RECT search{};
    RECT clearQuery{};  // the X beside the search box; clears the query
    RECT matchPrev{};   // steps the selection back to the previous match
    RECT matchNext{};   // steps the selection forward to the next match
    RECT count{};
    RECT prev{};
    RECT next{};
    RECT pageLabel{};
    RECT detail{};
    std::vector<RECT> switchButtons;  // one per switch button, the first leftmost
    RECT scrollTrack{};
    RECT scrollThumb{};
    bool scrollable = false;
    std::vector<Chip> chips;
};

// Tile geometry for one detail pane width.
struct TileStyle {
    int columns = 3;
    int gap = 12;
    int tileWidth = 100;
    int artBox = 60;
    int cell = 30;      // width of one rune icon slot
    int padding = 6;
    HFONT nameFont = nullptr;
};

bool Inside(const RECT& rect, int x, int y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

RECT Rect(int left, int top, int right, int bottom) {
    RECT rect{left, top, right, bottom};
    return rect;
}

// The frame drawn for the search box: from the left edge to the clear button.
RECT SearchBoxRect(const Layout& layout) {
    return Rect(layout.search.left, layout.search.top, layout.clearQuery.left, layout.search.bottom);
}

// The edit control sits inside that frame, one text line tall and centred vertically, because a
// single-line edit control always draws its text and caret at the top of its client area.
RECT SearchEditRect(const Layout& layout, int lineHeight, int pad) {
    const RECT box = SearchBoxRect(layout);
    const int top = box.top + (box.bottom - box.top - lineHeight) / 2;
    return Rect(box.left + 1 + pad, top, box.right - 1 - pad, top + lineHeight);
}

int Bounded(int value, int low, int high) { return value < low ? low : (value > high ? high : value); }

// Translucent fill so the background art shows through the panels.
void FillPanel(HDC dc, const RECT& rect, COLORREF color, BYTE alpha) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    const HDC source = CreateCompatibleDC(dc);
    const HBITMAP pixel = CreateCompatibleBitmap(dc, 1, 1);
    const HGDIOBJ previous = SelectObject(source, pixel);
    SetPixel(source, 0, 0, color);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
    AlphaBlend(dc, rect.left, rect.top, width, height, source, 0, 0, 1, 1, blend);
    SelectObject(source, previous);
    DeleteObject(pixel);
    DeleteDC(source);
}

void FillColor(HDC dc, const RECT& rect, COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void FrameColor(HDC dc, const RECT& rect, COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

// Kanji rune names are coloured by their place in the Kanji Runes list. I through Yo keep the
// default colour. Ki through Chi red, Sa through O yellow, Ho through Ha orange, Na through Mo green,
// No through Re gold, Su through Wo and Null purple. A name counts only as a whole word: "Ki Rune".
struct RuneNameGroup { const wchar_t* name; char group; };
// Off while the LoD Runes list is drawn: its "Ko Rune" is a different rune from the Kanji one.
bool gKanjiRuneColors = true;
// Names of the LoD runes on the card whose tooltip is being drawn ("Ko"). They are coloured orange
// wherever they appear as "<name> Rune", and take priority over a Kanji rune with the same name.
std::vector<std::wstring> gLodRuneNames;
const RuneNameGroup kRuneNameGroups[] = {
    {L"Ki", 'r'},
    {L"Ri", 'r'},
    {L"Mi", 'r'},
    {L"Ya", 'r'},
    {L"A", 'r'},
    {L"Tsu", 'r'},
    {L"Chi", 'r'},
    {L"Sa", 'y'},
    {L"Yu", 'y'},
    {L"Ke", 'y'},
    {L"E", 'y'},
    {L"Ko", 'y'},
    {L"Ra", 'y'},
    {L"O", 'y'},
    {L"Ho", 'o'},
    {L"Me", 'o'},
    {L"Ru", 'o'},
    {L"Ta", 'o'},
    {L"To", 'o'},
    {L"Wa", 'o'},
    {L"Ha", 'o'},
    {L"Na", 'g'},
    {L"Ni", 'g'},
    {L"Se", 'g'},
    {L"Fu", 'g'},
    {L"Ma", 'g'},
    {L"Hi", 'g'},
    {L"Mo", 'g'},
    {L"No", 'd'},
    {L"Te", 'd'},
    {L"Ro", 'd'},
    {L"So", 'd'},
    {L"Mu", 'd'},
    {L"Ne", 'd'},
    {L"Re", 'd'},
    {L"Su", 'p'},
    {L"He", 'p'},
    {L"Nu", 'p'},
    {L"Wo", 'p'},
    {L"Null", 'p'},
};

COLORREF RuneGroupColor(char group) {
    switch (group) {
    case 'r': return CodeColor('1');                // red
    case 'y': return CodeColor('9');                // yellow
    case 'o': return CodeColor('8');                // orange
    case 'g': return CodeColor('2');                // green
    case 'd': return RGB(230, 190, 60);             // gold
    case 'p': return CodeColor(';');                // purple
    default: return kBody;
    }
}

// Colour of a bare rune label such as "Ki" (the name without " Rune"), or the fallback.
COLORREF KanjiLabelColor(const std::wstring& label, COLORREF fallback) {
    if (!gKanjiRuneColors) return fallback;
    for (const RuneNameGroup& rune : kRuneNameGroups) {
        if (label == rune.name) return RuneGroupColor(rune.group);
    }
    return fallback;
}

// One piece of a line: plain text in the line's colour, or a rune name in its own colour.
struct RunePiece { std::wstring text; COLORREF color; bool named; bool colored = false; bool highlight = false; bool continues = false; };

// The search terms being highlighted (the query's required words and "phrases", never its -excluded ones); empty when
// nothing is searched. `key` is the query as typed, for the measure cache.
struct SearchHighlight {
    esrwiki::text::ParsedQuery terms;
    std::wstring key;
    bool empty() const { return key.empty(); }
    void clear() {
        terms = esrwiki::text::ParsedQuery{};
        key.clear();
    }
};
SearchHighlight gHighlight;
constexpr COLORREF kHighlightFill = RGB(104, 74, 20);

void SetHighlight(const std::string& query) {
    gHighlight.clear();
    esrwiki::text::ParsedQuery parsed = esrwiki::search::Parse(query);
    if (!parsed.HasRequired()) return;
    parsed.terms.erase(std::remove_if(parsed.terms.begin(), parsed.terms.end(),
                                      [](const esrwiki::text::QueryTerm& term) { return term.excluded; }),
                       parsed.terms.end());
    gHighlight.terms = std::move(parsed);
    gHighlight.key = Wide(query);
}

// Where the highlighted terms are in `line`, as [start, end) UTF-16 positions, sorted and not overlapping. The line is
// matched as the filter matches it (text::SearchText: folded, separators as spaces), one code point for one, so the
// positions found map straight back onto the line.
std::vector<std::pair<std::size_t, std::size_t>> HighlightRanges(const std::wstring& line) {
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    if (gHighlight.empty() || line.empty()) return ranges;
    std::u32string points;
    std::vector<std::size_t> unitOf;  // the UTF-16 position of each code point, then line.size()
    points.reserve(line.size());
    unitOf.reserve(line.size() + 1);
    for (std::size_t i = 0; i < line.size();) {
        unitOf.push_back(i);
        const unsigned int unit = static_cast<unsigned int>(line[i]);
        const unsigned int low = i + 1 < line.size() ? static_cast<unsigned int>(line[i + 1]) : 0u;
        if (unit >= 0xD800u && unit <= 0xDBFFu && low >= 0xDC00u && low <= 0xDFFFu) {
            points.push_back(static_cast<char32_t>(0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u)));
            i += 2;
        } else {
            points.push_back(static_cast<char32_t>(unit));
            ++i;
        }
    }
    unitOf.push_back(line.size());
    const std::u32string searchable = esrwiki::text::SearchText(points);
    for (const esrwiki::text::QueryTerm& term : gHighlight.terms.terms) {
        std::size_t from = 0;
        while (from < searchable.size()) {
            std::size_t length = 0;
            const std::size_t at = esrwiki::text::FindTerm(searchable, term, from, &length);
            if (at == std::u32string::npos || at >= points.size() || length == 0) break;
            const std::size_t end = std::min(at + length, points.size());
            ranges.emplace_back(unitOf[at], unitOf[end]);
            from = end;
        }
    }
    if (ranges.size() > 1) {
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<std::size_t, std::size_t>> merged;
        for (const auto& range : ranges) {
            if (!merged.empty() && range.first <= merged.back().second) merged.back().second = std::max(merged.back().second, range.second);
            else merged.push_back(range);
        }
        ranges.swap(merged);
    }
    return ranges;
}

bool HasHighlight(const std::wstring& line) {
    return !gHighlight.empty() && !HighlightRanges(line).empty();
}

// Splits pieces at each search match so the matched part is drawn on a highlight. Rune-name
// pieces are kept whole.
std::vector<RunePiece> HighlightPieces(const std::vector<RunePiece>& input) {
    if (gHighlight.empty()) return input;
    std::vector<RunePiece> out;
    for (const RunePiece& piece : input) {
        if (piece.named) {
            out.push_back(piece);
            continue;
        }
        // A part that starts right after a letter continues that word, so it cannot start a new line.
        auto add = [&](std::size_t start, std::size_t length, bool mark) {
            const bool continues = start == 0 ? piece.continues : piece.text[start - 1] != L' ';
            out.push_back({piece.text.substr(start, length), piece.color, false, piece.colored, mark, continues});
        };
        std::size_t pos = 0;
        for (const auto& range : HighlightRanges(piece.text)) {
            if (range.first > pos) add(pos, range.first - pos, false);
            add(range.first, range.second - range.first, true);
            pos = range.second;
        }
        if (pos < piece.text.size()) add(pos, piece.text.size() - pos, false);
    }
    return out;
}

bool IsWordChar(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

// True when the rune name starts at text[i] and ends on a word boundary. Compares in place, so
// scanning allocates nothing.
bool RuneNameAt(const std::wstring& text, std::size_t i, const wchar_t* name, std::size_t& after) {
    std::size_t k = i;
    for (const wchar_t* c = name; *c != L'\0'; ++c, ++k) {
        if (k >= text.size() || text[k] != *c) return false;
    }
    if (text.compare(k, 5, L" Rune") != 0) return false;
    k += 5;
    if (k < text.size() && IsWordChar(text[k])) return false;
    after = k;
    return true;
}

// Splits text into pieces, each kanji rune name ("Ki Rune") as its own named piece.
// Returns false when the text has no rune name, so callers can use the plain DrawText path.
bool FindRuneNames(const std::wstring& text, std::vector<RunePiece>& pieces) {
    pieces.clear();
    if (!gKanjiRuneColors) return false;
    if (text.find(L" Rune") == std::wstring::npos) return false;  // the common case: nothing to colour
    bool any = false;
    std::wstring plain;
    auto flush = [&]() {
        if (!plain.empty()) pieces.push_back({plain, kBody, false});
        plain.clear();
    };
    std::size_t i = 0;
    while (i < text.size()) {
        bool matched = false;
        if (i == 0 || !IsWordChar(text[i - 1])) {
            for (const std::wstring& lodName : gLodRuneNames) {
                std::size_t after = 0;
                if (RuneNameAt(text, i, lodName.c_str(), after)) {
                    flush();
                    pieces.push_back({text.substr(i, after - i), CodeColor('8'), true});
                    i = after;
                    matched = any = true;
                    break;
                }
            }
        }
        if (!matched && (i == 0 || !IsWordChar(text[i - 1]))) {
            for (const RuneNameGroup& rune : kRuneNameGroups) {
                std::size_t after = 0;
                if (RuneNameAt(text, i, rune.name, after)) {
                    flush();
                    pieces.push_back({text.substr(i, after - i), RuneGroupColor(rune.group), true});
                    i = after;
                    matched = any = true;
                    break;
                }
            }
        }
        if (!matched) {
            plain += text[i];
            ++i;
        }
    }
    flush();
    return any;
}

// Lays the pieces out wrapped to the rectangle's width, as DrawText does for words, and draws them
// when `draw` is set. Returns the height used.
int LayoutPieces(HDC dc, const std::vector<RunePiece>& source, COLORREF base, RECT rect, HFONT font,
                 UINT format, bool draw) {
    const std::vector<RunePiece> pieces = HighlightPieces(source);
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    const int lineHeight = std::max(1, static_cast<int>(metrics.tmHeight));
    const int maxWidth = std::max(1, static_cast<int>(rect.right - rect.left));

    struct Placed { int x; int line; int width; std::wstring text; COLORREF color; bool highlight = false; };
    std::vector<Placed> placed;
    std::vector<int> lineWidths;
    int x = 0;
    int line = 0;
    bool wordStart = true;
    std::vector<Placed> word;  // the current word; pieces that continue a word join it
    bool wordIsSpace = false;
    // Places the buffered word, moving the whole word to a new line first when it does not fit.
    auto flush = [&]() {
        if (word.empty()) return;
        int total = 0;
        for (const Placed& p : word) total += p.width;
        if (!wordIsSpace && x > 0 && x + total > maxWidth) {
            lineWidths.push_back(x);
            x = 0;
            ++line;
        }
        for (Placed& p : word) {
            p.x = x;
            p.line = line;
            x += p.width;
            placed.push_back(p);
        }
        word.clear();
    };
    for (const RunePiece& piece : pieces) {
        wordStart = !piece.continues;  // a continuation joins the word before it
        const COLORREF color = (piece.named || piece.colored) ? piece.color : base;
        std::size_t start = 0;
        while (start < piece.text.size()) {
            std::size_t end = start + 1;
            if (!piece.named) {
                if (piece.text[start] == L'\n') {
                    end = start + 1;
                } else {
                    const bool space = piece.text[start] == L' ';
                    while (end < piece.text.size() && piece.text[end] != L'\n' && (piece.text[end] == L' ') == space) ++end;
                }
            } else {
                end = piece.text.size();
            }
            const std::wstring token = piece.text.substr(start, end - start);
            start = end;
            if (token == L"\n") {
                flush();
                lineWidths.push_back(x);
                x = 0;
                ++line;
                wordStart = true;
                continue;
            }
            const bool isSpace = !piece.named && token[0] == L' ';
            SIZE size = {};
            GetTextExtentPoint32W(dc, token.c_str(), static_cast<int>(token.size()), &size);
            if (isSpace) {
                flush();
                if (x == 0) {  // no leading spaces on a wrapped line
                    wordStart = true;
                    continue;
                }
                wordIsSpace = true;
                word.push_back({0, line, static_cast<int>(size.cx), token, color, piece.highlight});
                flush();
            } else {
                if (wordStart) {
                    flush();
                    wordIsSpace = false;
                }
                word.push_back({0, line, static_cast<int>(size.cx), token, color, piece.highlight});
            }
            wordStart = isSpace;
        }
    }
    flush();
    lineWidths.push_back(x);
    const int height = placed.empty() ? 0 : (line + 1) * lineHeight;

    HBRUSH highlightBrush = CreateSolidBrush(kHighlightFill);
    if (draw && !placed.empty()) {
        int top = rect.top;
        if (format & DT_VCENTER) top += (static_cast<int>(rect.bottom - rect.top) - height) / 2;
        SetBkMode(dc, TRANSPARENT);
        for (const Placed& item : placed) {
            int offset = 0;
            const int used = lineWidths[static_cast<std::size_t>(item.line)];
            if (format & DT_CENTER) offset = (maxWidth - used) / 2;
            else if (format & DT_RIGHT) offset = maxWidth - used;
            if (item.highlight) {
                const int boxLeft = rect.left + offset + item.x;
                const int boxTop = top + item.line * lineHeight;
                RECT box = {boxLeft, boxTop, boxLeft + item.width, boxTop + lineHeight};
                FillRect(dc, &box, highlightBrush);
            }
            SetTextColor(dc, item.color);
            TextOutW(dc, rect.left + offset + item.x, top + item.line * lineHeight, item.text.c_str(),
                     static_cast<int>(item.text.size()));
        }
    }
    DeleteObject(highlightBrush);
    SelectObject(dc, previous);
    return height;
}

// True when the text has colour codes, so each run needs its own colour.
bool HasColorCodes(const std::string& text) {
    return text.find("\xC3\xBF" "c") != std::string::npos;
}

// The text as drawable pieces: each coded run takes its own colour; uncoded runs keep the base
// colour, with kanji rune names still coloured.
std::vector<RunePiece> LinePieces(const std::string& text, COLORREF base) {
    std::vector<RunePiece> pieces;
    for (const ColorRun& run : SplitColorCodes(text)) {
        if (run.text.empty()) continue;
        const std::wstring wide = Wide(run.text);
        if (run.code != '\0') {
            pieces.push_back({wide, CodeColor(run.code), false, true});
            continue;
        }
        std::vector<RunePiece> named;
        if (FindRuneNames(wide, named)) pieces.insert(pieces.end(), named.begin(), named.end());
        else pieces.push_back({wide, base, false, false});
    }
    return pieces;
}

void Text(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font, UINT format) {
    std::vector<RunePiece> pieces;
    const bool runes = FindRuneNames(text, pieces);
    if (runes || HasHighlight(text)) {
        if (!runes) {
            // FindRuneNames may leave its own plain pieces behind when it finds no rune names.
            pieces.clear();
            pieces.push_back({text, color, false, false, false});
        }
        LayoutPieces(dc, pieces, color, rect, font, format, true);
        return;
    }
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_NOPREFIX);
    SelectObject(dc, previous);
}

// Heights already measured, keyed by text, width and font. Measuring runs GDI and the same text
// is measured on every repaint, so remembering the results keeps scrolling light. Cleared when
// the fonts change.
struct MeasureKey {
    std::size_t textHash;
    int width;
    HFONT font;
    bool operator==(const MeasureKey& other) const {
        return textHash == other.textHash && width == other.width && font == other.font;
    }
};
struct MeasureKeyHash {
    std::size_t operator()(const MeasureKey& key) const {
        return key.textHash ^ (static_cast<std::size_t>(key.width) * 1000003u) ^ std::hash<HFONT>()(key.font);
    }
};
std::unordered_map<MeasureKey, int, MeasureKeyHash> gMeasureCache;

// Height that `text` needs when wrapped to `width` in `font`.
int MeasureHeightUncached(HDC dc, const std::wstring& text, int width, HFONT font) {
    if (text.empty()) return 0;
    std::vector<RunePiece> pieces;
    const bool runes = FindRuneNames(text, pieces);
    // Text with the search term in it is drawn piece by piece (Text), which wraps a little differently from DrawText, so
    // it is measured the same way; otherwise a line the drawing wraps would not have the room it needs.
    const bool marked = HasHighlight(text);
    if (runes || marked) {
        if (!runes) {
            pieces.clear();
            pieces.push_back({text, kBody, false, false, false});
        }
        return LayoutPieces(dc, pieces, kBody, Rect(0, 0, width, 0), font, DT_LEFT, false);
    }
    RECT measured = Rect(0, 0, width, 0);
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, previous);
    return measured.bottom - measured.top;
}

// Width of `text` on one line in `font` (no wrapping), used to fit a tooltip to its longest line.
int NaturalWidth(HDC dc, const std::wstring& text, HFONT font) {
    if (text.empty()) return 0;
    RECT measured = Rect(0, 0, 0, 0);
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured,
              DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, previous);
    return measured.right - measured.left;
}

// Height that `text` needs when wrapped to `width` in `font`, remembered for repeat measures.
int MeasureHeight(HDC dc, const std::wstring& text, int width, HFONT font) {
    if (text.empty()) return 0;
    // The search term is part of the key: the same text measures differently with a match in it.
    const MeasureKey key{std::hash<std::wstring>{}(text) ^ (std::hash<std::wstring>{}(gHighlight.key) * 0x9E3779B1u), width, font};
    const auto cached = gMeasureCache.find(key);
    if (cached != gMeasureCache.end()) return cached->second;
    const int height = MeasureHeightUncached(dc, text, width, font);
    if (gMeasureCache.size() > 50000) gMeasureCache.clear();
    gMeasureCache.emplace(key, height);
    return height;
}

// Width of `text` on one line in `font`.
// Drop shadows: a dark band under a panel (fading out), and a dark offset copy under header text.
void DropShadow(HDC dc, const RECT& rect, int px) {
    static const BYTE alphas[] = {150, 100, 60, 30};
    for (int k = 0; k < 4; ++k) {
        const RECT strip = Rect(rect.left + 2 * px, rect.bottom + k * px, rect.right - 2 * px, rect.bottom + (k + 1) * px);
        FillPanel(dc, strip, RGB(0, 0, 0), alphas[k]);
    }
}

void ShadowText(HDC dc, const std::wstring& text, RECT rect, HFONT font, UINT format, int px) {
    Text(dc, text, Rect(rect.left + px, rect.top + px, rect.right + px, rect.bottom + px), RGB(0, 0, 0), font, format);
}

// A shadow cast toward the bottom right: L-shaped bands just outside the rect, so the panel itself stays clear.
void CornerShadow(HDC dc, const RECT& rect, int px) {
    static const BYTE alphas[] = {150, 100, 60, 30};
    for (int k = 0; k < 4; ++k) {
        const int right = rect.right + k * px;   // this band's inner edge
        const int bottom = rect.bottom + k * px;
        const RECT side = Rect(right, rect.top + (k + 1) * px, right + px, bottom + px);
        const RECT foot = Rect(rect.left + (k + 1) * px, bottom, right + px, bottom + px);
        FillPanel(dc, side, RGB(0, 0, 0), alphas[k]);
        FillPanel(dc, foot, RGB(0, 0, 0), alphas[k]);
    }
}

int MeasureWidth(HDC dc, const std::wstring& text, HFONT font) {
    if (text.empty()) return 0;
    RECT measured = Rect(0, 0, 0, 0);
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured,
              DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, previous);
    return measured.right - measured.left;
}

// Height of one line of text in `font`.
int LineHeight(HDC dc, HFONT font) {
    TEXTMETRICW metrics{};
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous);
    return metrics.tmHeight;
}

// Largest visible top-level window of this process, which is the game window.
struct GameCandidate { HWND excluded = nullptr; HWND best = nullptr; long long area = 0; };

BOOL CALLBACK FindGameCallback(HWND window, LPARAM value) {
    auto& candidate = *reinterpret_cast<GameCandidate*>(value);
    if (window == candidate.excluded || !IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window) return TRUE;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId()) return TRUE;
    RECT client{};
    if (!GetClientRect(window, &client)) return TRUE;
    const long long area = static_cast<long long>(client.right - client.left) * (client.bottom - client.top);
    if (area > candidate.area) { candidate.area = area; candidate.best = window; }
    return TRUE;
}

HWND FindGameWindow(HWND overlay) {
    GameCandidate candidate{overlay};
    EnumWindows(FindGameCallback, reinterpret_cast<LPARAM>(&candidate));
    return candidate.best;
}

// Screen rectangle of the game's client area.
RECT GameScreenRect(HWND game) {
    RECT client{};
    GetClientRect(game, &client);
    POINT origin{0, 0};
    ClientToScreen(game, &origin);
    return Rect(origin.x, origin.y, origin.x + client.right, origin.y + client.bottom);
}

// Plain text of a color-coded string, with the color of its first code.
void PlainText(const std::string& text, std::string& plain, COLORREF& color, COLORREF fallback) {
    plain.clear();
    color = fallback;
    bool colored = false;
    for (const ColorRun& run : SplitColorCodes(text)) {
        if (!colored && run.code != '\0') {
            color = CodeColor(run.code);
            colored = true;
        }
        plain += run.text;
    }
}

// A hover target in the detail pane: a rune icon (rune >= 0) or a whole item tile (rune == -1).
struct DetailHit { RECT rect; const Entry* entry; const Block* block; int rune; };
// A whole item tile, for moving the controller's highlight from card to card (visible tiles only).
struct CardHit { RECT rect; const Entry* entry; const Block* block; };

// One line of the controller help: the button's name, what it does, and up to two of the game's own sprites for it.
struct HelpRow { const wchar_t* button; const wchar_t* action; const char* icon1; const char* icon2; };
// The help window's rows and the sizes they are drawn at; shared by sizing the window and drawing into it.
struct HelpMetrics {
    std::vector<HelpRow> rows;
    std::wstring heading;
    float size = 0, line = 0, iconHeight = 0, gap = 0, iconColumn = 0, textX = 0, inset = 0;
    int width = 0, height = 0;
    bool picture = false;  // a picture file stands in for the list
};

// What the pointer is over; only item blocks and their runes ever get a hover.
struct Hover { const Entry* entry = nullptr; const Block* block = nullptr; int rune = -1; POINT at{0, 0}; RECT target{0, 0, 0, 0}; };

// Clicks on the blocker must not activate it (that would move keyboard focus off
// the panel), and closing it is ignored so Alt+F4 cannot remove the input block.
// The game keeps hiding and showing the system cursor, which flickers. Over the wiki the system
// cursor is hidden instead, and a drawn copy of the game's cursor (CursorProc) follows the pointer.
// True while a window of this process (the game or the wiki) is in front. A screenshot tool such as
// Win+Shift+S puts its own window in front; then the system cursor must show and the drawn one must go.
bool OwnWindowInFront() {
    const HWND front = GetForegroundWindow();
    if (front == nullptr) return true;
    DWORD process = 0;
    GetWindowThreadProcessId(front, &process);
    return process == GetCurrentProcessId();
}

// The game's own cursor, if the game has a system cursor to copy (null when it draws its own). It is
// remembered while the wiki is closed; its picture is drawn by the cursor window (RenderCursorImage).
// Showing the game's cursor handle itself does not work: the game keeps hiding it, so it flickers and
// vanishes while the pointer is still. Set to false to always draw the built-in gauntlet instead.
constexpr bool kUseGameCursor = true;
std::atomic<HCURSOR> gGameCursor{nullptr};
std::atomic<int> gCursorHotX{0};  // hot spot of the cursor image now drawn, in pixels
std::atomic<int> gCursorHotY{0};
std::atomic<int> gCursorDrawWidth{0};   // size of the cursor image now drawn, so tooltips can keep clear of it
std::atomic<int> gCursorDrawHeight{0};
// The screen area the wiki covers (the game window), for the drawn cursor: outside it the pointer belongs to other
// programs and shows their own cursor. Empty (all zero) means no limit.
std::atomic<int> gCoverLeft{0}, gCoverTop{0}, gCoverRight{0}, gCoverBottom{0};
bool PointInCover(const POINT& p) {
    const int r = gCoverRight.load(), b = gCoverBottom.load();
    if (r <= gCoverLeft.load() || b <= gCoverTop.load()) return true;
    return p.x >= gCoverLeft.load() && p.x < r && p.y >= gCoverTop.load() && p.y < b;
}

// Puts the input blocker exactly over the game window (its client area), so every click outside the game goes to
// the program under it as usual. Without a game window it covers all monitors, as before.
void PlaceBlocker(HWND blocker, HWND game) {
    if (!IsWindow(game)) {
        gCoverLeft = gCoverTop = gCoverRight = gCoverBottom = 0;
        if (IsWindow(blocker))
            SetWindowPos(blocker, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                         GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    const RECT cover = GameScreenRect(game);
    gCoverLeft = cover.left; gCoverTop = cover.top; gCoverRight = cover.right; gCoverBottom = cover.bottom;
    if (IsWindow(blocker))
        SetWindowPos(blocker, nullptr, cover.left, cover.top, cover.right - cover.left, cover.bottom - cover.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
}

// Over the wiki the system cursor is hidden (the drawn one replaces it); with another program in front
// the normal arrow shows.
void HideSystemCursor() {
    SetCursor(OwnWindowInFront() ? nullptr : LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));  // IDC_ARROW
}

// While the wiki is closed: remembers the last cursor the game showed over its own window.
void RememberGameCursor() {
    if (!kUseGameCursor || !OwnWindowInFront()) return;
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING) || info.hCursor == nullptr) return;
    const HWND under = WindowFromPoint(info.ptScreenPos);
    DWORD process = 0;
    if (under != nullptr) GetWindowThreadProcessId(under, &process);
    if (process != GetCurrentProcessId()) return;  // the pointer is not over the game
    if (info.hCursor == LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))) return;  // the plain Windows arrow is not the game's
    gGameCursor = info.hCursor;
}

// The picture of a system cursor as premultiplied 0xAARRGGBB pixels, with its hot spot. False when it
// cannot be read (for example a black and white cursor).
bool CursorPixels(HCURSOR cursor, std::vector<std::uint32_t>& pixels, int& width, int& height, int& hotX, int& hotY) {
    ICONINFO icon{};
    if (!GetIconInfo(cursor, &icon)) return false;
    bool ok = false;
    BITMAP color{};
    if (icon.hbmColor != nullptr && GetObjectW(icon.hbmColor, sizeof(color), &color) != 0 && color.bmWidth > 0 &&
        color.bmHeight > 0 && color.bmWidth <= 512 && color.bmHeight <= 512) {
        width = color.bmWidth;
        height = color.bmHeight;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // top row first
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        const HDC screen = GetDC(nullptr);
        pixels.assign(static_cast<std::size_t>(width) * height, 0);
        if (GetDIBits(screen, icon.hbmColor, 0, height, pixels.data(), &info, DIB_RGB_COLORS) == height) {
            bool hasAlpha = false;
            for (const std::uint32_t pixel : pixels) hasAlpha = hasAlpha || (pixel >> 24) != 0;
            if (hasAlpha) {
                for (std::uint32_t& pixel : pixels) {  // colours were not multiplied by alpha
                    const std::uint32_t a = pixel >> 24;
                    pixel = (a << 24) | ((((pixel >> 16) & 255) * a / 255) << 16) | ((((pixel >> 8) & 255) * a / 255) << 8) |
                            ((pixel & 255) * a / 255);
                }
                ok = true;
            } else if (icon.hbmMask != nullptr) {  // no alpha: the mask says which pixels are drawn
                std::vector<std::uint32_t> mask(pixels.size(), 0);
                if (GetDIBits(screen, icon.hbmMask, 0, height, mask.data(), &info, DIB_RGB_COLORS) == height) {
                    for (std::size_t i = 0; i < pixels.size(); ++i)
                        pixels[i] = (mask[i] & 0xFFFFFF) == 0 ? (pixels[i] | 0xFF000000u) : 0u;
                    ok = true;
                }
            }
        }
        ReleaseDC(nullptr, screen);
        hotX = static_cast<int>(icon.xHotspot);
        hotY = static_cast<int>(icon.yHotspot);
    }
    if (icon.hbmColor != nullptr) DeleteObject(icon.hbmColor);
    if (icon.hbmMask != nullptr) DeleteObject(icon.hbmMask);
    return ok;
}

// Cursor size as a percent of kCursorWidth (before display scaling). The wiki sets it from the game
// window's height, so the cursor grows and shrinks with the game's resolution.
std::atomic<int> gCursorScale{100};
int gRenderedCursorScale = 0;  // scale the cursor image was last drawn at (cursor thread only)

int CursorWidth(int dpi) { return std::max(1, MulDiv(MulDiv(kCursorWidth, gCursorScale.load(), 100), dpi, 96)); }
int CursorHeight(int dpi) { return std::max(1, MulDiv(MulDiv(kCursorHeight, gCursorScale.load(), 100), dpi, 96)); }

// Moves the cursor window to the pointer, but only when the pointer has moved.
void MoveCursorToPointer(HWND window) {
    POINT point{};
    if (!GetCursorPos(&point)) return;
    POINT* last = reinterpret_cast<POINT*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (last != nullptr) {
        if (last->x == point.x && last->y == point.y) return;
        *last = point;
    }
    // The window sits up and to the left of the pointer so the cursor's hot spot is on the pointer.
    SetWindowPos(window, HWND_TOPMOST, point.x - gCursorHotX.load(), point.y - gCursorHotY.load(), 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
}

// Draws the cursor into the cursor window with per-pixel alpha: the game's own cursor picture when one was
// remembered (RememberGameCursor), at its own size, otherwise the built-in gauntlet (cursor_art.hpp, made
// by tools/make_cursor.py), sized for the dpi and the game's height. Done each time the window is shown.
void RenderCursorImage(HWND window) {
    const int dpi = static_cast<int>(GetDpiForWindow(window));
    gRenderedCursorScale = gCursorScale.load();
    std::vector<std::uint32_t> game;
    int sourceWidth = kCursorWidth, sourceHeight = kCursorHeight;
    const std::uint32_t* source = kCursorPixels;
    int hotX = MulDiv(MulDiv(kCursorHotX, gCursorScale.load(), 100), dpi, 96);
    int hotY = MulDiv(MulDiv(kCursorHotY, gCursorScale.load(), 100), dpi, 96);
    int width = CursorWidth(dpi);
    int height = CursorHeight(dpi);
    const HCURSOR remembered = kUseGameCursor ? gGameCursor.load() : nullptr;
    int gameHotX = 0, gameHotY = 0;
    if (remembered != nullptr && CursorPixels(remembered, game, sourceWidth, sourceHeight, gameHotX, gameHotY)) {
        source = game.data();
        width = sourceWidth;  // the game's cursor is drawn at the size the game uses
        height = sourceHeight;
        hotX = gameHotX;
        hotY = gameHotY;
    } else {
        sourceWidth = kCursorWidth;
        sourceHeight = kCursorHeight;
    }
    gCursorHotX = hotX;
    gCursorHotY = hotY;
    gCursorDrawWidth = width;
    gCursorDrawHeight = height;
    const HDC screen = GetDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top row first
    void* bits = nullptr;
    const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap != nullptr && bits != nullptr) {
        // Scale by averaging the source pixels under each target pixel (one to one for the game's cursor).
        // The colours are already multiplied by alpha, so averaging every channel is correct. GDI scaling is
        // not used because it can clear the alpha channel.
        auto* out = static_cast<std::uint32_t*>(bits);
        for (int y = 0; y < height; ++y) {
            const int y0 = y * sourceHeight / height;
            const int y1 = std::max(y0 + 1, std::min(sourceHeight, ((y + 1) * sourceHeight + height - 1) / height));
            for (int x = 0; x < width; ++x) {
                const int x0 = x * sourceWidth / width;
                const int x1 = std::max(x0 + 1, std::min(sourceWidth, ((x + 1) * sourceWidth + width - 1) / width));
                std::uint32_t sum[4] = {0, 0, 0, 0};
                for (int sy = y0; sy < y1; ++sy) {
                    for (int sx = x0; sx < x1; ++sx) {
                        const std::uint32_t pixel = source[sy * sourceWidth + sx];
                        sum[0] += pixel >> 24;
                        sum[1] += (pixel >> 16) & 255;
                        sum[2] += (pixel >> 8) & 255;
                        sum[3] += pixel & 255;
                    }
                }
                const std::uint32_t count = static_cast<std::uint32_t>((y1 - y0) * (x1 - x0));
                out[y * width + x] = ((sum[0] / count) << 24) | ((sum[1] / count) << 16) | ((sum[2] / count) << 8) |
                                     (sum[3] / count);
            }
        }
        const HDC memory = CreateCompatibleDC(screen);
        const HGDIOBJ old = SelectObject(memory, bitmap);
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        POINT origin{0, 0};
        SIZE size{width, height};
        UpdateLayeredWindow(window, screen, nullptr, &size, memory, &origin, 0, &blend, ULW_ALPHA);
        SelectObject(memory, old);
        DeleteDC(memory);
    }
    if (bitmap != nullptr) DeleteObject(bitmap);
    ReleaseDC(nullptr, screen);
}

// Click-through window that draws the game's cursor at the pointer. It lives on its own thread (CursorThread),
// so repaints of the wiki cannot delay it.
// What the UI thread hands to the cursor thread for the left stick, plus the cursor thread's flags. The stick moves the
// real pointer on the cursor thread (see CursorThread), so the pointer and the drawn cursor that follows it glide.
struct CursorDrive {
    std::atomic<float> stickX{0.0f}, stickY{0.0f};  // left stick, -1..1, up is positive
    std::atomic<bool> driving{false};               // a controller is in use: the stick moves the pointer
    std::atomic<bool> hidden{false};                // D-pad use hides the drawn cursor until the stick or the mouse moves
    std::atomic<int> speed{1300};                   // pixels per second at a full push
    std::atomic<LONG> pointerX{-1}, pointerY{-1};   // where the stick last put the pointer, to tell it from the real mouse
    std::atomic<ULONGLONG> movedAt{0};              // when it did (GetTickCount64)
    void Stop() {
        driving = false;
        hidden = false;
        stickX = 0.0f;
        stickY = 0.0f;
    }
};
thread_local CursorDrive* tCursorDrive = nullptr;  // the cursor thread's own overlay (CursorProc runs on that thread)

LRESULT CALLBACK CursorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    thread_local bool wanted = false;  // the wiki is open; only this thread reads or writes it
    switch (message) {
    case kMsgCursorShow:
        wanted = true;
        if (!OwnWindowInFront() || (tCursorDrive != nullptr && tCursorDrive->hidden.load())) return 0;  // the timer shows it later
        if (IsWindow(window)) {
            POINT* last = reinterpret_cast<POINT*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (last != nullptr) *last = POINT{-1, -1};
        }
        RenderCursorImage(window);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        MoveCursorToPointer(window);
        return 0;
    case kMsgCursorHide:
        wanted = false;
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_TIMER: {
        thread_local int polls = 0;
        if (!wanted && ++polls % 5 == 0) RememberGameCursor();  // about every 40 ms while the wiki is closed
        POINT pointer{};
        const bool inside = !GetCursorPos(&pointer) || PointInCover(pointer);
        const bool show = wanted && OwnWindowInFront() && inside && !(tCursorDrive != nullptr && tCursorDrive->hidden.load());
        if (show && !IsWindowVisible(window)) {
                POINT* last = reinterpret_cast<POINT*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (last != nullptr) *last = POINT{-1, -1};
            RenderCursorImage(window);
            ShowWindow(window, SW_SHOWNOACTIVATE);
        } else if (!show && IsWindowVisible(window)) {
            ShowWindow(window, SW_HIDE);
        }
        if (IsWindowVisible(window) && gRenderedCursorScale != gCursorScale.load()) {
            RenderCursorImage(window);  // the wiki's scale changed (window resized)
            POINT* last = reinterpret_cast<POINT*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (last != nullptr) *last = POINT{-1, -1};
        }
        if (IsWindowVisible(window)) MoveCursorToPointer(window);
        return 0;
    }
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        HideSystemCursor();
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// The controller help picture: it only shows, never takes the mouse or the focus.
LRESULT CALLBACK HelpProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK BlockerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_SETCURSOR) {
        HideSystemCursor();
        return TRUE;
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_CLOSE) return 0;
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

// One entry of a drop-down list on the IAS calculator page.
struct CalcOption {
    std::string label;
    int value = -1;
    std::string key;  // the weapon type, for the type lists
};

// One tab of the Campaign Progression map laid out in pixels from the graph's top left (overlay_campaign.inc). Made again
// when the data, the tab, the scale, the fonts or the language change.
struct CampaignGraph {
    std::shared_ptr<const campaign::Data> data;  // what it was made for
    int tab = -1;
    int scaleKey = 0;  // the scale * 1000
    HFONT font = nullptr;
    std::string lang;
    double scale = 1;
    int width = 0, height = 0;
    int padX = 0, padY = 0;  // the text's inset in its backing pill
    struct Dot {
        campaign::Point at;
        int area = 0;
        bool ghost = false;
        std::wstring member;  // a dungeon level's short label ("L3"), right of its dot
        RECT memberBox{};     // its backed box (empty when there is none)
        campaign::MarkerSlot marks;  // where its boss / quest item / caveat marks go
        int markCount = 0;
    };
    struct Label {
        RECT box{};               // the backing pill; the text is inset by padX / padY
        std::wstring name, info;  // the name (one line where it fits) and, for a ghost, "-> Act II"
        int nameHeight = 0;
        int area = 0;              // the station's first area
        int first = 0, last = 0;   // its dots (indices into dots)
        bool stack = false, ghost = false;
        bool smallFont = false;    // a stack's common name, in the small font
    };
    struct Line {
        std::vector<campaign::Point> points;
        campaign::EdgeKind kind = campaign::EdgeKind::Passage;
        bool arrow = false;
        int fromDot = -1, toDot = -1;
        std::wstring via;          // shown only while the edge or one of its ends is hovered or selected
        campaign::Point viaAt;
    };
    struct Region {
        RECT box{}, caption{};     // caption: the backed box at the region's top left
        std::wstring label;
    };
    std::vector<campaign::Node> nodes;
    std::vector<campaign::Neighbours> neighbours;
    std::vector<Dot> dots;  // one per node
    std::vector<Label> labels;  // one per station
    std::vector<Line> lines;
    std::vector<Region> regions;
};

namespace {

// What a SkillsHit is (OverlayState::SkillsHit::kind).
constexpr int kSkTab = 0;        // a tree tab (value: the tree's number)
constexpr int kSkTile = 1;       // a tile of the tree (value: index into the tree's cells) or a row of the item list
constexpr int kSkStep = 2;       // a - / value / + box (value: field * 3 + part; field 0 skill level, 1 character
                                 // level, 2 + i synergy i of the Values table)
constexpr int kSkQuick = 3;      // a quick skill level (value: the level)
constexpr int kSkMaxAll = 4;
constexpr int kSkReset = 5;
constexpr int kSkSubTab = 6;     // Values / Items / About (value 0..2)
constexpr int kSkAllLevels = 7;

}  // namespace

struct OverlayState {
    std::function<bool(unsigned int)> isCloseKey;  // see Overlay::SetCloseKey
    const Database* database = nullptr;
    std::mutex pendingLock;                  // guards pendingDb
    std::unique_ptr<Database> pendingDb;     // handed over by ReplaceDatabase, taken on the UI thread
    std::unique_ptr<Database> ownedDb;       // the database now shown, when it came from ReplaceDatabase
    std::wstring dataDir;
    HANDLE thread = nullptr;
    HANDLE ready = nullptr;
    HWND window = nullptr;
    HWND blocker = nullptr;
    HWND cursorWindow = nullptr;
    HDC backgroundDc = nullptr;  // background art composed once per window size
    HBITMAP backgroundBitmap = nullptr;
    HGDIOBJ backgroundPrevious = nullptr;
    int backgroundWidth = 0;
    int backgroundHeight = 0;
    int backgroundSplit = 0;  // header line position the cached background was built for
    POINT lastCursor{-1, -1};
    HANDLE cursorThread = nullptr;
    DWORD cursorThreadId = 0;
    HWND search = nullptr;
    HWND previousForeground = nullptr;
    ULONG_PTR gdiplusToken = 0;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontBold = nullptr;
    HFONT fontTitle = nullptr;
    HFONT fontSnippet = nullptr;      // the card text under a search suggestion (9 pt), and its matched words (bold)
    HFONT fontSnippetBold = nullptr;
    HBRUSH searchBrush = nullptr;
    HBRUSH backgroundBrush = nullptr;
    bool firstPainted = false;   // false until the current show has painted once
    ULONGLONG handoffStarted = 0;
    bool handoffSettled = false;
    bool handingOff = false;     // the other panel is taking over; this one is waiting to hide
    int dpi = 96;
    double uiScale = 1.0;  // font and text scale from the window size, on top of DPI
    int bodyLine = 16;     // line heights in pixels for the current fonts
    int smallLine = 14;
    int boldLine = 18;
    int snippetLine = 15;
    int width = 0;
    int height = 0;
    // Entry list geometry, set by ComputeLayout. EnsurePages splits the filtered entries into list pages,
    // so a row can grow to fit a wrapped title.
    int listLeftX = 0;
    int listRightX = 0;
    int listTopY = 0;
    int listBottomY = 0;
    int listRowGap = 0;
    int listTextWidth = 1;
    bool pagesDirty = true;
    std::vector<std::size_t> pageStarts{0};  // first filtered entry on each list page
    std::vector<int> rowHeights;             // row height for each filtered entry
    std::size_t pageIndex = 0;
    std::vector<PageView> views;
    std::string query;
    std::vector<std::size_t> filtered;
    std::vector<std::size_t> tabMatches;  // matching entries on each page, for the tab highlights
    std::size_t totalMatches = 0;         // matching entries across all pages
    bool hasPendingSelect = false;        // after clearing a search: keep this entry selected on this tab
    std::size_t pendingPage = 0;
    std::size_t pendingSelect = 0;
    std::size_t searchAnchor = 0;         // the entry being viewed when the search started
    std::size_t searchAnchorPage = 0;     // the tab that search started on
    std::map<std::string, std::unique_ptr<Gdiplus::Bitmap>> images;
    Layout layout;
    bool started = false;
    bool open = false;  // user wants it open; it only shows while the game is active
    std::wstring title = L"ESR Wiki";
    std::wstring windowClassName;
    std::wstring blockerClassName;
    std::wstring cursorClassName;
    std::vector<SwitchButton> switches;  // buttons that open the other panels, left of the close button
    void DrawSwitchButton(HDC dc, const RECT& rect, const std::wstring& label, bool current);
    void BeginSwitch(const std::function<void()>& onSwitch);
    bool greenNames = false;  // set-item pages draw names and headers in green
    bool gemwordCards = false;          // the Gemwords page: its cards are three per row
    COLORREF itemNameColor = kGold;     // item names on this page: gold uniques/runewords/gemwords, green sets, orange class crafting
    COLORREF tooltipNameColor = kGold;  // tooltip heading colour on this page (sets keep the plain gold)
    int titleLine = 18;       // line height of fontTitle
    bool trackingMouse = false;
    HWND gameWindow = nullptr;

    // ---- Controller (pad_logic.hpp has the rules, gamepad.hpp reads the devices) ----
    esrwiki::pad::Gamepad gamepad;
    esrwiki::pad::Repeater padRepeat;
    bool padCursorMoved = false;  // the left stick moved the cursor: the next D-pad press starts from what is under it
    ULONGLONG padGraceUntil = 0;  // after a controller switch the pad may take a moment to open: stay in controller mode
    bool padRunning = false;   // the poll timer is on and the controller devices are open (the window is showing)
    bool padActive = false;    // a controller was the last thing used; the mouse or keyboard switches it off
    // Cards: item tiles in the detail pane; Chips: the filter and sort buttons above them; Calc: the calculator's fields;
    // Switches: header buttons
    enum class PadZone { Tabs, List, Cards, Switches, Chips, Calc, Spec, Campaign, Skills };
    PadZone padZone = PadZone::Tabs;
    std::size_t padChip = 0;   // the filter or sort button the D-pad highlights (an index into layout.chips)
    int calcPadKind = 0;       // the calculator field the D-pad highlights: 0 drop-down, 1 number
    int calcPadId = -1;        // and its id
    int calcPadOption = 0;     // the highlighted row of the open drop-down list
    int calcHold = 0;          // Left/Right presses in a row on a number, for the step size
    ULONGLONG calcHoldAt = 0;
    std::size_t tabFocus = 0;  // the tab highlighted with the D-pad (opened with A)
    esrwiki::pad::Kind padKind = esrwiki::pad::Kind::None;
    ULONGLONG padLast = 0;        // time of the previous poll
    ULONGLONG padMouseUntil = 0;  // the left stick moved the cursor: A clicks at the cursor until this time
    CursorDrive drive;            // the left stick and the cursor flags, shared with the cursor thread
    double padScrollCarry = 0;
    bool oskOpen = false;
    bool oskAccents = false;
    int oskRow = 1, oskCol = 0;
    esrwiki::pad::OskRows oskRows;
    struct OskHit { RECT rect; int row; int col; };
    std::vector<OskHit> oskHits;
    RECT oskRect{};
    HWND helpWindow = nullptr;  // the controller help picture, right of the wiki
    std::wstring helpClassName;
    int helpWidth = 0;
    esrwiki::pad::Kind helpDrawnKind = esrwiki::pad::Kind::None;
    int helpDrawnWidth = 0, helpDrawnHeight = 0;
    int helpDrawnContext = -1;  // the SearchFocus the help list was drawn for
    int helpSyncedContext = -1; // and the one PadTick last asked the help window for
    // The suggestion list has the controller's focus: the D-pad picks a row, A opens it, B closes the list.
    bool suggestPadFocus = false;
    esrwiki::pad::PopupPlace popupPlace;  // where the keyboard and the suggestion list go (PlanSearchPopups)
    int oskUnits = 20, oskUnit = 12, oskBoxWidth = 0;  // the keyboard's widest row in half keys, a half key, its width
    esrwiki::pad::SearchFocus PadSearchFocus() const;
    esrwiki::pad::FocusContext PadFocusContext() const;
    void PadSetSearchFocus(esrwiki::pad::SearchFocus next);
    void PadSuggestStep(int delta, bool repeat = false);
    bool PadSuggestShortcut();
    void PlanSearchPopups();
    void PadSync();
    void PadTick();
    void PadDirection(int dx, int dy);
    void PadConfirm(ULONGLONG now);
    void PadBack();
    void PadStepTab(int direction);
    void PadClickAtCursor();
    void PadMouseUsed();
    void PadModeChanged();
    void OpenOsk();
    void CloseOsk();
    void PressOskKey(int row, int col);
    bool OskClick(int x, int y);
    void DrawPadFocus(HDC dc);
    void DrawCardFocus(HDC dc);
    void PadCardMove(int dx, int dy);
    void PadEnterCards();
    void PadEnterSwitches();
    void PadLeaveCards();
    void PadFocusFromCursor();
    void PadEnterChips(int fromX);
    void PadValidateZone();
    void PadEnterCalc();
    void PadEnterSpec();
    void CalcPadMove(int dx, int dy);
    void CalcPadConfirm();
    bool CalcPadClear();
    bool CalcPadBack();
    bool CalcPadRect(RECT& out) const;
    void CalcPadReveal();
    void DrawDetailFocus(HDC dc);
    void ApplyChip(const Chip& chip);
    HelpMetrics MeasureHelp(esrwiki::pad::Kind kind);
    float HelpIconWidth(const std::string& folder, const char* name, float iconHeight);
    void DrawOsk(HDC dc);
    void PositionHelp();
    void RenderHelp();
    void SyncHelp();
    RECT fittedGameRect{};
    std::vector<DetailHit> detailHits;
    std::vector<CardHit> cardHits;          // item tiles on screen, rebuilt each paint
    const Entry* padCardEntry = nullptr;     // the card the D-pad highlights (its tooltip is shown)
    const Block* padCardBlock = nullptr;
    int padCardPending = 0;                 // a scroll that was started to find the next card: +1 down, -1 up
    std::size_t padSwitch = 0;              // the header button the D-pad highlights
    Hover hover;
    int clipTop = 0;        // visible band of the detail content, in client coordinates
    int clipBottom = 0;
    int scrollTotal = 0;    // content height and visible height from the last paint
    int scrollVisible = 0;
    std::vector<int> matchTops;  // scroll offsets of the blocks that match the search, from the last layout
    int matchBase = 0;           // the top of the detail band, for those offsets
    bool dragging = false;  // dragging the scrollbar thumb
    int dragStartY = 0;
    int dragStartScroll = 0;

    // The IAS calculator page. calcPending is handed over by Overlay::SetCalculatorData from another thread; the rest
    // belongs to the UI thread.
    std::mutex calcLock;
    std::shared_ptr<const ias::Data> calcPending;
    std::shared_ptr<const ias::Data> calcData;
    ias::Setup calcSetup;
    std::string calcMainType, calcOffType;  // the weapon type lists narrow the weapon lists
    int calcOpen = -1;        // the drop-down whose list is showing
    int calcListScroll = 0;   // its first visible row
    int calcNumFocus = -1;    // the number field being typed in
    std::string calcNumText;
    bool calcNumFresh = false;  // the next character replaces the whole value
    struct CalcHit { RECT rect; int kind; int index; };  // kind: 0 drop-down, 1 number field, 2 list row
    std::vector<CalcHit> calcHits;
    RECT calcListRect{};
    bool CalcActive() const;
    bool CalcClick(int x, int y);
    bool CalcWheel(int x, int y, int steps);
    bool CalcChar(wchar_t ch);
    int* CalcNumber(int id);
    void CalcChoose(int field, const CalcOption& option);
    int RenderCalculator(HDC dc, int top, int scroll, bool draw);

    // The Specializations page. specPending is handed over by Overlay::SetSpecializationsData from another thread; the
    // rest belongs to the UI thread.
    std::mutex specLock;
    std::shared_ptr<const spec::Data> specPending;
    std::shared_ptr<const spec::Data> specData;
    int specTab = -1;          // the shown tab; -1 is the page's own default
    int specHoverTab = -1;     // the tab and the perk under the pointer
    int specHoverPerk = -1;
    RECT specHoverRect{};
    int specPadKind = 1;       // what the D-pad highlights: 0 a tab, 1 a perk
    int specPadIndex = 0;
    struct SpecHit { RECT rect; int kind; int index; };  // kind: 0 tab, 1 perk (of the shown tab)
    std::vector<SpecHit> specHits;
    bool SpecActive() const;
    int SpecShownTab() const;
    void SpecSelectTab(int index);
    bool SpecClick(int x, int y);
    bool SpecMove(int x, int y);
    void SpecPadMove(int dx, int dy);
    void SpecPadConfirm();
    void SpecPadReveal();
    bool SpecPadRect(RECT& out) const;
    void DrawSpecTooltip(HDC dc);
    int RenderSpecializations(HDC dc, int top, int scroll, bool draw);
    // Search on the board (overlay_spec.inc): the perks are records of the search index (SetExtra), rebuilt when the data,
    // the database or the language change; the in-page filter keeps the board while a perk matches and dims the others.
    bool AdoptSpecData();      // takes specPending (UI thread only); true when the data changed
    void ApplySpecData();      // kMsgSpecData: adopts it and looks the search text up again
    int SpecPageIndex() const; // the database page of the board, or -1
    std::vector<esrwiki::search::ExtraRecord> SpecSearchRecords() const;
    const std::vector<std::vector<char>>& SpecMatches();  // per tab, per perk: matches the search text (empty: no text)
    std::vector<std::size_t> FilterPage(std::size_t index);  // FilterEntries, keeping the board's page while a perk matches
    bool SpecFlashing() const;
    std::shared_ptr<const spec::Data> specIndexed;  // the data the index's perk records were made from
    const Database* specIndexedFor = nullptr;       // and the database (for the page and its title)
    std::vector<std::vector<char>> specMatch;
    std::size_t specMatchCount = 0;
    bool specMatchValid = false;
    std::string specMatchQuery;                     // what specMatch was made for
    std::shared_ptr<const spec::Data> specMatchData;
    const Database* specMatchDb = nullptr;
    int specFlashTab = -1;     // the perk a search jump arrived at, framed until specFlashUntil (and its tooltip shown)
    int specFlashPerk = -1;
    ULONGLONG specFlashUntil = 0;

    // The Campaign Progression map (overlay_campaign.inc), under the text of that Welcome entry. campaignPending is handed
    // over by Overlay::SetCampaignData from another thread; the rest belongs to the UI thread.
    std::mutex campaignLock;
    std::shared_ptr<const campaign::Data> campaignPending;
    std::shared_ptr<const campaign::Data> campaignData;
    int campaignTab = 0;            // the shown tab (an index into campaignData->tabs)
    int campaignSelected = 0;       // the area whose details show under the map (0: none)
    int campaignHoverKind = -1;     // what the pointer is on (a CampaignHit kind, -1: nothing) and its value
    int campaignHover = 0;
    RECT campaignHoverRect{};
    bool campaignSpoilers = false;  // the selected area's spoiler notes are shown
    int campaignPadIndex = 0;       // the area the D-pad highlights (an index into campaignSpots)
    bool campaignPadFollow = false; // after the next layout, the highlight goes to the selected area
    bool campaignPadRevealNext = false;  // after the next layout, the highlight is scrolled into view
    bool campaignReveal = false;    // after the next layout, the selected area is scrolled into view
    int campaignFlashArea = 0;      // the area a search jump arrived at, ringed until campaignFlashUntil
    ULONGLONG campaignFlashUntil = 0;
    // kind: 0 tab (value: its index), 1 area (dot, label or card; value: area id), 2 link chip (area id), 3 spoilers, 4 close,
    // 5 an edge with a "via" caption (value: index into campaignGraph.lines; hit-tested along the line, not listed here)
    struct CampaignHit { RECT rect; int kind; int value; };
    std::vector<CampaignHit> campaignHits;  // client coordinates, from the last paint
    // The shown tab's areas (dots in node order, or cards) from the last layout, relative to the top of the entry.
    struct CampaignSpot { int area; RECT rect; };
    std::vector<CampaignSpot> campaignSpots;
    CampaignGraph campaignGraph;
    POINT campaignGraphAt{};          // the graph's top left in client coordinates, from the last paint
    bool campaignGraphShown = false;  // the last paint drew a graph tab (its edges can be hovered)
    std::shared_ptr<const campaign::Data> campaignIndexed;  // the data the index's area records were made from
    const Database* campaignIndexedFor = nullptr;
    std::vector<int> campaignMatch;  // the areas matching the search text (sorted ids)
    bool campaignMatchValid = false;
    std::string campaignMatchQuery;
    std::shared_ptr<const campaign::Data> campaignMatchData;
    const Database* campaignMatchDb = nullptr;
    static bool IsCampaignEntry(const Entry& entry);
    bool AdoptCampaignData();
    void ApplyCampaignData();
    std::string CampaignLang() const;  // the data language of the names ("enUS" for English)
    bool CampaignShowing() const;      // the map's entry is the one in the detail pane, and there is data
    int CampaignPageIndex() const;
    int CampaignEntryIndex(const Page& page) const;
    bool CampaignOnTab(int tabIndex, int areaId) const;
    int CampaignTabOf(int areaId) const;
    bool CampaignFlashing() const;
    void CampaignSelectTab(int index);
    void CampaignShowArea(int areaId, bool flash);
    void CampaignPick(int areaId);
    bool CampaignEscape();
    std::vector<esrwiki::search::ExtraRecord> CampaignSearchRecords() const;
    const std::vector<int>& CampaignMatches();
    int CampaignAreaOfHit(const esrwiki::search::Hit& hit) const;
    void CampaignLayoutGraph(HDC dc, int tabIndex, double s);
    int RenderCampaign(HDC dc, Gdiplus::Graphics* graphics, int left, int paneW, int y, bool draw);
    int RenderCampaignLegend(HDC dc, Gdiplus::Graphics* graphics, int left, int paneW, int y, bool draw);
    int RenderCampaignGraph(HDC dc, Gdiplus::Graphics* graphics, int left, int paneW, int y, bool draw, const std::vector<int>& matches);
    int RenderCampaignCards(HDC dc, Gdiplus::Graphics* graphics, int left, int paneW, int y, bool draw, const std::vector<int>& matches);
    int RenderCampaignDetails(HDC dc, int left, int paneW, int y, bool draw);
    int CampaignDetailBody(HDC dc, const campaign::Area& area, int x, int bodyW, int y, bool draw);
    std::vector<std::wstring> CampaignCaveats(const campaign::Area& area) const;
    void CampaignApplyReveal(int total, int visible);
    bool CampaignMove(int x, int y);
    bool CampaignClick(int x, int y);
    void DrawCampaignTooltip(HDC dc);
    void PadEnterCampaign();
    bool CampaignPadRect(RECT& out) const;
    void CampaignPadReveal();
    void CampaignPadMove(int dx, int dy);
    void CampaignPadConfirm();
    bool CampaignPadBack();
    bool CampaignPadStepTab(int direction);
    // The official Maps page on the map (overlay_campaign_live.inc): the Special areas cards and the teleport caveats in
    // the website's wording. mapsPending is handed over by Overlay::SetMapsPageData from another thread.
    std::mutex mapsLock;
    std::shared_ptr<const campaign::LiveMaps> mapsPending;
    std::shared_ptr<const campaign::LiveMaps> mapsData;
    bool AdoptMapsData();   // takes mapsPending (UI thread only); true when the data changed
    void ApplyMapsData();   // kMsgMapsData: adopts it and repaints
    std::wstring CampaignLiveCardEntry(const campaign::Tab& tab, const campaign::Area& area) const;
    bool CampaignLiveTeleport(const campaign::Area& area, std::vector<std::wstring>& out) const;
    bool CampaignLiveHidesNote(const campaign::Tab& tab, const campaign::Note& note) const;
    int RenderCampaignLiveNotes(HDC dc, const campaign::Tab& tab, int left, int paneW, int y, bool draw);

    // The Skills page (overlay_skills.inc). skillsPending is handed over by Overlay::SetSkillsData from another thread; the
    // rest belongs to the UI thread. skillsEngine, skillsLayout, skillsCache and skillsFlash point into skillsData, so they
    // are declared after it (and reset whenever it changes).
    std::mutex skillsLock;
    std::shared_ptr<const skills::Data> skillsPending;
    std::shared_ptr<const skills::Data> skillsData;
    std::unique_ptr<skills::Engine> skillsEngine;
    skills::PageLayout skillsLayout;   // which skill each (hidden, searched) block of the page is
    // What is chosen, kept while the plugin runs: per class the tree and the skill (its key; "" = the tree's first), the
    // levels (0 until the data's defaults are taken) and the synergy points by skill id (Context::synergyPoints).
    struct SkillsChoice { int tree = 0; std::string skill; };
    std::map<std::string, SkillsChoice> skillsChoice;
    int skillsLevel = 0;
    int skillsCharacterLevel = 0;
    std::map<std::string, int> skillsSynergy;
    int skillsSubTab = 0;              // 0 Values, 1 Items, 2 About
    bool skillsAllLevels = false;      // the Values table's every-level rows are showing
    // The shown class's tree (or item list) and the selected skill's facts, values and sources, made again only when the
    // class, the tree, the skill, the levels or the synergy points change (SkillsEnsure).
    struct SkillsCache {
        const skills::Data* data = nullptr;
        std::string cls;
        int tree = -1;
        bool items = false;            // no trees (the item skills): a list
        skills::TreeLayout grid;
        std::vector<const skills::Skill*> list;
        const skills::Skill* skill = nullptr;
        int level = 0;
        int characterLevel = 0;
        std::map<std::string, int> synergy;
        bool built = false;
        skills::HeaderFacts facts;
        skills::ValueTable table;
        std::vector<skills::SourceGroup> sources;
        bool allBuilt = false;
        std::vector<std::vector<skills::ValueCell>> all;
    };
    SkillsCache skillsCache;
    // kind: see kSkTab ... above; zone 0 the tree (or list), 1 the details. rect is cut to what shows,
    // full is the whole box. Client coordinates, from the last paint.
    struct SkillsHit { RECT rect; RECT full; int kind; int value; int zone; };
    std::vector<SkillsHit> skillsHits;
    int skillsHoverKind = -1;          // the box under the pointer
    int skillsHoverValue = 0;
    int skillsPadKind = -1;            // the box the D-pad highlights
    int skillsPadValue = 0;
    bool skillsPadFollow = false;      // after the next paint, the highlight goes to the selected tile
    int skillsHold = 0;                // Left/Right presses in a row on a value, for the step size
    ULONGLONG skillsHoldAt = 0;
    bool skillsWide = true;            // the last layout was split (tree left, details right)
    int skillsDetailScroll = 0;        // split: the details panel's own scroll, its limit and its place on screen
    int skillsDetailMax = 0;
    RECT skillsDetailView{};
    int skillsListScroll = 0;          // split: the item skills' list
    int skillsListMax = 0;
    RECT skillsListView{};
    RECT skillsBarTrack[2]{};          // the scroll bars (0 the item list, 1 the details): the grab area, as last drawn
    RECT skillsBarThumb[2]{};
    int skillsBarTotal[2]{};
    struct LinkHit {
        RECT rect;
        std::wstring url;
    };
    std::vector<LinkHit> linkHits;     // the web links drawn in the page's text, as last drawn
    int linkHot = -1;                  // the link under the pointer, or -1
    int LinkAt(int x, int y) const;
    bool LinkClick(int x, int y);
    int skillsBarDragging = -1;        // the bar being dragged by the mouse, or -1
    int skillsBarStartY = 0;
    int skillsBarStartScroll = 0;
    bool skillsReveal = false;         // the selected tile is to be scrolled into view after the next layout
    int skillsRevealTop = 0;
    int skillsRevealBottom = 0;
    int skillsTileTop = 0;             // the selected tile's place in the last layout
    int skillsTileBottom = 0;
    const skills::Skill* skillsFlash = nullptr;  // the tile a search jump arrived at, framed until skillsFlashUntil
    ULONGLONG skillsFlashUntil = 0;
    skills::IconMap skillsIcons;       // data\skill-icons.txt, read on first use
    bool skillsIconsLoaded = false;
    std::unordered_map<const skills::Skill*, bool> skillsLitMap;  // SkillsLit's answers for skillsLitQuery
    std::string skillsLitQuery;
    const Database* skillsLitDb = nullptr;
    HDC skillsClipDc = nullptr;        // SkillsClipped's off-screen copy, kept while the client size stays
    HBITMAP skillsClipBitmap = nullptr;
    HGDIOBJ skillsClipPrevious = nullptr;
    int skillsClipWidth = 0;
    int skillsClipHeight = 0;
    void ReleaseSkillsClip();
    bool SkillsActive() const;
    bool AdoptSkillsData();            // takes skillsPending (UI thread only); true when there was new data
    void ApplySkillsData();            // kMsgSkillsData: adopts it and shows the page made from it
    void PrepareSkillsPage();          // puts the page into the database shown (or takes it out without data)
    int SkillsShownEntry();
    const skills::Class* SkillsShownClass();
    const skills::Skill* SkillsTileSkill(int value) const;
    int SkillsTileOf(const skills::Skill* skill) const;
    const Block* SkillsBlockOf(const skills::Skill* skill);
    bool SkillsLit(const skills::Skill* skill);
    void SkillsEnsure(const skills::Class& cls);
    skills::Context SkillsContext(const skills::Skill& skill) const;
    void SkillsSelect(const skills::Skill& skill, bool flash);
    void SkillsSelectTree(int tree);
    void SkillsTakeJump();
    bool SkillsStepMatch(int direction);
    bool SkillsStepField(int field, int delta);
    void SkillsActivate(int kind, int value);
    Gdiplus::Bitmap* SkillsIcon(const skills::Skill& skill);
    void SkillsDrawIcon(HDC dc, Gdiplus::Graphics* graphics, const skills::Skill& skill, const RECT& r);
    void SkillsAddHit(const RECT& r, int kind, int value, int zone, const RECT& view);
    bool SkillsHot(int kind, int value) const;
    void SkillsBox(HDC dc, const RECT& r, const std::wstring& text, bool selected, bool hot, bool dimText, HFONT font);
    void SkillsClipped(HDC dc, RECT clip, const std::function<void(HDC, Gdiplus::Graphics&)>& paint);
    void SkillsScrollbar(HDC dc, const RECT& view, int total, int scroll, int bar);
    bool SkillsBarDown(int x, int y);
    void SkillsTileSize(int availH, int rows, int& tileH, bool& compact, bool& fits) const;
    void SkillsBarDrag(int y);
    int RenderSkills(HDC dc, const Entry& entry, int top, int scroll, bool draw);
    void SkillsApplyReveal(int total, int visible);
    int RenderSkillsTree(HDC dc, const skills::Class& cls, int x, int w, int y, bool wide, bool draw);
    int SkillsDetails(HDC dc, Gdiplus::Graphics* graphics, const skills::Class& cls, int x, int w, int y, bool draw,
                      const RECT& view);
    bool SkillsMove(int x, int y);
    bool SkillsClick(int x, int y);
    bool SkillsWheel(int x, int y, int steps);
    bool SkillsPadFocusTile();
    void PadEnterSkills();
    const SkillsHit* SkillsPadHit() const;
    bool SkillsPadRect(RECT& out) const;
    void SkillsPadReveal();
    void SkillsPadMove(int dx, int dy);
    void SkillsPadConfirm();
    bool SkillsPadBack();
    bool SkillsPadStepTab(int direction);
    void SkillsPadAfterPaint();

    // Search suggestions (overlay_suggest.inc): the dropdown under the search box and the jump to the card picked.
    esrwiki::search::Index searchIndex;
    const Database* searchIndexFor = nullptr;  // the database the index was built from
    std::vector<esrwiki::search::Hit> suggest;
    std::string suggestQuery;
    int suggestSel = -1;       // the row picked with the keys (-1: none)
    int suggestHover = -1;     // the row under the pointer
    bool suggestOpen = false;
    bool suggestMute = false;  // set while the program itself changes the search text
    bool suggestDirty = false; // the text changed and the list is waiting for kTimerSuggest
    std::wstring suggestMessage;  // what the list says when nothing is found ("Nothing has: ...")
    int suggestDivider = -1;   // the first typo hit, which gets a "Did you mean" row above it (-1: none)
    std::wstring searchNote;   // a note in place of the list's counter, until searchNoteUntil
    ULONGLONG searchNoteUntil = 0;
    int wheelRemainder = 0;    // wheel movement short of a whole notch (precision touchpads send small steps)
    RECT suggestRect{};
    std::vector<RECT> suggestRows;  // the rows showing, from the last paint
    std::vector<int> suggestRowIndex;  // which suggestion each of them is
    int suggestScroll = 0;     // the first suggestion showing
    int suggestVisible = 8;    // how many rows showed
    bool jumpPending = false;  // a jump is waiting for its card to be laid out
    std::size_t jumpEntry = 0;
    int jumpBlock = -1;
    int flashBlock = -1;       // the card framed after a jump, until flashUntil
    ULONGLONG flashUntil = 0;
    struct BlockSpan { int block; int top; int bottom; };  // a card's place in the entry, from the last layout (relative to bodyTop)
    std::vector<BlockSpan> blockSpans;
    int bodyTop = 0;           // the top of the entry's content in the detail pane, for those places
    void EnsureSearchIndex();
    void UpdateSuggest();
    void ScheduleSuggest();
    void FlushSuggest();
    bool ReopenSuggest();
    int SuggestDisplayCount() const;
    int SuggestDisplayRow(int hitIndex) const;
    int SuggestHitOfRow(int row) const;
    std::vector<bool> SuggestPickRows() const;
    void ShowSearchNote(const std::wstring& note);
    void CloseSuggest();
    void DrawSuggest(HDC dc);
    bool SuggestClick(int x, int y);
    bool SuggestMove(int x, int y);
    bool SuggestKey(WPARAM key);
    bool SuggestWheel(int x, int y, int steps);
    void SuggestReveal();
    void JumpTo(const esrwiki::search::Hit& hit);
    void ApplyJump(int total, int visible);
    void DrawJumpFlash(HDC dc);

    const Page& CurrentPage() const { return database->pages[pageIndex]; }
    PageView& CurrentView() { return views[pageIndex]; }
    int S(int value) const { return MulDiv(value, dpi, 96); }
    int RuneRowHeight(const TileStyle& style) const { return style.cell + smallLine + S(4); }
    int RuneRows(std::size_t count, const TileStyle& style) const;
    TileStyle MakeTileStyle(int width_, bool compact, bool fiveAcross = false) const;

    HFONT MakeFont(int points, int weight, double minScale = 0.0) const;
    void CreateFonts();
    void ApplyFonts();
    Layout ComputeLayout();
    void FitToGame();
    void Refilter();
    void SetPage(std::size_t index);
    void SelectFiltered(std::size_t index);
    void EnsurePages();
    std::size_t PageOf(std::size_t index);
    std::vector<RECT> PageRects(std::size_t page);
    void MovePage(int direction);
    void StepMatch(int direction);
    bool StepWithinEntry(int direction);
    void Show();
    void Hide();
    void ApplyPendingDatabase();
    void FinishHandoff();
    void HandoffTick();
    void Toggle();
    void SyncVisibility();
    void ShowBlocker(bool show);
    void ShowCursorWindow(bool show);
    void HandleClick(int x, int y);
    void HandleWheel(int x, int y, int delta);
    void HandleKey(WPARAM key);
    void HandleMove(int x, int y);
    void EndDrag();
    void ClearHover();
    void QueryChanged();
    void Paint(HDC target);
    void PaintList(HDC dc);
    int BuildControls(HDC dc, const Entry& entry, PageView& view);
    void DrawScrollbar(HDC dc, int top, int visible, int total, int scroll);
    int TileHeight(HDC dc, const Block& block, const TileStyle& style);
    int TagRowHeight(HDC dc, const Block& block, const TileStyle& style);
    int RenderTiles(HDC dc, Gdiplus::Graphics* graphics, const Entry& entry, const std::vector<std::size_t>& order,
                    std::size_t begin, std::size_t end, int left, int width_, int y, bool compact, bool draw);
    int RenderWideTiles(HDC dc, Gdiplus::Graphics* graphics, const Entry& entry, const std::vector<std::size_t>& order,
                        std::size_t begin, std::size_t end, int left, int width_, int y, bool draw);
    int RenderGuideBlock(HDC dc, Gdiplus::Graphics* graphics, const Block& block, int left, int width_, int y,
                         bool draw);
    // An ascendancy: the tier lines in one half, the name and picture in the other, like the website.
    int RenderSplitBlock(HDC dc, Gdiplus::Graphics* graphics, const Block& block, int left, int width_, int y, bool draw);
    int splitNow = 0;  // the split layout of the entry being drawn (Entry::splitLayout), 0 for none
    std::string splitTitleNow;
    int RenderTable(HDC dc, const std::vector<Line>& lines, std::size_t first, std::size_t last, int left,
                    int width_, int y, bool draw);
    // Row heights per table, keyed by width and text, so scrolling does not re-measure every cell.
    std::unordered_map<std::string, std::vector<int>> tableHeightCache;
    // The width each column needs for its longest word, per table, for the same reason.
    std::unordered_map<std::string, std::vector<int>> tableNeedCache;
    // Table rows in the order of the active row sort, per block, so painting does not sort again.
    BlockSort rowSortNow = BlockSort::Default;
    BlockSort sortedRowsFor = BlockSort::Default;
    const Database* sortedRowsDb = nullptr;
    std::unordered_map<const Block*, std::vector<Line>> sortedRows;
    // The block order of the detail pane, reused until one of its inputs changes.
    struct ArrangeKey {
        const Database* db;
        std::size_t page;
        std::size_t entry;
        std::size_t socket;
        std::string tier;
        std::string family;
        std::string query;
        BlockSort sort;
        bool operator==(const ArrangeKey& other) const {
            return db == other.db && page == other.page && entry == other.entry && socket == other.socket &&
                   sort == other.sort && tier == other.tier && family == other.family && query == other.query;
        }
    };
    ArrangeKey arrangeKey{};
    std::vector<std::size_t> arrangeOrder;
    bool arrangeValid = false;
    std::vector<std::size_t> ArrangedOrder(const Entry& entry, std::size_t entryIndex, const PageView& view);
    int RenderDetail(HDC dc, const Entry& entry, const std::vector<std::size_t>& order, bool headers, int top,
                     int scroll, bool compact, bool draw);
    void DrawImageKey(Gdiplus::Graphics& graphics, const std::string& key, RECT box);
    void DrawBackground(Gdiplus::Graphics& graphics, RECT client, int headerLineY);
    void EnsureBackgroundCache(HDC target, int width, int height);
    void ReleaseBackgroundCache();
    void DrawTooltip(HDC dc);
    Gdiplus::Bitmap* FindImage(const std::string& key);
};

int OverlayState::RuneRows(std::size_t count, const TileStyle& style) const {
    const std::size_t perRow = static_cast<std::size_t>(std::max(1, (style.tileWidth - S(12)) / style.cell));
    return static_cast<int>((count + perRow - 1) / perRow);
}

// Tiles follow the detail pane width: normal item tiles are three per row with
// art at three quarters of the tile width; compact rune tiles are small, with
// fixed-size art (about a quarter of the normal size) and up to six per row.
TileStyle OverlayState::MakeTileStyle(int width_, bool compact, bool fiveAcross) const {
    TileStyle style;
    style.columns = fiveAcross ? 5 : gemwordCards ? 3 : compact ? (width_ >= S(560) ? 6 : 4) : 3;
    style.gap = compact || fiveAcross ? S(6) : S(12);
    style.tileWidth = std::max(S(40), (width_ - style.gap * (style.columns - 1)) / style.columns);
    if (gemwordCards) {
        // Three per row, but each card keeps the width it has at the compact six (or four) per row, so the
        // row is left-aligned with space to its right.
        const int compactColumns = width_ >= S(560) ? 6 : 4;
        style.tileWidth = std::max(S(40), (width_ - S(6) * (compactColumns - 1)) / compactColumns);
    }
    style.padding = S(6);
    style.nameFont = compact || fiveAcross ? fontSmall : fontBold;
    if (compact) {
        style.artBox = std::max(S(20), std::min(S(35), style.tileWidth - S(12)));
        style.cell = S(30);
    } else {
        style.artBox = std::max(S(30), std::min(style.tileWidth / 2 - S(10), style.tileWidth * 3 / 8));
        style.cell = std::max(S(30), std::min(S(40), (style.tileWidth - S(12)) / 6));
    }
    return style;
}

// `minScale` keeps a font from shrinking below its size at that scale when the window is small.
HFONT OverlayState::MakeFont(int points, int weight, double minScale) const {
    const int pixelHeight = -static_cast<int>(std::lround(points * std::max(uiScale, minScale) * dpi / 72.0));
    return CreateFontW(pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void OverlayState::CreateFonts() {
    const HDC screen = GetDC(nullptr);
    dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    uiScale = 1.0;
    ApplyFonts();
}

// Rebuilds the fonts for the current uiScale and re-measures their line heights.
void OverlayState::ApplyFonts() {
    for (HFONT* font : {&fontBody, &fontSmall, &fontBold, &fontTitle, &fontSnippet, &fontSnippetBold}) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    tableHeightCache.clear();
    gMeasureCache.clear();
    fontBody = MakeFont(10, FW_NORMAL);
    fontSmall = MakeFont(8, FW_NORMAL);
    fontBold = MakeFont(10, FW_SEMIBOLD);
    fontTitle = MakeFont(13, FW_SEMIBOLD);
    fontSnippet = MakeFont(9, FW_NORMAL, 1.0);  // never below 9 pt at the display's scaling
    fontSnippetBold = MakeFont(9, FW_BOLD, 1.0);

    const HDC screen = GetDC(nullptr);
    bodyLine = LineHeight(screen, fontBody);
    smallLine = LineHeight(screen, fontSmall);
    boldLine = LineHeight(screen, fontBold);
    titleLine = LineHeight(screen, fontTitle);
    snippetLine = std::max(LineHeight(screen, fontSnippet), LineHeight(screen, fontSnippetBold));
    ReleaseDC(nullptr, screen);
    if (IsWindow(search)) SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(fontBody), TRUE);
}

// Positions everything for the current width and height. Row heights come from
// the bold font, so two-line titles fit at any scale, and the list length follows
// the window height.
Layout OverlayState::ComputeLayout() {
    Layout computed;
    const auto& pages = database->pages;
    // Content sits inside the background picture's frame, with room to breathe.
    const int margin = kFrameSlice + S(10);

    // Pages are grouped into tab rows by their group name, in order of appearance.
    std::vector<std::string> rowNames;
    std::vector<int> rowOfPage(pages.size(), 0);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const auto found = std::find(rowNames.begin(), rowNames.end(), pages[i].group);
        if (found == rowNames.end()) {
            rowNames.push_back(pages[i].group);
            rowOfPage[i] = static_cast<int>(rowNames.size()) - 1;
        } else {
            rowOfPage[i] = static_cast<int>(found - rowNames.begin());
        }
    }
    std::vector<int> pagesInRow(rowNames.size(), 0);
    for (int row : rowOfPage) ++pagesInRow[static_cast<std::size_t>(row)];

    // One spacing scale: 16 outer margin, 8 between neighbouring blocks, 4 between rows inside a block.
    const int gap = S(8);
    const int tight = S(4);
    const int right = width - margin;

    // Header: the switch and close buttons share one band, right-aligned with the tabs.
    // The header is raised 10 px from its centred spot, so it sits closer to the picture's top frame. The header
    // line and the tabs stay where they were.
    const int headerLine = kFrameSlice + S(6);  // where the header would sit with the header line below it
    const int headerTop = headerLine - S(10);  // clear of the picture's top frame, which grows with the window
    const int headerHeight = S(28);
    // The title, switch buttons and close button share one row, 3 px below the header top, so their centres line up.
    const int rowTop = headerTop + S(3);
    // The buttons sit on the title's row, centred with the title's text.
    const int buttonTop = rowTop + S(1);
    computed.close = Rect(right - S(32), buttonTop, right, buttonTop + headerHeight);
    // The switch buttons sit left of the close button, the first one leftmost; the title takes the rest.
    computed.switchButtons.resize(switches.size());
    int switchRight = computed.close.left - gap;
    for (std::size_t i = switches.size(); i-- > 0;) {
        computed.switchButtons[i] = Rect(switchRight - S(140), buttonTop, switchRight, buttonTop + headerHeight);
        switchRight = computed.switchButtons[i].left - gap;
    }
    computed.title = Rect(margin, rowTop, switchRight, rowTop + headerHeight);

    // Tabs: full width between the margins, equal gaps across and down.
    const int tabRowHeight = std::max(S(30), bodyLine + S(10));
    const int tabGap = gap;
    const int tabTop = headerLine + headerHeight + gap + S(2);
    computed.tabs.resize(pages.size());
    std::vector<int> seenInRow(rowNames.size(), 0);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const auto row = static_cast<std::size_t>(rowOfPage[i]);
        const int count = pagesInRow[row];
        const int column = seenInRow[row]++;
        const int span = right - margin - (count - 1) * tabGap;
        const int left = margin + span * column / count + column * tabGap;
        const int tabRight = margin + span * (column + 1) / count + column * tabGap;
        const int top = tabTop + static_cast<int>(row) * (tabRowHeight + tabGap);
        computed.tabs[i] = Rect(left, top, tabRight, top + tabRowHeight);
    }

    const int contentTop = tabTop + static_cast<int>(rowNames.size()) * (tabRowHeight + tabGap);
    // The picture's header line sits under the title row, so the tabs sit on the plain content
    // background rather than inside the header art.
    computed.separatorY = headerLine + headerHeight + S(6);
    const int bottom = height - margin;
    const int listWidth = Bounded(static_cast<int>(S(340) * uiScale), S(260), std::max(S(260), width * 38 / 100)) * 85 / 100 * 70 / 100;
    const int listRight = margin + listWidth;

    // List column: search, match count and buttons, rows, then paging pinned to the bottom edge.
    // Tall enough for the body font plus the control border, so the text caret fits inside the box.
    const int searchHeight = std::max(S(28), bodyLine + S(10));
    const int countHeight = S(22);
    const int buttonWidth = S(24);
    const int countTop = contentTop + searchHeight + tight;
    computed.search = Rect(margin, contentTop, listRight, contentTop + searchHeight);
    computed.clearQuery = Rect(listRight - searchHeight, contentTop, listRight, contentTop + searchHeight);
    computed.matchNext = Rect(listRight - buttonWidth, countTop, listRight, countTop + countHeight);
    computed.matchPrev = Rect(computed.matchNext.left - tight - buttonWidth, countTop,
                              computed.matchNext.left - tight, countTop + countHeight);
    computed.count = Rect(margin, countTop, computed.matchPrev.left - tight, countTop + countHeight);

    const int pagingHeight = std::max(S(30), bodyLine + S(10));
    const int listTop = countTop + countHeight + gap;
    const int pagingTop = bottom - pagingHeight;
    // The rows themselves are placed per list page once their titles are measured (see EnsurePages).
    listLeftX = margin;
    listRightX = listRight;
    listTopY = listTop;
    listBottomY = pagingTop - gap;
    listRowGap = tight;
    listTextWidth = std::max(S(20), listWidth - S(24));
    pagesDirty = true;
    const int pagingButton = S(80);
    computed.prev = Rect(margin, pagingTop, margin + pagingButton, bottom);
    computed.next = Rect(listRight - pagingButton, pagingTop, listRight, bottom);
    computed.pageLabel = Rect(computed.prev.right + gap, pagingTop, computed.next.left - gap, bottom);

    // Detail pane: one margin right of the list, same top and bottom edges as the list column.
    computed.detail = Rect(listRight + margin, contentTop, right, bottom);
    return computed;
}

// Sizes the window to the game: about 80% of the game's width and 90% of its
// height, clamped to a sensible range and centered over the game. Fonts scale with
// the resulting width. Without a game window it is centered on the screen.
void OverlayState::FitToGame() {
    if (!IsWindow(gameWindow)) gameWindow = FindGameWindow(window);
    int left = 0;
    int top = 0;
    int w = S(960);
    int h = S(620);
    helpWidth = 0;
    if (IsWindow(gameWindow)) {
        const RECT game = GameScreenRect(gameWindow);
        const int cw = game.right - game.left;
        const int ch = game.bottom - game.top;
        // 60% of the game's width and 72% of its height, within the same limits as before: 1920x1080 gives about
        // 1152x778 (it was 1500x972 and filled the screen); at 4K the limits still apply, so it is unchanged there.
        w = std::min(Bounded(cw * 3 / 5, S(960), S(1500)), cw);
        h = std::min(Bounded(ch * 72 / 100, S(620), S(1000)), ch);
        left = game.left + (cw - w) / 2;
        top = game.top + (ch - h) / 2;
        // The controller help picture sits right of the wiki, where the wiki already is; the wiki does not move.
        if (padActive) helpWidth = std::clamp(cw * 17 / 100, S(280), S(440));
        fittedGameRect = game;
    } else {
        left = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        top = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        fittedGameRect = RECT{};
    }

    PlaceBlocker(blocker, gameWindow);

    const double scale = std::clamp(static_cast<double>(w) / S(960), 0.85, 1.5);
    if (std::fabs(scale - uiScale) > 0.02) {
        uiScale = scale;
        ApplyFonts();
    }
    // The cursor follows the game window's height, relative to the display scaling. Tuned on a 150% display:
    // 150 px wide in a 2160 px high window and 62 px wide at 1080p, which is a little more than linear.
    // Display scaling does not change it (the 1440 is a height in 96 dpi units).
    gCursorScale = std::clamp(static_cast<int>(std::lround(80.0 * std::pow(static_cast<double>(h) / S(1440), 1.27))), 20, 150);

    width = w;
    height = h;
    RECT current{};
    GetWindowRect(window, &current);
    if (current.left != left || current.top != top || current.right - current.left != w ||
        current.bottom - current.top != h) {
        SetWindowPos(window, nullptr, left, top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    layout = ComputeLayout();
    if (IsWindow(search)) {
        const RECT edit = SearchEditRect(layout, bodyLine, S(6));
        MoveWindow(search, edit.left, edit.top, edit.right - edit.left, edit.bottom - edit.top, TRUE);
    }
    PositionHelp();
}

Gdiplus::Bitmap* OverlayState::FindImage(const std::string& key) {
    const auto cached = images.find(key);
    if (cached != images.end()) return cached->second.get();
    const std::wstring path = dataDir + L"\\images\\" + Wide(key) + L".png";
    auto bitmap = std::make_unique<Gdiplus::Bitmap>(path.c_str());
    if (bitmap->GetLastStatus() != Gdiplus::Ok) bitmap.reset();
    Gdiplus::Bitmap* raw = bitmap.get();
    images.emplace(key, std::move(bitmap));
    return raw;
}

// Background art: the picture's frame is drawn at its own pixel size and only the middle is stretched, so the
// frame stays thin at any window size. The picture's header band is stretched so its bottom line lands at
// `headerLineY`, between the tabs and the content. A dark tint keeps the text readable.
void OverlayState::DrawBackground(Gdiplus::Graphics& graphics, RECT client, int headerLineY) {
    Gdiplus::Bitmap* bitmap = FindImage("background");
    const int areaWidth = client.right - client.left;
    const int areaHeight = client.bottom - client.top;
    if (bitmap == nullptr || areaWidth <= 0 || areaHeight <= 0) return;
    const int sourceWidth = static_cast<int>(bitmap->GetWidth());
    const int sourceHeight = static_cast<int>(bitmap->GetHeight());
    const int side = std::min(kFrameSlice, sourceWidth / 4);
    const int headerSource = std::min(kHeaderSlice, sourceHeight / 2);
    const int footer = side;
    if (sourceWidth <= 0 || sourceHeight <= 0 || areaWidth < 4 * side || areaHeight < headerSource / 2 + 3 * side) return;
    const int headerDest = std::clamp(headerLineY + 1, side, areaHeight - footer - side);

    const int sourceX[4] = {0, side, sourceWidth - side, sourceWidth};
    const int sourceY[4] = {0, headerSource, sourceHeight - footer, sourceHeight};
    const int destX[4] = {0, side, areaWidth - side, areaWidth};
    const int destY[4] = {0, headerDest, areaHeight - footer, areaHeight};

    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);  // no seams where the pieces meet
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            graphics.DrawImage(bitmap,
                               Gdiplus::Rect(destX[column], destY[row], destX[column + 1] - destX[column],
                                             destY[row + 1] - destY[row]),
                               sourceX[column], sourceY[row], sourceX[column + 1] - sourceX[column],
                               sourceY[row + 1] - sourceY[row], Gdiplus::UnitPixel, &attributes);
        }
    }
    Gdiplus::SolidBrush tint(Gdiplus::Color(150, 14, 14, 14));
    graphics.FillRectangle(&tint, 0, 0, areaWidth, areaHeight);
}

// Builds the background once for the current window size, so repaints only copy it.
void OverlayState::EnsureBackgroundCache(HDC target, int areaWidth, int areaHeight) {
    if (backgroundBitmap != nullptr && areaWidth == backgroundWidth && areaHeight == backgroundHeight &&
        layout.separatorY == backgroundSplit) return;
    ReleaseBackgroundCache();
    backgroundDc = CreateCompatibleDC(target);
    backgroundBitmap = CreateCompatibleBitmap(target, areaWidth, areaHeight);
    backgroundPrevious = SelectObject(backgroundDc, backgroundBitmap);
    const RECT rect{0, 0, areaWidth, areaHeight};
    FillColor(backgroundDc, rect, kBackground);
    {
        Gdiplus::Graphics graphics(backgroundDc);
        DrawBackground(graphics, rect, layout.separatorY);
    }
    backgroundSplit = layout.separatorY;
    backgroundWidth = areaWidth;
    backgroundHeight = areaHeight;
}

void OverlayState::ReleaseBackgroundCache() {
    if (backgroundDc != nullptr) {
        SelectObject(backgroundDc, backgroundPrevious);
        DeleteDC(backgroundDc);
        backgroundDc = nullptr;
    }
    if (backgroundBitmap != nullptr) {
        DeleteObject(backgroundBitmap);
        backgroundBitmap = nullptr;
    }
    backgroundWidth = 0;
    backgroundHeight = 0;
}

// Display size of some mythical art, as a share of its fitted size (1.0 = unchanged).
static double ImageDisplayScale(const std::string& key) {
    if (key == "mythical_rathmaseternalskull") return 0.35;  // reduced 65%
    if (key == "mythical_eternalhourglassofthetimelesssands") return 0.70;  // reduced 30%
    if (key == "mythical_baalsvilewhisper") return 0.75;  // reduced 25%
    // Mythical rings, amulets and the Ember (jewel) of Creation: reduced 35%
    if (key == "mythical_mythicalhighlordswrath" || key == "mythical_thetiesthatbind" || key == "mythical_vesselofsouls" ||
        key == "mythical_emberofcreation" || key == "mythical_lichkingsessence") return 0.65;
    return 1.0;
}

void OverlayState::DrawImageKey(Gdiplus::Graphics& graphics, const std::string& key, RECT box) {
    Gdiplus::Bitmap* bitmap = FindImage(key);
    if (bitmap == nullptr) return;
    const double imageWidth = bitmap->GetWidth();
    const double imageHeight = bitmap->GetHeight();
    if (imageWidth <= 0 || imageHeight <= 0) return;
    const double boxWidth = box.right - box.left;
    const double boxHeight = box.bottom - box.top;
    const double scale = std::min(boxWidth / imageWidth, boxHeight / imageHeight) * ImageDisplayScale(key);
    const int drawWidth = static_cast<int>(imageWidth * scale);
    const int drawHeight = static_cast<int>(imageHeight * scale);
    const int left = box.left + (static_cast<int>(boxWidth) - drawWidth) / 2;
    const int top = box.top + (static_cast<int>(boxHeight) - drawHeight) / 2;
    graphics.DrawImage(bitmap, left, top, drawWidth, drawHeight);
}

// Moves the selection to the next or previous matching entry, wrapping at either end.
void OverlayState::StepMatch(int direction) {
    if (query.empty() || totalMatches == 0) return;
    // First the matches inside the entry being viewed; past the last one, the next entry with matches.
    if (StepWithinEntry(direction)) {
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    PageView& view = CurrentView();
    if (direction > 0 && view.selected + 1 < filtered.size()) {
        SelectFiltered(view.selected + 1);
    } else if (direction < 0 && view.selected > 0) {
        SelectFiltered(view.selected - 1);
    } else {
        // Past the end of this tab: go to the next tab with matches, wrapping around the panel.
        const std::size_t pages = database->pages.size();
        for (std::size_t step = 1; step <= pages; ++step) {
            const std::size_t index = direction > 0 ? (pageIndex + step) % pages : (pageIndex + pages - step) % pages;
            if (tabMatches[index] == 0) continue;
            if (index == pageIndex) {
                SelectFiltered(direction > 0 ? 0 : filtered.size() - 1);
            } else {
                SetPage(index);  // Refilter selects the first match on the new tab
                if (direction < 0) SelectFiltered(filtered.size() - 1);
            }
            break;
        }
    }
    InvalidateRect(window, nullptr, FALSE);
}

// Moves the open entry to its next matching block in the direction given, scrolling that block to the top of the
// view. False when no match is left that way, so the caller moves on to another entry.
bool OverlayState::StepWithinEntry(int direction) {
    if (SkillsActive()) return SkillsStepMatch(direction);  // the next matching skill of the class (overlay_skills.inc)
    PageView& view = CurrentView();
    const int current = view.detailScroll;
    const int maxScroll = std::max(0, scrollTotal - scrollVisible);
    bool found = false;
    int target = 0;
    for (const int offset : matchTops) {
        const int scroll = std::min(offset, maxScroll);  // the last matches cannot scroll past the end
        if (direction > 0 && scroll > current && (!found || scroll < target)) {
            target = scroll;
            found = true;
        } else if (direction < 0 && scroll < current && (!found || scroll > target)) {
            target = scroll;
            found = true;
        }
    }
    if (!found) return false;
    view.detailScroll = target;
    return true;
}

// The search box passes Enter to the overlay as the next match, and Shift+Enter as the previous (while the suggestions
// are open the message loop gives Enter to them first, see SuggestKey).
WNDPROC gSearchProc = nullptr;

// The grey hint in the empty search box. Drawn here rather than with EM_SETCUEBANNER, which needs version 6 of the common
// controls (the game process does not load them).
void DrawSearchPlaceholder(HWND hwnd) {
    if (GetWindowTextLengthW(hwnd) != 0) return;
    const std::wstring hint = UiText(L"Search names, stats, runes\u2026 (in English)");
    RECT client{};
    GetClientRect(hwnd, &client);
    const HDC dc = GetDC(hwnd);
    if (dc == nullptr) return;
    HideCaret(hwnd);
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    const HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(128, 114, 92));
    client.left += 2;  // past the caret
    DrawTextW(dc, hint.c_str(), static_cast<int>(hint.size()), &client, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (previous != nullptr) SelectObject(dc, previous);
    ReleaseDC(hwnd, dc);
    ShowCaret(hwnd);
}

LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_LBUTTONDOWN) {  // a click on the search box with the controller cursor opens the on-screen keyboard
        auto* owner = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
        if (owner != nullptr && owner->padActive) PostMessageW(GetParent(hwnd), kMsgOpenOsk, 0, 0);
        else if (owner != nullptr) owner->ReopenSuggest();  // a click on the box brings the suggestions back
    }
    if (message == WM_PAINT) {
        const LRESULT result = CallWindowProcW(gSearchProc, hwnd, message, wParam, lParam);
        DrawSearchPlaceholder(hwnd);
        return result;
    }
    if ((message == WM_KEYDOWN || message == WM_CHAR) && wParam == VK_TAB) return 0;  // Tab picks suggestions, never a tab character
    if ((message == WM_KEYDOWN || message == WM_CHAR) && wParam == VK_RETURN) {
        if (message == WM_KEYDOWN) {
            auto* self = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
            if (self && self->CalcActive() && self->calcNumFocus >= 0) {
                self->calcNumFocus = -1;  // Enter finishes the number being typed
                InvalidateRect(self->window, nullptr, FALSE);
            } else if (self) {
                self->StepMatch((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            }
        }
        return 0;
    }
    if (message == WM_CHAR) {
        // A number field of the IAS calculator page takes the typing instead of the search box.
        auto* self = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
        if (self != nullptr && self->CalcChar(static_cast<wchar_t>(wParam))) return 0;
    }
    return CallWindowProcW(gSearchProc, hwnd, message, wParam, lParam);
}

void OverlayState::Refilter() {
    wchar_t buffer[256] = {};
    GetWindowTextW(search, buffer, 255);
    const bool wasSearching = !query.empty();
    const PageView& before = CurrentView();
    const std::size_t selectedEntry = before.selected < filtered.size() ? filtered[before.selected] : 0;
    query = Narrow(buffer);
    if (!query.empty()) {
        hasPendingSelect = false;
        if (!wasSearching) {  // a new search begins where the user is looking
            searchAnchor = selectedEntry;
            searchAnchorPage = pageIndex;
        }
    } else if (wasSearching) {
        // Clearing the search keeps the tab and the entry being viewed in the search results.
        hasPendingSelect = true;
        pendingPage = pageIndex;
        pendingSelect = selectedEntry;
    }
    SetHighlight(query);
    filtered = FilterPage(pageIndex);
    pagesDirty = true;
    tabMatches.assign(database->pages.size(), 0);
    totalMatches = 0;
    for (std::size_t i = 0; i < database->pages.size(); ++i) {
        tabMatches[i] = i == pageIndex ? filtered.size() : FilterPage(i).size();
        totalMatches += tabMatches[i];
    }
    PageView& view = CurrentView();
    view.selected = 0;
    view.listPage = 0;
    view.detailScroll = 0;
    // While searching on the tab the search began on, select the first match at or after the entry that was being
    // viewed (wrapping to the first match when there is none after it).
    if (!query.empty() && searchAnchorPage == pageIndex && !filtered.empty()) {
        const auto at = std::find_if(filtered.begin(), filtered.end(),
                                     [this](std::size_t entry) { return entry >= searchAnchor; });
        view.selected = at == filtered.end() ? 0 : static_cast<std::size_t>(at - filtered.begin());
        view.listPage = PageOf(view.selected);
    }
    // Keeps the viewed entry selected once the search is cleared. Applied on every refilter until
    // the search is typed in again, so a second refilter cannot reset it.
    if (query.empty() && hasPendingSelect && pendingPage == pageIndex && !filtered.empty()) {
        view.selected = std::min(pendingSelect, filtered.size() - 1);
        view.listPage = PageOf(view.selected);
    }
}

void OverlayState::SetPage(std::size_t index) {
    if (index >= database->pages.size() || index == pageIndex) return;
    pageIndex = index;
    calcOpen = -1;
    calcNumFocus = -1;
    specHoverTab = specHoverPerk = -1;
    campaignHoverKind = -1;
    campaignHover = 0;
    skillsHoverKind = -1;
    suggestOpen = false;
    Refilter();
}

// Splits the filtered entries into list pages. A row is at least the usual height and grows to fit its
// wrapped title; a page holds the rows that fit above the paging buttons.
void OverlayState::EnsurePages() {
    if (!pagesDirty) return;
    pagesDirty = false;
    const int baseHeight = std::max(S(48), boldLine * 2 + S(10));
    rowHeights.assign(filtered.size(), baseHeight);
    HDC dc = GetDC(window);
    for (std::size_t i = 0; i < filtered.size(); ++i) {
        const Entry& entry = CurrentPage().entries[filtered[i]];
        const int titleHeight = MeasureHeight(dc, UiDataText(Wide(entry.title)), listTextWidth, fontBold);
        rowHeights[i] = std::max(baseHeight, titleHeight + S(8));
    }
    ReleaseDC(window, dc);
    pageStarts.assign(1, 0);
    int y = listTopY;
    for (std::size_t i = 0; i < filtered.size(); ++i) {
        if (y != listTopY && y + rowHeights[i] > listBottomY) {
            pageStarts.push_back(i);
            y = listTopY;
        }
        y += rowHeights[i] + listRowGap;
    }
}

std::size_t OverlayState::PageOf(std::size_t index) {
    EnsurePages();
    const auto after = std::upper_bound(pageStarts.begin(), pageStarts.end(), index);
    return static_cast<std::size_t>(after - pageStarts.begin()) - 1;
}

// The rectangles of the rows on one list page, top to bottom.
std::vector<RECT> OverlayState::PageRects(std::size_t page) {
    EnsurePages();
    std::vector<RECT> rects;
    const std::size_t first = pageStarts[page];
    const std::size_t last = page + 1 < pageStarts.size() ? pageStarts[page + 1] : filtered.size();
    int y = listTopY;
    for (std::size_t i = first; i < last; ++i) {
        rects.push_back(Rect(listLeftX, y, listRightX, y + rowHeights[i]));
        y += rowHeights[i] + listRowGap;
    }
    return rects;
}

void OverlayState::SelectFiltered(std::size_t index) {
    if (index >= filtered.size()) return;
    PageView& view = CurrentView();
    view.selected = index;
    view.listPage = PageOf(index);
    view.detailScroll = 0;
}

void OverlayState::MovePage(int direction) {
    if (filtered.empty()) return;
    PageView& view = CurrentView();
    EnsurePages();
    const auto pages = pageStarts.size();
    const auto current = view.listPage;
    if (direction < 0 && current == 0) return;
    if (direction > 0 && current + 1 >= pages) return;
    view.listPage = direction < 0 ? current - 1 : current + 1;
    view.selected = pageStarts[view.listPage];
    view.detailScroll = 0;
}

void OverlayState::Show() {
    if (handingOff) {  // switched back before the handoff finished
        KillTimer(window, kTimerHandoff);
        handingOff = false;
        ReleaseHandoffBlocker();
    }
    open = true;
    if (gHandoffForeground != nullptr) {
        previousForeground = gHandoffForeground;
        gHandoffForeground = nullptr;
    } else {
        previousForeground = GetForegroundWindow();
    }
    FitToGame();
    SyncVisibility();
    if (IsWindowVisible(window)) {
        SetForegroundWindow(window);
        SetFocus(search);
    }
    InvalidateRect(window, nullptr, FALSE);
    if (IsWindowVisible(window)) {
        UpdateWindow(window);  // painted before the outgoing panel is allowed to hide
        gHandoffReady = true;
    }
    PadSync();
}

// Called after the other panel has had time to appear: hides this panel and its blocker.
// Polls until the incoming panel reports it is painted, waits one more tick so the compositor has
// presented it, then lets this panel go. A timeout keeps a failed handoff from leaving a stuck panel.
void OverlayState::HandoffTick() {
    if (!handingOff) {
        KillTimer(window, kTimerHandoff);
        return;
    }
    const bool timedOut = GetTickCount64() - handoffStarted > kHandoffTimeoutMs;
    if (!timedOut) {
        if (!gHandoffReady.load()) return;
        if (!handoffSettled) {
            handoffSettled = true;
            return;
        }
    }
    FinishHandoff();
}

void OverlayState::FinishHandoff() {
    KillTimer(window, kTimerHandoff);
    if (!handingOff) return;
    handingOff = false;
    ReleaseHandoffBlocker();
    ShowBlocker(false);
    ShowWindow(window, SW_HIDE);
    firstPainted = false;
    PadSync();
}

void OverlayState::Hide() {
    wheelRemainder = 0;
    if (handingOff) {
        KillTimer(window, kTimerHandoff);
        handingOff = false;
        ReleaseHandoffBlocker();
    }
    open = false;
    firstPainted = false;
    EndDrag();
    ClearHover();
    suggestOpen = false;
    ShowBlocker(false);
    ShowWindow(window, SW_HIDE);
    PadSync();
    if (previousForeground != nullptr && previousForeground != window && IsWindow(previousForeground)) {
        SetForegroundWindow(previousForeground);
    }
}

// Swaps in the database handed over by Overlay::ReplaceDatabase. Runs on the UI thread only, so nothing
// is drawing while the pointers change. Everything that points into the old data is dropped.
void OverlayState::ApplyPendingDatabase() {
    std::unique_ptr<Database> next;
    {
        std::lock_guard<std::mutex> guard(pendingLock);
        next = std::move(pendingDb);
    }
    if (!next || next->pages.empty()) return;
    EndDrag();
    ClearHover();
    detailHits.clear();
    cardHits.clear();
    tableHeightCache.clear();
    arrangeKey = ArrangeKey{};
    ownedDb = std::move(next);
    database = ownedDb.get();
    PrepareSkillsPage();  // the Skills page is not in the wiki data: it is made from the skills data (overlay_skills.inc)
    searchIndexFor = nullptr;  // the index belongs to the old data
    const bool suggestWasOpen = suggestOpen;
    suggest.clear();
    suggestQuery.clear();
    suggestOpen = false;
    suggestSel = suggestHover = -1;
    jumpPending = false;
    flashBlock = -1;
    specFlashPerk = -1;
    specMatchValid = false;
    specIndexedFor = nullptr;  // the perk records name the old data's page
    campaignMatchValid = false;
    campaignIndexedFor = nullptr;  // the area records name the old data's page and entry
    campaignHits.clear();
    campaignSpots.clear();
    campaignHoverKind = -1;
    campaignHover = 0;
    views.assign(database->pages.size(), PageView{});
    if (pageIndex >= database->pages.size()) pageIndex = 0;
    hasPendingSelect = false;
    FitToGame();  // the tab layout depends on the pages
    Refilter();
    EnsureSearchIndex();  // built now, not on the next key typed
    if (!query.empty()) {
        // The old suggestions point into the old data: look the text up again in the new one, and keep the list showing
        // if it was.
        UpdateSuggest();
        if (!suggestWasOpen) suggestOpen = false;
    }
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::Toggle() {
    if (open) Hide();
    else Show();
}

void OverlayState::ShowCursorWindow(bool show) {
    PostMessageW(cursorWindow, show ? kMsgCursorShow : kMsgCursorHide, 0, 0);
}

void OverlayState::ShowBlocker(bool show) {
    ShowCursorWindow(show);
    if (!IsWindow(blocker)) return;
    if (!show) {
        ShowWindow(blocker, SW_HIDE);
        return;
    }
    if (IsWindow(gameWindow) && GetWindow(blocker, GW_OWNER) != gameWindow)
        SetWindowLongPtrW(blocker, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(gameWindow));
    PlaceBlocker(blocker, gameWindow);
    // Shown fully transparent and then faded to its strength, so the first frame is never opaque black.
    SetLayeredWindowAttributes(blocker, 0, 0, LWA_ALPHA);
    SetWindowPos(blocker, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (gHandoffActive) gHandoffBlocker = blocker;  // raised when the outgoing panel lets go
    else SetLayeredWindowAttributes(blocker, 0, kBlockerAlpha, LWA_ALPHA);
}

// Runs on a timer. The overlay is owned by the game window, so it stays above the
// game without being topmost. While it is open and the game is active, the
// blocker covers the screen and the panel holds keyboard focus. It is hidden
// whenever another app is in focus, and it follows the game when the game moves.
void OverlayState::SyncVisibility() {
    if (!IsWindow(window)) return;
    if (!IsWindow(gameWindow)) gameWindow = FindGameWindow(window);
    bool gameActive = false;
    if (IsWindow(gameWindow)) {
        DWORD foregroundProcess = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
        gameActive = foregroundProcess == GetCurrentProcessId();
        if (GetWindow(window, GW_OWNER) != gameWindow)
            SetWindowLongPtrW(window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(gameWindow));
    }
    if (handingOff) return;  // the other panel is taking over; FinishHandoff hides this one
    const bool shouldShow = open && gameActive;
    const bool shown = IsWindowVisible(window) != FALSE;
    if (!shouldShow) {
        if (shown) {
            EndDrag();
            ClearHover();
            ShowBlocker(false);
            ShowWindow(window, SW_HIDE);
            firstPainted = false;
            PadSync();
        }
        return;
    }
    if (!shown) {
        FitToGame();
        ShowBlocker(true);
        // Shown fully transparent, painted once, then made opaque: no unpainted frame is ever visible.
        SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
        SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(window, nullptr, FALSE);
        UpdateWindow(window);
        SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
    } else if (IsWindow(gameWindow)) {
        const RECT game = GameScreenRect(gameWindow);
        if (game.left != fittedGameRect.left || game.top != fittedGameRect.top ||
            game.right != fittedGameRect.right || game.bottom != fittedGameRect.bottom) {
            FitToGame();
            InvalidateRect(window, nullptr, FALSE);
        }
    }
    // Keep keyboard focus here, so the game does not receive keys while the overlay is open.
    const HWND foreground = GetForegroundWindow();
    if (foreground != window && foreground != blocker) SetForegroundWindow(window);
    PadSync();
}

void OverlayState::EndDrag() {
    if (skillsBarDragging >= 0) {
        skillsBarDragging = -1;
        ReleaseCapture();
    }
    if (!dragging) return;
    dragging = false;
    ReleaseCapture();
}

void OverlayState::HandleMove(int x, int y) {
    if (!trackingMouse) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = window;
        TrackMouseEvent(&track);
        trackingMouse = true;
    }
    if (skillsBarDragging >= 0) {
        SkillsBarDrag(y);
        return;
    }
    if (dragging) {
        const int trackHeight = layout.scrollTrack.bottom - layout.scrollTrack.top;
        const int thumbHeight = layout.scrollThumb.bottom - layout.scrollThumb.top;
        const int span = trackHeight - thumbHeight;
        const int maxScroll = std::max(0, scrollTotal - scrollVisible);
        if (span > 0) {
            const long long offset = static_cast<long long>(y - dragStartY) * maxScroll / span;
            const long long target = std::clamp<long long>(dragStartScroll + offset, 0, maxScroll);
            CurrentView().detailScroll = static_cast<int>(target);
            InvalidateRect(window, nullptr, FALSE);
        }
        return;
    }
    if (SuggestMove(x, y)) return;
    if (!Inside(layout.detail, x, y)) {
        ClearHover();
        return;
    }
    {
        const int hotLink = LinkAt(x, y);
        if (hotLink != linkHot) {
            linkHot = hotLink;
            InvalidateRect(window, nullptr, FALSE);
        }
    }
    if (SpecMove(x, y)) return;  // the Specializations page has its own hover
    if (CampaignMove(x, y)) return;  // so has the Campaign Progression map
    if (SkillsMove(x, y)) return;    // and the Skills page (overlay_skills.inc)
    for (const DetailHit& hit : detailHits) {
        if (!Inside(hit.rect, x, y)) continue;
        // Same target: keep the tooltip where it is and do not repaint on every pixel of movement.
        if (hover.entry == hit.entry && hover.block == hit.block && hover.rune == hit.rune) return;
        hover.entry = hit.entry;
        hover.block = hit.block;
        hover.rune = hit.rune;
        hover.at = POINT{x, y};
        hover.target = hit.rect;
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    ClearHover();
}

void OverlayState::ClearHover() {
    if (specHoverTab >= 0 || specHoverPerk >= 0) {
        specHoverTab = specHoverPerk = -1;
        InvalidateRect(window, nullptr, FALSE);
    }
    if (campaignHoverKind >= 0) {
        campaignHoverKind = -1;
        campaignHover = 0;
        InvalidateRect(window, nullptr, FALSE);
    }
    if (skillsHoverKind >= 0) {
        skillsHoverKind = -1;
        InvalidateRect(window, nullptr, FALSE);
    }
    if (hover.block == nullptr) return;
    hover = Hover{};
    InvalidateRect(window, nullptr, FALSE);
}

// Tooltip for an item block (all of its fields and lines), or for a rune icon.
void OverlayState::DrawTooltip(HDC dc) {
    if (hover.block == nullptr) return;
    const Block& block = *hover.block;

    struct TipRow { std::wstring text; COLORREF color; HFONT font; };
    std::vector<TipRow> rows;
    if (hover.rune >= 0 && static_cast<std::size_t>(hover.rune) < block.runes.size()) {
        // Gem and jewel sockets show only "Any Gem" or "Any Jewel". Other runes show their name.
        const std::wstring socket = Wide(block.runes[hover.rune].label);
        if (socket.rfind(L"Any ", 0) == 0) {
            rows.push_back({UiText(socket), kGold, fontBold});
        } else {
            rows.push_back({UiFormat(L"{0} Rune", {socket}), block.runes[hover.rune].lod ? CodeColor('8') : kGold, fontBold});
            // The rune's own stat lines for this kind of item (Runes page), instead of a "Socketed in" line.
            const RuneIcon& rune = block.runes[hover.rune];
            for (const Line& line : RuneBonusLines(*database, rune, *hover.entry, block)) {
                std::string plain;
                COLORREF color = kBody;
                PlainText(line.text, plain, color, kBody);
                rows.push_back({Wide(plain), color, fontBody});
            }
        }
    } else {
        const std::string heading = block.title.empty() ? hover.entry->title : block.title;
        // Mythicals (the wide cards) are purple, set items green; the rest follow the page's name colour.
        COLORREF headingColor = tooltipNameColor;
        if (hover.entry->wide) headingColor = CodeColor(';');
        else if (hover.entry->title == "LoD Runes") headingColor = CodeColor('8');
        else if (CurrentPage().id == "set-items") headingColor = CodeColor('2');
        rows.push_back({Wide(heading), headingColor, fontBold});
        if (!block.subtitle.empty()) rows.push_back({Wide(block.subtitle), kDim, fontSmall});
        for (const Field& field : block.fields) {
            // A value of several entries (the data separates them with \x1e, e.g. one Automod entry per tier) is shown
            // one entry per line, the continuation lines indented under the label.
            const std::wstring label = UiDataText(Wide(field.label)) + L": ";
            std::size_t start = 0;
            bool first = true;
            while (true) {
                const std::size_t cut = field.value.find('\x1e', start);
                std::string piece = field.value.substr(start, cut == std::string::npos ? std::string::npos : cut - start);
                while (!piece.empty() && piece.back() == ' ') piece.pop_back();
                if (!piece.empty() || first)
                    rows.push_back({(first ? label : std::wstring(L"    ")) + Wide(piece), kBody, fontBody});
                first = false;
                if (cut == std::string::npos) break;
                start = cut + 1;
            }
        }
        for (const Line& line : block.lines) {
            std::string plain;
            COLORREF color = kBody;
            PlainText(line.text, plain, color, kBody);
            const bool isHeading = line.kind == LineKind::Heading;
            const bool isLabel = line.kind == LineKind::Label;  // a small dim sub-heading (socketable bonus group)
            rows.push_back({Wide(plain), isHeading ? kGold : (isLabel ? kDim : color),
                            isHeading ? fontBold : (isLabel ? fontSmall : fontBody)});
        }
    }

    // A LoD rune's name must not take the Kanji rune colour ("Ko" exists in both sets), so name
    // colouring is off while this tip is measured and drawn, and put back afterwards.
    struct KanjiColorGuard {
        bool previous;
        explicit KanjiColorGuard(bool value) : previous(gKanjiRuneColors) { gKanjiRuneColors = value; }
        ~KanjiColorGuard() { gKanjiRuneColors = previous; }
    } kanjiGuard(!(hover.rune >= 0 && static_cast<std::size_t>(hover.rune) < block.runes.size() &&
                   block.runes[hover.rune].lod));

    struct LodNamesGuard {
        std::vector<std::wstring> previous;
        explicit LodNamesGuard(std::vector<std::wstring> names) : previous(std::move(gLodRuneNames)) {
            gLodRuneNames = std::move(names);
        }
        ~LodNamesGuard() { gLodRuneNames = std::move(previous); }
    };
    std::vector<std::wstring> lodNames;
    for (const RuneIcon& rune : block.runes) {
        if (rune.lod && rune.label.rfind("Any ", 0) != 0) lodNames.push_back(Wide(rune.label));
    }
    LodNamesGuard lodGuard(std::move(lodNames));

    const int gap = S(4);
    RECT client{};
    GetClientRect(window, &client);
    // Start at the normal width and widen only when the text would run past the bottom of the
    // window (long weapon gemwords), so most tips keep their usual shape.
    // A tooltip stays below the search box, its buttons and the tabs: those are windows or controls drawn over the tip.
    int minTop = S(8);
    for (const RECT* above : {&layout.search, &layout.clearQuery, &layout.matchPrev, &layout.matchNext})
        minTop = std::max(minTop, static_cast<int>(above->bottom) + S(6));
    const int availableHeight = static_cast<int>(client.bottom) - minTop - S(8);
    const int maxTipWidth = std::max(S(340), static_cast<int>(client.right) - S(16));
    int tipWidth = S(340);
    std::vector<int> heights;
    int totalHeight = S(16);
    // Mythical cards (wide entries) start at the wider size.
    // The tip is as wide as its longest line needs (within the window), so it has no empty margin. Wider sizes
    // follow only if the text does not fit the bottom of the window at that width.
    int widest = 0;
    for (const TipRow& row : rows) widest = std::max(widest, NaturalWidth(dc, row.text, row.font));
    // A few pixels of spare width: text drawn in pieces (a search match, colour codes) can come out slightly wider than
    // DrawText measured it, and with no spare room its last word wrapped onto a line the tip had no height for.
    // No wider than 640 (scaled): a very long line wraps instead of stretching the tip across the whole window.
    const int fitWidth = std::min(std::max(widest + S(24) + S(8), S(160)), std::min(maxTipWidth, S(640)));
    std::vector<int> widths{fitWidth};
    for (const int candidate : {S(340), S(420), S(520), S(640)}) {
        if (candidate > fitWidth) widths.push_back(candidate);
    }
    for (const int candidate : widths) {
        tipWidth = std::min(candidate, maxTipWidth);
        const int inner = tipWidth - S(24);
        heights.clear();
        totalHeight = S(16);
        for (const TipRow& row : rows) {
            const int rowHeight = MeasureHeight(dc, row.text, inner, row.font);
            heights.push_back(rowHeight);
            totalHeight += rowHeight + gap;
        }
        if (totalHeight <= availableHeight) break;
    }

    // Rows are placed in one column unless even the widest tip is taller than the window (a long mythical): then
    // the rows flow into two to four columns, each row whole, so nothing is cut off at the bottom.
    // columnWidth is the width of the text in one column; columns are separated by one padding unit (S(12)) and
    // the tip has one padding unit at each outside edge, the same as a single column.
    int columnWidth = tipWidth - S(24);
    std::vector<int> rowColumn(rows.size(), 0);
    std::vector<int> rowY(rows.size(), 0);  // from the top of the tip's text area
    {
        int y = 0;
        for (std::size_t i = 0; i < rows.size(); ++i) { rowY[i] = y; y += heights[i] + gap; }
    }
    if (totalHeight > availableHeight && !rows.empty()) {
        for (int columns = 2; columns <= 4; ++columns) {
            const int spare = maxTipWidth - S(24) - (columns - 1) * S(12);
            columnWidth = std::min({S(420), spare / columns, widest});
            const int inner = columnWidth;
            int column = 0, y = 0, tallest = 0;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                heights[i] = MeasureHeight(dc, rows[i].text, inner, rows[i].font);
                if (y > 0 && y + heights[i] + S(16) > availableHeight) { column++; y = 0; }
                rowColumn[i] = column;
                rowY[i] = y;
                y += heights[i] + gap;
                tallest = std::max(tallest, y);
            }
            tipWidth = S(24) + columns * columnWidth + (columns - 1) * S(12);
            totalHeight = tallest + S(16);
            if (column < columns) break;  // everything fits in this many columns
        }
    }

    // The tooltip sits beside the thing hovered, not at the pointer, and clear of the drawn cursor: the
    // cursor reaches right and down from its hot spot, so the tip starts past the target's edge plus the
    // cursor's width. A pointer anywhere on the target can then never cover the tooltip.
    const RECT& target = hover.target;
    const int cursorRight = std::max(gCursorDrawWidth.load() - gCursorHotX.load(), 0) + S(6);
    const int cursorLeft = gCursorHotX.load() + S(6);
    int left = target.right + cursorRight;
    if (left + tipWidth > client.right - S(8)) {
        const int leftSide = target.left - cursorLeft - tipWidth;
        left = leftSide >= S(8) ? leftSide : std::max(S(8), static_cast<int>(client.right) - S(8) - tipWidth);
    }
    int top = target.top;
    if (top + totalHeight > client.bottom - S(8)) top = client.bottom - totalHeight - S(8);
    top = std::max(minTop, top);

    // Long items are clipped to the window rather than pushed off screen.
    const int visibleHeight = std::min(totalHeight, static_cast<int>(client.bottom) - top - S(8));
    const RECT box = Rect(left, top, left + tipWidth, top + visibleHeight);
    // Tooltips sit on a see-through black so the wiki behind them still shows.
    CornerShadow(dc, box, S(2));
    FillPanel(dc, box, RGB(10, 7, 4), 214);
    FrameColor(dc, box, kGold);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, box.left, box.top, box.right, box.bottom);
    int y = top + S(8);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const int columnLeft = left + S(12) + rowColumn[i] * (columnWidth + S(12));
        const int rowTop = y + rowY[i];
        const RECT lineRect = Rect(columnLeft, rowTop, columnLeft + columnWidth, rowTop + heights[i]);
        Text(dc, rows[i].text, lineRect, rows[i].color, rows[i].font, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
    RestoreDC(dc, saved);
}

void OverlayState::QueryChanged() {
    const bool wasEmpty = query.empty();
    Refilter();
    ScheduleSuggest();  // the list is looked up once the typing stops (kTimerSuggest)
    // The box repaints whole when it empties or fills, so the placeholder comes back or goes completely.
    if (wasEmpty != query.empty() && IsWindow(search)) InvalidateRect(search, nullptr, TRUE);
    InvalidateRect(window, nullptr, FALSE);
}

// A filter or sort button was pressed, by the mouse or the controller.
void OverlayState::ApplyChip(const Chip& chip) {
    PageView& view = CurrentView();
    switch (chip.kind) {
        case ChipKind::Socket: view.socketFilter = chip.value; break;
        case ChipKind::Sort: {
            const BlockSort picked = static_cast<BlockSort>(chip.value);
            view.sort = (IsRowSort(picked) && view.sort == picked) ? BlockSort::Default : picked;  // again: back to names
            break;
        }
        case ChipKind::Tier: view.tierFilter = chip.text; break;
        case ChipKind::Family: view.familyFilter = chip.text; break;
    }
    view.detailScroll = 0;
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::HandleClick(int x, int y) {
    PageView& view = CurrentView();
    if (SuggestClick(x, y)) return;  // the suggestions are on top of everything else
    if (oskOpen) {
        if (OskClick(x, y)) return;
        if (!Inside(layout.search, x, y)) CloseOsk();  // a click anywhere else puts the keyboard away
    }
    if (!query.empty() && Inside(layout.clearQuery, x, y)) {
        SetWindowTextW(search, L"");
        QueryChanged();
        SetFocus(search);
        return;
    }
    if (!query.empty() && Inside(layout.matchPrev, x, y)) {
        StepMatch(-1);
        return;
    }
    if (!query.empty() && Inside(layout.matchNext, x, y)) {
        StepMatch(1);
        return;
    }
    if (Inside(SearchBoxRect(layout), x, y)) {  // the frame round the text: focus the box and bring the suggestions back
        SetFocus(search);
        ReopenSuggest();
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (layout.scrollable && Inside(layout.scrollTrack, x, y)) {
        if (Inside(layout.scrollThumb, x, y)) {
            dragging = true;
            dragStartY = y;
            dragStartScroll = view.detailScroll;
            SetCapture(window);
        } else {
            view.detailScroll += (y < layout.scrollThumb.top) ? -scrollVisible : scrollVisible;
            view.detailScroll = std::clamp(view.detailScroll, 0, std::max(0, scrollTotal - scrollVisible));
        }
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (CalcClick(x, y)) return;
    if (SpecClick(x, y)) return;
    if (CampaignClick(x, y)) return;
    if (LinkClick(x, y)) return;
    if (SkillsClick(x, y)) return;
    for (const Chip& chip : layout.chips) {
        if (!Inside(chip.rect, x, y)) continue;
        ApplyChip(chip);
        return;
    }
    for (std::size_t i = 0; i < switches.size(); ++i) {
        if (Inside(layout.switchButtons[i], x, y)) {
            if (switches[i].current) return;
            if (switches[i].available && !switches[i].available()) return;
            BeginSwitch(switches[i].onSwitch);
            return;
        }
    }
    if (Inside(layout.close, x, y)) {
        Hide();
        return;
    }
    for (std::size_t i = 0; i < layout.tabs.size(); ++i) {
        if (Inside(layout.tabs[i], x, y)) {
            SetPage(i);
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
    }
    const std::vector<RECT> pageRows = PageRects(view.listPage);
    for (std::size_t i = 0; i < pageRows.size(); ++i) {
        if (!Inside(pageRows[i], x, y)) continue;
        SelectFiltered(pageStarts[view.listPage] + i);
        break;
    }
    if (Inside(layout.prev, x, y)) MovePage(-1);
    if (Inside(layout.next, x, y)) MovePage(1);
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::HandleWheel(int x, int y, int delta) {
    PageView& view = CurrentView();
    // Precision touchpads send many small deltas: they add up to whole notches instead of each rounding to none.
    if ((wheelRemainder > 0 && delta < 0) || (wheelRemainder < 0 && delta > 0)) wheelRemainder = 0;  // turned back
    wheelRemainder += delta;
    const int steps = wheelRemainder / WHEEL_DELTA;
    wheelRemainder -= steps * WHEEL_DELTA;
    if (steps == 0) return;
    if (SuggestWheel(x, y, steps)) return;
    if (SkillsWheel(x, y, steps)) return;  // the Skills page: a value under the pointer, or its own panels
    if (Inside(layout.detail, x, y)) {
        if (CalcWheel(x, y, steps)) return;
        view.detailScroll = std::max(0, view.detailScroll - steps * S(60));  // 60 px a notch (was 40, 50% faster)
        campaignHoverKind = -1;  // the area under a still pointer changed: no tooltip at the old place until the pointer moves
        campaignHover = 0;
    } else if (x < layout.detail.left && y >= layout.search.top && !filtered.empty()) {
        // Wheel down selects the next entry, wheel up the previous one.
        const long long target = static_cast<long long>(view.selected) - steps;
        const std::size_t last = filtered.size() - 1;
        SelectFiltered(static_cast<std::size_t>(std::clamp(target, 0LL, static_cast<long long>(last))));
    }
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::HandleKey(WPARAM key) {
    PageView& view = CurrentView();
    if (filtered.empty()) return;
    switch (key) {
    case VK_DOWN:
        SelectFiltered(std::min(view.selected + 1, filtered.size() - 1));
        break;
    case VK_UP:
        SelectFiltered(view.selected == 0 ? 0 : view.selected - 1);
        break;
    case VK_NEXT:
        MovePage(1);
        break;
    case VK_PRIOR:
        MovePage(-1);
        break;
    default:
        return;
    }
    InvalidateRect(window, nullptr, FALSE);
}

// ---- Controller ------------------------------------------------------------------------------------------------
// While the wiki is showing, the controller is polled every 16 ms. D-pad: highlight (tabs, then the list on the left);
// A: confirm; B: back; Y: on-screen keyboard; X: clear the search; LB/RB: previous/next tab; right stick: scroll;
// LT/RT: scroll faster; left stick: move the cursor (A then clicks it); R3: the other panel; Start: close.

void OverlayState::PadSync() {
    const bool want = IsWindow(window) && IsWindowVisible(window);
    if (want == padRunning) return;
    padRunning = want;
    if (want) {
        // controller.txt in the plugin folder: "playstation" or "xbox" forces the help picture (a PlayStation
        // controller used through Steam Input or DS4Windows looks like an Xbox one).
        esrwiki::pad::Kind forced = esrwiki::pad::Kind::None;
        std::ifstream file(dataDir + L"\\controller.txt");
        std::string word;
        if (file >> word) {
            for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (word.find("play") != std::string::npos || word.find("sony") != std::string::npos) forced = esrwiki::pad::Kind::PlayStation;
            else if (word.find("xbox") != std::string::npos) forced = esrwiki::pad::Kind::Xbox;
        }
        gamepad.SetKindOverride(forced);
        gamepad.Start();
        padRepeat.Reset();
        padLast = 0;
        const ULONGLONG switchedAt = gPadHandoffAt.exchange(0);
        SetTimer(window, kTimerPad, kPadInterval, nullptr);
        if (switchedAt != 0 && GetTickCount64() - switchedAt < 3000) {
            // Switched with the controller: this panel is in controller mode from its first frame and its help window
            // appears at once, in the same place as the outgoing panel's, so nothing flickers when that one hides.
            padActive = true;
            padKind = static_cast<esrwiki::pad::Kind>(gPadHandoffKind.load());
            padZone = PadZone::Tabs;
            tabFocus = pageIndex;
            padMouseUntil = 0;
            padLast = 0;
            padGraceUntil = GetTickCount64() + 1500;
            POINT here{};
            if (GetCursorPos(&here)) {
                drive.pointerX.store(here.x);
                drive.pointerY.store(here.y);
            }
            drive.hidden.store(false);
            PadModeChanged();
        }
    } else {
        KillTimer(window, kTimerPad);
        gamepad.Stop();
        CloseOsk();
        padActive = false;
        padMouseUntil = 0;
        drive.Stop();
        SyncHelp();
    }
}

// A real mouse or keyboard was used: the controller help goes away until the controller is used again.
void OverlayState::PadMouseUsed() {
    if (!padActive) return;
    padActive = false;
    suggestPadFocus = false;
    drive.Stop();  // the stick lets go of the pointer and the cursor comes back
    CloseOsk();
    PadModeChanged();
}

// The layout reserves room for the help picture while a controller is in use, so it is redone when that changes.
void OverlayState::PadModeChanged() {
    FitToGame();
    SyncHelp();
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::PadTick() {
    if (!IsWindowVisible(window)) return;
    using esrwiki::pad::Button;
    const ULONGLONG now = GetTickCount64();
    esrwiki::pad::State s;
    if (!gamepad.Poll(s, now)) {
        if (padActive && now < padGraceUntil) {  // just switched panels: the controller is still being opened
            padLast = now;
            return;
        }
        padKind = esrwiki::pad::Kind::None;
        drive.Stop();
        if (padActive) {
            padActive = false;
            PadSetSearchFocus(esrwiki::pad::SearchFocus::Page);  // the list focus (and its pick) goes with the pad
            CloseOsk();
            PadModeChanged();
        }
        padLast = now;
        return;
    }
    bool pressed[esrwiki::pad::kButtonCount];
    bool released[esrwiki::pad::kButtonCount];
    padRepeat.Step(s, now, pressed, released);
    const float dt = padLast == 0 ? 0.016f : std::min(0.1f, static_cast<float>(now - padLast) / 1000.0f);
    padLast = now;
    if (!padActive) {
        if (!esrwiki::pad::Active(s)) return;
        padActive = true;
        padKind = s.kind;
        padZone = PadZone::Tabs;
        tabFocus = pageIndex;
        padMouseUntil = 0;
        POINT here{};
        if (GetCursorPos(&here)) {
            drive.pointerX.store(here.x);
            drive.pointerY.store(here.y);
        }
        drive.hidden.store(false);
        PadModeChanged();
    } else if (s.kind != padKind) {
        padKind = s.kind;
        SyncHelp();
    }
    const auto press = [&](Button b) { return pressed[static_cast<int>(b)]; };
    // The directional buttons put the cursor away; the left stick or the mouse brings it back.
    if (press(Button::Left) || press(Button::Right) || press(Button::Up) || press(Button::Down)) drive.hidden.store(true);

    if (press(Button::Start)) {
        Hide();
        return;
    }
    bool changed = false;
    using esrwiki::pad::SearchEvent;
    using esrwiki::pad::SearchFocus;
    FlushSuggest();  // never act on a list that is about to be replaced
    if (suggestPadFocus && esrwiki::pad::FirstPickable(SuggestPickRows()) < 0) {  // the list went away (a tab change, a jump)
        PadSetSearchFocus(oskOpen ? SearchFocus::Keyboard : SearchFocus::Page);
        changed = true;
    }
    if (press(Button::Back) && PadSuggestShortcut()) {
        // View (Share / Create): into the search suggestions (they come back for the text typed if they were closed),
        // or out of them again.
        changed = true;
    } else if (suggestPadFocus) {
        // The suggestion list has the focus: up / down pick a row (the "Did you mean" heading is skipped), up from the first
        // row goes back to the keyboard (or the page), LB / RB turn a page of rows, A opens the row, B closes the list and Y
        // shows or hides the keyboard.
        const bool any = press(Button::Down) || press(Button::Up) || press(Button::RB) || press(Button::LB) ||
                         press(Button::A) || press(Button::B) || press(Button::Y);
        const int pageRows = std::max(1, suggestVisible);
        if (press(Button::Down)) {
            PadSuggestStep(1);
        } else if (press(Button::Up)) {
            PadSuggestStep(-1, padRepeat.IsRepeat(Button::Up));  // a held Up stops at the first row
        } else if (press(Button::RB)) {
            PadSuggestStep(pageRows);
        } else if (press(Button::LB)) {
            PadSuggestStep(-pageRows);
        } else if (press(Button::A)) {
            if (now < padMouseUntil) {
                PadClickAtCursor();
            } else if (suggestSel >= 0 && static_cast<std::size_t>(suggestSel) < suggest.size()) {
                const esrwiki::search::Hit hit = suggest[static_cast<std::size_t>(suggestSel)];  // copied: the jump rebuilds the list
                suggestPadFocus = esrwiki::pad::NextSearchFocus(SearchFocus::List, SearchEvent::Pick, PadFocusContext()) == SearchFocus::List;
                CloseOsk();
                JumpTo(hit);
            }
        } else if (press(Button::B)) {  // the list closes; the focus goes back to the keyboard or the page
            PadSetSearchFocus(esrwiki::pad::NextSearchFocus(SearchFocus::List, SearchEvent::Back, PadFocusContext()));
            CloseSuggest();
        } else if (press(Button::Y)) {
            const SearchFocus next = esrwiki::pad::NextSearchFocus(SearchFocus::List, SearchEvent::KeyboardToggle, PadFocusContext());
            if (oskOpen) CloseOsk();
            else OpenOsk();
            PadSetSearchFocus(next);
        }
        if (any) changed = true;
    } else if (oskOpen) {
        int dx = 0, dy = 0;
        if (press(Button::Left)) dx = -1;
        if (press(Button::Right)) dx = 1;
        if (press(Button::Up)) dy = -1;
        if (press(Button::Down)) dy = 1;
        // LB / RB open the previous / next tab, as with the keyboard put away (the suggestions have their own focus now:
        // D-pad down from the bottom row, or View).
        if (press(Button::LB)) { PadStepTab(-1); changed = true; }
        if (press(Button::RB)) { PadStepTab(1); changed = true; }
        if (dx != 0 || dy != 0) {
            if (suggestSel >= 0) { suggestSel = -1; changed = true; }
            padMouseUntil = 0;
            if (padCursorMoved) {  // start from the key under the cursor
                padCursorMoved = false;
                POINT at{};
                if (GetCursorPos(&at) && ScreenToClient(window, &at)) {
                    for (const OskHit& hit : oskHits) {
                        if (Inside(hit.rect, at.x, at.y)) {
                            oskRow = hit.row;
                            oskCol = hit.col;
                        }
                    }
                }
            }
            bool intoList = false;
            const bool bottomDown = dy > 0 && dx == 0 && oskRow == static_cast<int>(oskRows.size()) - 1;
            if (bottomDown && padRepeat.IsRepeat(Button::Down) && PadFocusContext().listPickable) {
                intoList = true;  // a held Down stops at the bottom row: only a fresh press crosses into the list
            } else if (bottomDown) {
                // Down from the bottom row: into the suggestions when they have a row to open (the list comes back for
                // the text typed if B closed it); otherwise round to the top row as before.
                if (!query.empty()) ReopenSuggest();
                const SearchFocus next = esrwiki::pad::NextSearchFocus(SearchFocus::Keyboard, SearchEvent::Down, PadFocusContext());
                if (next == SearchFocus::List) {
                    PadSetSearchFocus(next);
                    intoList = true;
                }
            }
            if (!intoList) esrwiki::pad::OskMove(oskRows, oskRow, oskCol, dx, dy);
            changed = true;
        }
        if (press(Button::A)) {
            if (now < padMouseUntil) {
                PadClickAtCursor();
            } else {
                PressOskKey(oskRow, oskCol);
            }
            changed = true;
        }
        if (press(Button::Y)) CloseOsk();
        if (press(Button::B)) {  // an empty search: B puts the keyboard away; otherwise it clears the search
            if (GetWindowTextLengthW(search) == 0) {
                CloseOsk();
            } else {
                SetWindowTextW(search, L"");
                QueryChanged();
            }
            changed = true;
        }
        if (press(Button::X)) {  // Square / X deletes one character
            SendMessageW(search, WM_CHAR, static_cast<WPARAM>(VK_BACK), 0);
            changed = true;
        }
    } else {
        if (press(Button::Left)) PadDirection(-1, 0);
        if (press(Button::Right)) PadDirection(1, 0);
        if (press(Button::Up)) PadDirection(0, -1);
        if (press(Button::Down)) PadDirection(0, 1);
        if (press(Button::A)) PadConfirm(now);
        if (press(Button::B)) PadBack();
        if (press(Button::Y)) OpenOsk();
        if (press(Button::X)) {
            if (!(padZone == PadZone::Calc && CalcPadClear())) {  // on a calculator number X sets it to 0
                SetWindowTextW(search, L"");
                QueryChanged();
            }
        }
        // LB / RB: the previous / next tab; on the Campaign Progression map, the map's tabs; on the Skills page's boxes, its trees.
        if (press(Button::LB) && !CampaignPadStepTab(-1) && !SkillsPadStepTab(-1)) PadStepTab(-1);
        if (press(Button::RB) && !CampaignPadStepTab(1) && !SkillsPadStepTab(1)) PadStepTab(1);
        if (press(Button::R3) && !switches.empty()) {
            const SwitchButton& other = switches[0];
            if (!other.current && (!other.available || other.available())) {
                BeginSwitch(other.onSwitch);
                return;
            }
        }
    }
    if (released[static_cast<int>(Button::A)] && dragging) EndDrag();
    // The help list follows the focus (page, keyboard, suggestions).
    const int helpContext = static_cast<int>(PadSearchFocus());
    if (helpContext != helpSyncedContext) {
        helpSyncedContext = helpContext;
        SyncHelp();
    }

    // Left stick: the cursor. The cursor thread moves the pointer (see CursorThread), a millisecond at a time and eased,
    // so it glides; this poll only hands the stick over. Speed grows with the push, so small pushes are precise.
    drive.speed.store(S(1300));
    drive.stickX.store(s.lx);
    drive.stickY.store(s.ly);
    drive.driving.store(true);
    if (s.lx != 0.0f || s.ly != 0.0f) {
        drive.hidden.store(false);  // moving the cursor brings it back after the D-pad put it away
        padMouseUntil = now + 2500;
        padCursorMoved = true;
    }

    // Right stick scrolls; the triggers scroll much faster (RT down, LT up).
    const auto trigger = [](float value) { return value < 0.12f ? 0.0f : (value - 0.12f) / 0.88f; };
    const double scrollSpeed = -static_cast<double>(s.ry) * S(700) + (trigger(s.rt) - trigger(s.lt)) * S(2600);
    if (scrollSpeed != 0.0) {
        padScrollCarry += scrollSpeed * dt;
        const int whole = static_cast<int>(padScrollCarry);
        padScrollCarry -= whole;
        if (whole != 0) {
            if (SkillsActive() && skillsWide) {
                // Split layout: the panels scroll on their own; the one the pad focus is in (the item list or the details).
                const SkillsHit* focus = SkillsPadHit();
                const bool inList = focus != nullptr && focus->zone == 0 && skillsCache.items;
                int& panelScroll = inList ? skillsListScroll : skillsDetailScroll;
                const int panelMax = inList ? skillsListMax : skillsDetailMax;
                const int nextPanel = std::clamp(panelScroll + whole, 0, panelMax);
                if (nextPanel != panelScroll) {
                    panelScroll = nextPanel;
                    changed = true;
                }
            } else {
                PageView& view = CurrentView();
                const int limit = std::max(0, scrollTotal - scrollVisible);
                const int next = std::clamp(view.detailScroll + whole, 0, limit);
                if (next != view.detailScroll) {
                    view.detailScroll = next;
                    changed = true;
                }
            }
        }
    }
    if (changed) InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::PadClickAtCursor() {
    POINT point{};
    if (!GetCursorPos(&point)) return;
    ScreenToClient(window, &point);
    HandleClick(point.x, point.y);
}

// LB / RB: the previous / next tab, opened at once.
void OverlayState::PadStepTab(int direction) {
    const std::size_t count = database->pages.size();
    if (count == 0) return;
    const std::size_t next = (pageIndex + count + static_cast<std::size_t>(direction + static_cast<int>(count))) % count;
    SetPage(next);
    tabFocus = next;
    InvalidateRect(window, nullptr, FALSE);
}

// Where the controller's focus is while searching (see esrwiki::pad::NextSearchFocus).
esrwiki::pad::SearchFocus OverlayState::PadSearchFocus() const {
    if (suggestPadFocus) return esrwiki::pad::SearchFocus::List;
    return oskOpen ? esrwiki::pad::SearchFocus::Keyboard : esrwiki::pad::SearchFocus::Page;
}

esrwiki::pad::FocusContext OverlayState::PadFocusContext() const {
    esrwiki::pad::FocusContext c;
    c.keyboardOpen = oskOpen;
    c.atKeyboardBottom = oskOpen && !oskRows.empty() && oskRow == static_cast<int>(oskRows.size()) - 1;
    const std::vector<bool> rows = SuggestPickRows();
    c.listPickable = esrwiki::pad::FirstPickable(rows) >= 0;
    const int current = suggestSel >= 0 ? SuggestDisplayRow(suggestSel) : -1;
    c.atListTop = esrwiki::pad::ListStep(rows, current, -1).leave;
    return c;
}

// Gives the suggestion list the focus (its first row picked), or takes the focus back from it to the keyboard (on its
// bottom row, next to the list) or the page.
void OverlayState::PadSetSearchFocus(esrwiki::pad::SearchFocus next) {
    const bool toList = next == esrwiki::pad::SearchFocus::List;
    if (toList && !suggestPadFocus) {
        suggestSel = SuggestHitOfRow(esrwiki::pad::FirstPickable(SuggestPickRows()));
        padMouseUntil = 0;  // A opens the row; it does not click at the cursor
    } else if (!toList && suggestPadFocus) {
        suggestSel = -1;
        if (oskOpen && !oskRows.empty()) {
            oskRow = static_cast<int>(oskRows.size()) - 1;
            oskCol = std::clamp(oskCol, 0, static_cast<int>(oskRows.back().size()) - 1);
        }
    }
    suggestPadFocus = toList;
    suggestHover = -1;
    SuggestReveal();
    InvalidateRect(window, nullptr, FALSE);
}

// D-pad up / down (one row) and LB / RB (a page of rows) in the suggestion list. Up from the first row leaves it.
void OverlayState::PadSuggestStep(int delta, bool repeat) {
    const std::vector<bool> rows = SuggestPickRows();
    const int current = suggestSel >= 0 ? SuggestDisplayRow(suggestSel) : -1;
    const esrwiki::pad::ListStepResult step = esrwiki::pad::ListStep(rows, current, delta);
    if (step.leave) {
        if (repeat) return;  // an auto-repeat does not carry the focus out of the list
        esrwiki::pad::FocusContext c = PadFocusContext();
        c.atListTop = true;
        PadSetSearchFocus(esrwiki::pad::NextSearchFocus(esrwiki::pad::SearchFocus::List, esrwiki::pad::SearchEvent::Up, c));
        return;
    }
    if (step.row >= 0) suggestSel = SuggestHitOfRow(step.row);
    suggestHover = -1;
    SuggestReveal();
    InvalidateRect(window, nullptr, FALSE);
}

// View (Share / Create): into the suggestion list for the text typed (it comes back if it was closed), or out of it.
// True when the focus moved.
bool OverlayState::PadSuggestShortcut() {
    if (!suggestPadFocus && !query.empty()) ReopenSuggest();
    const esrwiki::pad::SearchFocus next =
        esrwiki::pad::NextSearchFocus(PadSearchFocus(), esrwiki::pad::SearchEvent::Shortcut, PadFocusContext());
    const bool toList = next == esrwiki::pad::SearchFocus::List;
    if (toList == suggestPadFocus) return false;
    PadSetSearchFocus(next);
    return true;
}

void OverlayState::OpenOsk() {
    if (oskOpen) return;
    if (oskRows.empty()) oskRows = esrwiki::pad::OskLayout(oskAccents, gKeyboardLocale);
    oskOpen = true;
    oskRow = 1;
    oskCol = 0;
    SetFocus(search);
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::CloseOsk() {
    if (!oskOpen) return;
    oskOpen = false;
    oskHits.clear();
    if (IsWindow(window)) InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::PressOskKey(int row, int col) {
    if (row < 0 || row >= static_cast<int>(oskRows.size())) return;
    const auto& keys = oskRows[static_cast<std::size_t>(row)];
    if (col < 0 || col >= static_cast<int>(keys.size())) return;
    const esrwiki::pad::OskKey& key = keys[static_cast<std::size_t>(col)];
    switch (key.kind) {
    case esrwiki::pad::KeyKind::Char:
    case esrwiki::pad::KeyKind::Space:
        SendMessageW(search, WM_CHAR, static_cast<WPARAM>(key.ch), 0);
        break;
    case esrwiki::pad::KeyKind::Backspace:
        SendMessageW(search, WM_CHAR, static_cast<WPARAM>(VK_BACK), 0);
        break;
    case esrwiki::pad::KeyKind::Clear:
        SetWindowTextW(search, L"");
        QueryChanged();
        break;
    case esrwiki::pad::KeyKind::Done:
        CloseOsk();
        break;
    case esrwiki::pad::KeyKind::Accents:
        oskAccents = !oskAccents;
        oskRows = esrwiki::pad::OskLayout(oskAccents, gKeyboardLocale);
        oskRow = std::min(oskRow, static_cast<int>(oskRows.size()) - 1);
        oskCol = std::min(oskCol, static_cast<int>(oskRows[static_cast<std::size_t>(oskRow)].size()) - 1);
        break;
    }
    InvalidateRect(window, nullptr, FALSE);
}

// A click on a key of the on-screen keyboard.
bool OverlayState::OskClick(int x, int y) {
    if (!oskOpen || !Inside(oskRect, x, y)) return false;
    for (const OskHit& hit : oskHits) {
        if (!Inside(hit.rect, x, y)) continue;
        oskRow = hit.row;
        oskCol = hit.col;
        PressOskKey(hit.row, hit.col);
        break;
    }
    return true;
}

// The bright frame round what the D-pad has highlighted.
void OverlayState::DrawPadFocus(HDC dc) {
    // The detail pane's zones are framed after the pane is drawn: DrawCardFocus a card, DrawDetailFocus a button or field.
    if (!padActive || oskOpen || suggestPadFocus || padZone == PadZone::Cards || padZone == PadZone::Chips ||
        padZone == PadZone::Calc || padZone == PadZone::Spec || padZone == PadZone::Campaign || padZone == PadZone::Skills) return;
    const COLORREF focus = RGB(255, 240, 180);
    RECT target{};
    if (padZone == PadZone::Switches) {
        if (padSwitch >= layout.switchButtons.size()) return;
        target = layout.switchButtons[padSwitch];
    } else if (padZone == PadZone::Tabs) {
        if (tabFocus >= layout.tabs.size()) return;
        target = layout.tabs[tabFocus];
    } else {
        PageView& view = CurrentView();
        const std::vector<RECT> rows = PageRects(view.listPage);
        if (view.selected < pageStarts[view.listPage] || view.selected - pageStarts[view.listPage] >= rows.size()) return;
        target = rows[view.selected - pageStarts[view.listPage]];
    }
    for (int grow : {S(1), S(2)}) {
        FrameColor(dc, Rect(target.left - grow, target.top - grow, target.right + grow, target.bottom + grow), focus);
    }
}

// The focus frame for the filter and sort buttons and the calculator's fields. They are inside the detail pane, so this
// runs after the pane is drawn, with this paint's positions.
void OverlayState::DrawDetailFocus(HDC dc) {
    if (!padActive || oskOpen || suggestPadFocus) return;
    RECT target{};
    if (padZone == PadZone::Chips) {
        if (padChip >= layout.chips.size()) return;
        target = layout.chips[padChip].rect;
    } else if (padZone == PadZone::Calc) {
        if (!CalcPadRect(target)) return;
    } else if (padZone == PadZone::Spec) {
        if (!SpecPadRect(target)) return;
    } else if (padZone == PadZone::Campaign) {
        if (!CampaignPadRect(target)) return;
    } else if (padZone == PadZone::Skills) {
        if (!SkillsPadRect(target)) return;
    } else {
        return;
    }
    const COLORREF focus = RGB(255, 240, 180);
    const int saved = SaveDC(dc);
    if (padZone == PadZone::Calc || padZone == PadZone::Spec || padZone == PadZone::Campaign || padZone == PadZone::Skills) IntersectClipRect(dc, layout.detail.left + 1, clipTop, layout.detail.right - S(18), clipBottom);
    else IntersectClipRect(dc, layout.detail.left, layout.detail.top, layout.detail.right, layout.detail.bottom);
    for (int grow : {S(1), S(2)}) {
        FrameColor(dc, Rect(target.left - grow, target.top - grow, target.right + grow, target.bottom + grow), focus);
    }
    RestoreDC(dc, saved);
}

// The item card the D-pad highlights: its frame, and its tooltip beside it. It runs after the detail pane is drawn, so
// it uses this paint's positions. A card that has scrolled out of view gives the focus to a card that is showing.
void OverlayState::DrawCardFocus(HDC dc) {
    if (!padActive || oskOpen || suggestPadFocus || padZone != PadZone::Cards) {  // the list's rows are not under a card tooltip
        if (hover.block != nullptr && hover.block == padCardBlock) ClearHover();
        return;
    }
    if (cardHits.empty()) {
        ClearHover();
        padCardBlock = nullptr;
        return;
    }
    // While the left stick is steering the pointer the tooltip follows the pointer (HandleMove). Forcing the D-pad's card
    // back on every paint fought it: each pointer move cleared the tooltip, each repaint put it back, and the repaints
    // kept the UI thread too busy to poll the controller.
    if (GetTickCount64() < padMouseUntil) return;
    const CardHit* focus = nullptr;
    for (const CardHit& hit : cardHits) {
        if (hit.block == padCardBlock) {
            focus = &hit;
            break;
        }
    }
    if (focus == nullptr) {
        focus = &cardHits.front();
        if (padCardPending < 0) {  // scrolled up: take the lowest card that shows
            for (const CardHit& hit : cardHits) {
                if (hit.rect.top >= focus->rect.top) focus = &hit;
            }
        }
    }
    if (padCardPending != 0 && focus->block == padCardBlock) {
        // The pane scrolled to make room for the next card in that direction, so the highlighted card is still showing
        // (now further down or up). Step on to the card beyond it instead of staying on it.
        std::vector<esrwiki::pad::Box> boxes;
        int index = 0;
        for (std::size_t i = 0; i < cardHits.size(); ++i) {
            const RECT& b = cardHits[i].rect;
            boxes.push_back(esrwiki::pad::Box{b.left, b.top, b.right, b.bottom});
            if (&cardHits[i] == focus) index = static_cast<int>(i);
        }
        const int next = esrwiki::pad::Neighbor(boxes, index, 0, padCardPending);
        if (next >= 0) focus = &cardHits[static_cast<std::size_t>(next)];
    }
    padCardPending = 0;
    padCardBlock = focus->block;
    padCardEntry = focus->entry;
    const RECT r = focus->rect;
    // A card the D-pad lands on that is cut off by the top or bottom of the pane is scrolled fully into view (a card taller
    // than the pane lines its top up). The tooltip and frame are drawn on the repaint that follows.
    {
        const int margin = S(6);
        int delta = 0;
        if (r.top < clipTop + margin) delta = r.top - (clipTop + margin);
        else if (r.bottom > clipBottom - margin) delta = std::min<int>(r.bottom - (clipBottom - margin), r.top - (clipTop + margin));
        if (delta != 0) {
            PageView& view = CurrentView();
            const int target = std::clamp(view.detailScroll + delta, 0, std::max(0, scrollTotal - scrollVisible));
            if (target != view.detailScroll) {
                view.detailScroll = target;
                InvalidateRect(window, nullptr, FALSE);
                return;
            }
        }
    }
    hover.entry = focus->entry;
    hover.block = focus->block;
    hover.rune = -1;
    hover.at = POINT{(r.left + r.right) / 2, r.top};
    hover.target = r;
    const COLORREF gold = RGB(255, 240, 180);
    // A card scrolled partly out of the pane keeps its frame inside the pane, not over the tabs or header.
    const int saved = SaveDC(dc);
    // only the band the cards scroll in: not the filter and sort bar above it, nor the tabs
    IntersectClipRect(dc, layout.detail.left + 1, clipTop, layout.detail.right - S(18), clipBottom);
    for (int grow : {S(1), S(2)}) {
        FrameColor(dc, Rect(r.left - grow, r.top - grow, r.right + grow, r.bottom + grow), gold);
    }
    RestoreDC(dc, saved);
}

// Moves the card highlight to the nearest card in that direction. At the left edge it returns to the categories; at
// the top or bottom edge of what is showing it scrolls the pane a step and carries on with the card it finds there.
void OverlayState::PadCardMove(int dx, int dy) {
    if (cardHits.empty()) {
        PadLeaveCards();
        padZone = PadZone::List;
        return;
    }
    std::vector<esrwiki::pad::Box> boxes;
    int current = 0;
    for (std::size_t i = 0; i < cardHits.size(); ++i) {
        const RECT& r = cardHits[i].rect;
        boxes.push_back(esrwiki::pad::Box{r.left, r.top, r.right, r.bottom});
        if (cardHits[i].block == padCardBlock) current = static_cast<int>(i);
    }
    const int next = esrwiki::pad::Neighbor(boxes, current, dx, dy);
    if (next >= 0) {
        const CardHit& hit = cardHits[static_cast<std::size_t>(next)];
        padCardBlock = hit.block;
        padCardEntry = hit.entry;
        return;
    }
    if (dx < 0) {
        PadLeaveCards();
        padZone = PadZone::List;
        return;
    }
    if (dy != 0) {
        PageView& view = CurrentView();
        const int step = std::max(S(40), static_cast<int>(layout.detail.bottom - layout.detail.top) * 3 / 4);
        const int limit = std::max(0, scrollTotal - scrollVisible);
        const int target = std::clamp(view.detailScroll + dy * step, 0, limit);
        if (target != view.detailScroll) {
            view.detailScroll = target;
            padCardPending = dy;
        } else if (dy < 0 && !layout.chips.empty()) {  // already at the top: up goes on to the filter and sort buttons
            const RECT& from = cardHits[static_cast<std::size_t>(current)].rect;
            const int fromX = (from.left + from.right) / 2;
            PadLeaveCards();
            PadEnterChips(fromX);
        }
    }
}

// The filter and sort buttons: the one in the bottom row nearest the card the highlight came from.
void OverlayState::PadEnterChips(int fromX) {
    if (layout.chips.empty()) return;
    LONG lowest = layout.chips.front().rect.top;
    for (const Chip& chip : layout.chips) lowest = std::max(lowest, chip.rect.top);
    padChip = 0;
    int bestDistance = -1;
    for (std::size_t i = 0; i < layout.chips.size(); ++i) {
        const RECT& r = layout.chips[i].rect;
        if (r.top != lowest) continue;
        const int distance = std::abs((r.left + r.right) / 2 - fromX);
        if (bestDistance < 0 || distance < bestDistance) {
            bestDistance = distance;
            padChip = i;
        }
    }
    padZone = PadZone::Chips;
}

// A zone the page no longer has (another tab was opened, or the buttons went away) goes back to the categories.
void OverlayState::PadValidateZone() {
    if (padZone == PadZone::Chips && layout.chips.empty()) padZone = PadZone::List;
    if (padZone == PadZone::Calc && (!CalcActive() || calcHits.empty())) padZone = PadZone::List;
    if (padZone == PadZone::Spec && (!SpecActive() || specHits.empty())) padZone = PadZone::List;
    if (padZone == PadZone::Campaign && (!CampaignShowing() || campaignSpots.empty())) padZone = PadZone::List;
    if (padZone == PadZone::Skills && (!SkillsActive() || skillsHits.empty())) padZone = PadZone::List;
    if (padZone == PadZone::Chips && padChip >= layout.chips.size()) padChip = layout.chips.size() - 1;
}

void OverlayState::PadEnterCards() {
    if (cardHits.empty()) return;
    padZone = PadZone::Cards;
    padCardBlock = cardHits.front().block;
    padCardEntry = cardHits.front().entry;
    padCardPending = 0;
}

void OverlayState::PadLeaveCards() {
    if (padCardBlock != nullptr) ClearHover();
    padCardBlock = nullptr;
    padCardEntry = nullptr;
    padCardPending = 0;
}

// Up from the top row of tabs goes to the header buttons, the one nearest the highlighted tab.
void OverlayState::PadEnterSwitches() {
    padZone = PadZone::Switches;
    padSwitch = 0;
    if (tabFocus < layout.tabs.size() && !layout.switchButtons.empty()) {
        const int centre = (layout.tabs[tabFocus].left + layout.tabs[tabFocus].right) / 2;
        int bestDistance = -1;
        for (std::size_t i = 0; i < layout.switchButtons.size() && i < switches.size(); ++i) {
            const RECT& button = layout.switchButtons[i];
            const int distance = std::abs((button.left + button.right) / 2 - centre);
            if (bestDistance < 0 || distance < bestDistance) {
                bestDistance = distance;
                padSwitch = i;
            }
        }
    }
}

// After the left stick has moved the cursor, the highlight jumps to whatever is under it, so the D-pad press that follows
// moves on from there instead of from where the highlight was left.
void OverlayState::PadFocusFromCursor() {
    POINT at{};
    if (!GetCursorPos(&at) || !ScreenToClient(window, &at)) return;
    const int x = at.x, y = at.y;
    for (std::size_t i = 0; i < layout.switchButtons.size(); ++i) {
        if (Inside(layout.switchButtons[i], x, y)) {
            padZone = PadZone::Switches;
            padSwitch = i;
            return;
        }
    }
    for (std::size_t i = 0; i < layout.tabs.size(); ++i) {
        if (Inside(layout.tabs[i], x, y)) {
            padZone = PadZone::Tabs;
            tabFocus = i;
            return;
        }
    }
    if (CalcActive()) {
        for (const CalcHit& hit : calcHits) {
            if ((hit.kind == 0 || hit.kind == 1) && Inside(hit.rect, x, y) && calcOpen < 0) {
                padZone = PadZone::Calc;
                calcPadKind = hit.kind;
                calcPadId = hit.index;
                return;
            }
        }
    }
    if (SpecActive() && Inside(layout.detail, x, y) && y >= clipTop) {
        for (const SpecHit& hit : specHits) {
            if (Inside(hit.rect, x, y)) {
                padZone = PadZone::Spec;
                specPadKind = hit.kind;
                specPadIndex = hit.index;
                return;
            }
        }
    }
    if (SkillsActive() && Inside(layout.detail, x, y)) {
        for (const SkillsHit& hit : skillsHits) {
            if (Inside(hit.rect, x, y) && !(hit.kind == kSkStep && hit.value % 3 != 1)) {  // not a - or + button
                padZone = PadZone::Skills;
                skillsPadKind = hit.kind;
                skillsPadValue = hit.value;
                return;
            }
        }
    }
    if (CampaignShowing() && Inside(layout.detail, x, y) && y >= clipTop) {
        const int shift = bodyTop - CurrentView().detailScroll;
        for (std::size_t i = 0; i < campaignSpots.size(); ++i) {
            const RECT& r = campaignSpots[i].rect;
            if (x >= r.left && x < r.right && y >= r.top + shift && y < r.bottom + shift) {
                padZone = PadZone::Campaign;
                campaignPadIndex = static_cast<int>(i);
                return;
            }
        }
    }
    for (std::size_t i = 0; i < layout.chips.size(); ++i) {
        if (Inside(layout.chips[i].rect, x, y)) {
            padZone = PadZone::Chips;
            padChip = i;
            return;
        }
    }
    for (const CardHit& hit : cardHits) {
        if (Inside(hit.rect, x, y) && Inside(layout.detail, x, y)) {
            padZone = PadZone::Cards;
            padCardBlock = hit.block;
            padCardEntry = hit.entry;
            return;
        }
    }
    PageView& view = CurrentView();
    const std::vector<RECT> rows = PageRects(view.listPage);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (Inside(rows[i], x, y)) {
            padZone = PadZone::List;
            const std::size_t index = pageStarts[view.listPage] + i;
            if (index != view.selected && index < filtered.size()) SelectFiltered(index);
            return;
        }
    }
}

void OverlayState::PadDirection(int dx, int dy) {
    padMouseUntil = 0;
    if (padCursorMoved) {
        padCursorMoved = false;
        PadFocusFromCursor();
    }
    PadValidateZone();
    PageView& view = CurrentView();
    switch (padZone) {
    case PadZone::Tabs: {
        std::vector<esrwiki::pad::Box> boxes;
        for (const RECT& tab : layout.tabs) boxes.push_back({tab.left, tab.top, tab.right, tab.bottom});
        if (tabFocus >= boxes.size()) tabFocus = pageIndex;
        const int next = esrwiki::pad::Neighbor(boxes, static_cast<int>(tabFocus), dx, dy);
        if (next >= 0) tabFocus = static_cast<std::size_t>(next);
        else if (dy > 0 && !filtered.empty()) padZone = PadZone::List;  // no more tabs below: on to the categories
        else if (dy < 0 && !switches.empty()) PadEnterSwitches();        // above the top row: the header buttons
        break;
    }
    case PadZone::Switches:
        if (dx < 0 && padSwitch > 0) --padSwitch;
        else if (dx > 0 && padSwitch + 1 < switches.size()) ++padSwitch;
        else if (dy > 0) {
            padZone = PadZone::Tabs;
            tabFocus = pageIndex;
        }
        break;
    case PadZone::List:
        if (dy > 0) {
            SelectFiltered(std::min(view.selected + 1, filtered.empty() ? std::size_t{0} : filtered.size() - 1));
        } else if (dy < 0) {
            if (filtered.empty() || view.selected == 0) {
                padZone = PadZone::Tabs;
                tabFocus = pageIndex;
            } else {
                SelectFiltered(view.selected - 1);
            }
        } else if (dx > 0) {
            if (CalcActive() && !calcHits.empty()) PadEnterCalc();  // the calculator's fields
            else if (SpecActive() && !specHits.empty()) PadEnterSpec();  // the specialization board
            else if (SkillsActive() && !skillsHits.empty()) PadEnterSkills();  // the skill tree
            else if (CampaignShowing() && !campaignSpots.empty()) PadEnterCampaign();  // the Campaign Progression map
            else if (!cardHits.empty()) PadEnterCards();            // right from the categories: into the items
            else if (!layout.chips.empty() && pageStarts.size() <= 1) {  // nothing showing: the filter and sort buttons
                padZone = PadZone::Chips;
                padChip = 0;
            } else MovePage(1);
        } else if (dx < 0) {
            MovePage(-1);
        }
        break;
    case PadZone::Cards:
        PadCardMove(dx, dy);
        break;
    case PadZone::Chips: {
        std::vector<esrwiki::pad::Box> boxes;
        for (const Chip& chip : layout.chips) boxes.push_back({chip.rect.left, chip.rect.top, chip.rect.right, chip.rect.bottom});
        const int next = esrwiki::pad::Neighbor(boxes, static_cast<int>(padChip), dx, dy);
        if (next >= 0) padChip = static_cast<std::size_t>(next);
        else if (dx < 0) padZone = PadZone::List;                // left of the first button: the categories
        else if (dy > 0 && !cardHits.empty()) PadEnterCards();   // below the last row: into the items
        break;
    }
    case PadZone::Calc:
        CalcPadMove(dx, dy);
        break;
    case PadZone::Spec:
        SpecPadMove(dx, dy);
        break;
    case PadZone::Campaign:
        CampaignPadMove(dx, dy);
        break;
    case PadZone::Skills:
        SkillsPadMove(dx, dy);
        break;
    }
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::PadConfirm(ULONGLONG now) {
    PadValidateZone();
    if (now < padMouseUntil) {  // the left stick moved the cursor: A is a click
        PadClickAtCursor();
        return;
    }
    if (padZone == PadZone::Chips) {  // a filter or sort button
        if (padChip < layout.chips.size()) {
            const Chip chip = layout.chips[padChip];  // copied: the repaint after it rebuilds the buttons
            ApplyChip(chip);
        }
        return;
    }
    if (padZone == PadZone::Calc) {
        CalcPadConfirm();
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (padZone == PadZone::Spec) {
        SpecPadConfirm();
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (padZone == PadZone::Campaign) {
        CampaignPadConfirm();
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (padZone == PadZone::Skills) {
        SkillsPadConfirm();
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (padZone == PadZone::Switches) {  // a header button: A opens that panel
        if (padSwitch < switches.size() && !switches[padSwitch].current &&
            (!switches[padSwitch].available || switches[padSwitch].available())) {
            BeginSwitch(switches[padSwitch].onSwitch);
        }
        return;
    }
    if (padZone == PadZone::Tabs && tabFocus < database->pages.size()) SetPage(tabFocus);  // only A changes the tab
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::PadBack() {
    PadValidateZone();
    if (padZone == PadZone::Cards) {
        PadLeaveCards();
        padZone = PadZone::List;
    } else if (padZone == PadZone::Chips || padZone == PadZone::Spec) {
        padZone = PadZone::List;
    } else if (padZone == PadZone::Skills) {
        if (!SkillsPadBack()) padZone = PadZone::List;  // from the details, back to the tree first
    } else if (padZone == PadZone::Campaign) {
        if (!CampaignPadBack()) padZone = PadZone::List;  // the details close first
    } else if (padZone == PadZone::Calc) {
        if (!CalcPadBack()) padZone = PadZone::List;  // an open list closes first
    } else if (padZone == PadZone::List || padZone == PadZone::Switches) {
        padZone = PadZone::Tabs;
        tabFocus = pageIndex;
    }
    InvalidateRect(window, nullptr, FALSE);
}

// The on-screen keyboard, under the search box.
void OverlayState::DrawOsk(HDC dc) {
    oskHits.clear();
    if (!oskOpen) return;
    // The keyboard is not page text: the search highlight must not mark its keys (typing "a" would light up the "a" key).
    struct HighlightOff {
        SearchHighlight saved;
        HighlightOff() : saved(gHighlight) { gHighlight.clear(); }
        ~HighlightOff() { gHighlight = saved; }
    } highlightOff;
    // The size and place come from PlanSearchPopups (overlay_suggest.inc): the keys get shorter when the suggestion list
    // needs the room under the keyboard.
    PlanSearchPopups();
    const int units = oskUnits;  // the widest row, in half-key units (20 unless the language's letters need more)
    const int gap = S(4);
    const int pad = S(8);
    const int unit = oskUnit;
    const int keyHeight = popupPlace.keyHeight;
    const int innerWidth = units * unit;
    const int boxWidth = oskBoxWidth;
    const int rowsCount = static_cast<int>(oskRows.size());
    const int left = layout.search.left;
    const int top = popupPlace.keyboardTop;
    oskRect = Rect(left, top, left + boxWidth, popupPlace.keyboardBottom);
    DropShadow(dc, oskRect, S(3));
    FillPanel(dc, oskRect, RGB(10, 7, 4), 238);
    FrameColor(dc, oskRect, kGold);
    const int keysLeft = left + (boxWidth - innerWidth) / 2;
    for (int r = 0; r < rowsCount; ++r) {
        const auto& keys = oskRows[static_cast<std::size_t>(r)];
        const int y = top + pad + r * (keyHeight + gap);
        int cumulative = 0;
        int rowSpan = 0;
        for (const esrwiki::pad::OskKey& key : keys) rowSpan += key.span;
        const int rowLeft = keysLeft + (units - rowSpan) * unit / 2;  // a shorter row is centred
        for (int c = 0; c < static_cast<int>(keys.size()); ++c) {
            const esrwiki::pad::OskKey& key = keys[static_cast<std::size_t>(c)];
            const RECT rect = Rect(rowLeft + cumulative * unit, y, rowLeft + (cumulative + key.span) * unit - gap, y + keyHeight);
            cumulative += key.span;
            const bool focus = padActive && !suggestPadFocus && r == oskRow && c == oskCol;
            FillPanel(dc, rect, focus ? kSelected : kPanel, 235);
            FrameColor(dc, rect, focus ? kGold : kBorder);
            if (focus) FrameColor(dc, Rect(rect.left - 1, rect.top - 1, rect.right + 1, rect.bottom + 1), RGB(255, 240, 180));
            std::wstring label = key.label;
            if (key.kind == esrwiki::pad::KeyKind::Space) label = UiText(L"Space");
            else if (key.kind == esrwiki::pad::KeyKind::Clear) label = UiText(L"Clear");
            else if (key.kind == esrwiki::pad::KeyKind::Done) label = UiText(L"Done");
            Text(dc, label, rect, focus ? kHeaderGold : kText, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            oskHits.push_back(OskHit{rect, r, c});
        }
    }
}

// ---- Controller help picture, right of the wiki ----------------------------------------------------------------------
float OverlayState::HelpIconWidth(const std::string& folder, const char* name, float iconHeight) {
    if (name == nullptr) return 0.0f;
    Gdiplus::Bitmap* img = FindImage("controller/" + folder + "/" + name);
    if (img == nullptr || img->GetHeight() == 0) return 0.0f;
    return iconHeight * static_cast<float>(img->GetWidth()) / static_cast<float>(img->GetHeight());
}

// The help rows for where the controller's focus is: the page, the on-screen keyboard, or the search suggestions.
static std::vector<HelpRow> HelpRowsFor(esrwiki::pad::Kind kind, esrwiki::pad::SearchFocus focus) {
    using esrwiki::pad::SearchFocus;
    const bool sony = kind == esrwiki::pad::Kind::PlayStation;
    const wchar_t* a = sony ? L"Cross" : L"A";
    const wchar_t* b = sony ? L"Circle" : L"B";
    const wchar_t* x = sony ? L"Square" : L"X";
    const wchar_t* y = sony ? L"Triangle" : L"Y";
    const wchar_t* shoulder = sony ? L"L1 / R1" : L"LB / RB";
    const wchar_t* trig = sony ? L"L2 / R2" : L"LT / RT";
    const wchar_t* start = sony ? L"Options" : L"Menu";
    const wchar_t* view = sony ? L"Share / Create" : L"View";
    if (focus == SearchFocus::List) {
        return {{L"D-pad up / down", L"Pick a suggestion", "dpadup", "dpaddown"},
                {a, L"Open the suggestion", "buttonbottom", nullptr},
                {b, L"Close the suggestions", "buttonright", nullptr},
                {shoulder, L"Page through the suggestions", "bumperleft", "bumperright"},
                {y, L"Show / hide keyboard", "buttontop", nullptr},
                {view, L"Leave the suggestions", "buttonselect", nullptr},
                {start, L"Close", "buttonstart", nullptr}};
    }
    if (focus == SearchFocus::Keyboard) {
        return {{L"D-pad", L"Highlight a key", "dpad", nullptr},
                {L"D-pad down", L"From the bottom row: suggestions", "dpaddown", nullptr},
                {a, L"Type the key", "buttonbottom", nullptr},
                {b, L"Clear search / close keyboard", "buttonright", nullptr},
                {x, L"Delete a character", "buttonleft", nullptr},
                {y, L"Close keyboard", "buttontop", nullptr},
                {view, L"Search suggestions", "buttonselect", nullptr},
                {shoulder, L"Previous / next tab", "bumperleft", "bumperright"},
                {L"Left stick", L"Move cursor (A clicks)", "stickleft", nullptr},
                {start, L"Close", "buttonstart", nullptr}};
    }
    return {{L"D-pad", L"Highlight", "dpad", nullptr},
            {a, L"Select", "buttonbottom", nullptr},
            {b, L"Back", "buttonright", nullptr},
            {y, L"Search", "buttontop", nullptr},
            {x, L"Clear search", "buttonleft", nullptr},
            {view, L"Search suggestions", "buttonselect", nullptr},
            {shoulder, L"Previous / next tab", "bumperleft", "bumperright"},
            {trig, L"Scroll up / down fast", "triggerleft", "triggerright"},
            {L"Right stick", L"Scroll", "stickright", nullptr},
            {L"Left stick", L"Move cursor (A clicks)", "stickleft", nullptr},
            {L"R3", L"Switch panel", "stickrightbutton", nullptr},
            {start, L"Close", "buttonstart", nullptr}};
}

// The help list and how big it is: its rows, and a window size that fits them (no more room than the text and sprites need).
HelpMetrics OverlayState::MeasureHelp(esrwiki::pad::Kind kind) {
    using esrwiki::pad::Kind;
    using esrwiki::pad::SearchFocus;
    HelpMetrics m;
    const bool sony = kind == Kind::PlayStation;
    m.picture = FindImage(sony ? "controller_playstation" : "controller_xbox") != nullptr;
    m.rows = HelpRowsFor(kind, PadSearchFocus());
    // The window is sized for the largest of the three lists, so it does not change size as the focus moves.
    std::vector<HelpRow> allRows;
    for (SearchFocus focus : {SearchFocus::Page, SearchFocus::Keyboard, SearchFocus::List}) {
        const std::vector<HelpRow> rows = HelpRowsFor(kind, focus);
        allRows.insert(allRows.end(), rows.begin(), rows.end());
    }
    std::size_t mostRows = 0;
    for (SearchFocus focus : {SearchFocus::Page, SearchFocus::Keyboard, SearchFocus::List})
        mostRows = std::max(mostRows, HelpRowsFor(kind, focus).size());
    m.heading = UiText(sony ? L"PlayStation controller" : L"Xbox controller");
    m.inset = static_cast<float>(S(10));
    m.size = static_cast<float>(std::max(12.0, S(15) * uiScale));
    m.line = m.size * 2.3f;
    m.iconHeight = m.size * 2.0f;
    m.gap = static_cast<float>(S(6));
    const std::string folder = sony ? "ps4" : "xbox";
    for (const HelpRow& row : allRows) {
        const float group = HelpIconWidth(folder, row.icon1, m.iconHeight) +
                            (row.icon2 != nullptr ? m.gap + HelpIconWidth(folder, row.icon2, m.iconHeight) : 0.0f);
        m.iconColumn = std::max(m.iconColumn, group);
    }
    m.textX = m.inset + m.iconColumn + m.gap;

    // The widest text, measured with the fonts the list is drawn with.
    Gdiplus::Bitmap probe(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&probe);
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, m.size, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font bold(&family, m.size, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    const auto measure = [&](const std::wstring& text, const Gdiplus::Font& f) {
        Gdiplus::RectF box;
        g.MeasureString(text.c_str(), -1, &f, Gdiplus::PointF(0, 0), &box);
        return box.Width;
    };
    float right = m.inset + measure(m.heading, bold);
    for (const HelpRow& row : allRows) {
        const float widest = std::max(measure(UiText(row.button), bold), measure(UiText(row.action), font));
        right = std::max(right, m.textX + widest);
    }
    m.width = static_cast<int>(std::ceil(right + m.inset));
    m.height = static_cast<int>(std::ceil(m.inset + m.size * 2.0f + m.line * static_cast<float>(mostRows) + m.inset));
    return m;
}

// The help window is as big as its list, against the wiki's right edge and centred on the wiki's height.
void OverlayState::PositionHelp() {
    if (!IsWindow(helpWindow) || !IsWindow(window) || helpWidth <= 0) return;
    RECT area{};
    GetWindowRect(window, &area);
    const HelpMetrics m = MeasureHelp(padKind);
    const int available = area.bottom - area.top;
    const int w = m.picture ? helpWidth : m.width;
    const int h = std::min(m.picture ? available : m.height, available);
    const int x = area.right + S(8);  // against the wiki's right edge
    const int y = area.top + (available - h) / 2;
    SetWindowPos(helpWindow, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

void OverlayState::RenderHelp() {
    using esrwiki::pad::Kind;
    if (!IsWindow(helpWindow)) return;
    RECT area{};
    GetWindowRect(helpWindow, &area);
    const int w = area.right - area.left;
    const int h = area.bottom - area.top;
    if (w <= 4 || h <= 4) return;
    const int context = static_cast<int>(PadSearchFocus());
    if (helpDrawnKind == padKind && helpDrawnWidth == w && helpDrawnHeight == h && helpDrawnContext == context) return;
    helpDrawnContext = context;
    helpDrawnKind = padKind;
    helpDrawnWidth = w;
    helpDrawnHeight = h;

    Gdiplus::Bitmap bitmap(w, h, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&bitmap);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    // Opaque: while one panel hands over to the other, both help windows are up for a moment in the same place. With a
    // see-through fill the two stacked (twice as dark) and the drop back to one was the flicker.
    Gdiplus::SolidBrush panel(Gdiplus::Color(255, 12, 9, 6));
    Gdiplus::Pen frame(Gdiplus::Color(255, 122, 92, 48), 2.0f);
    g.FillRectangle(&panel, 1, 1, w - 2, h - 2);
    g.DrawRectangle(&frame, 1, 1, w - 3, h - 3);

    const bool sony = padKind == Kind::PlayStation;
    Gdiplus::Bitmap* picture = FindImage(sony ? "controller_playstation" : "controller_xbox");
    const int inset = S(10);
    if (picture != nullptr && picture->GetWidth() > 0 && picture->GetHeight() > 0) {
        const double scale = std::min(static_cast<double>(w - 2 * inset) / picture->GetWidth(),
                                      static_cast<double>(h - 2 * inset) / picture->GetHeight());
        const int pw = static_cast<int>(picture->GetWidth() * scale);
        const int ph = static_cast<int>(picture->GetHeight() * scale);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(picture, (w - pw) / 2, (h - ph) / 2, pw, ph);
    } else {
        // No picture file yet: the buttons as a list, each with its sprite from the plugin's images\controller folder
        // (ps4 or xbox, the game's own controller art). MeasureHelp sizes the window to this list.
        const HelpMetrics m = MeasureHelp(padKind);
        const std::string folder = sony ? "ps4" : "xbox";
        Gdiplus::FontFamily family(L"Segoe UI");
        const Gdiplus::REAL size = m.size;
        Gdiplus::Font font(&family, size, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font bold(&family, size, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush gold(Gdiplus::Color(255, 224, 182, 96));
        Gdiplus::SolidBrush text(Gdiplus::Color(255, 244, 236, 220));
        float top = m.inset;
        g.DrawString(m.heading.c_str(), -1, &bold, Gdiplus::PointF(m.inset, top), &gold);
        top += size * 2.0f;
        // Each sprite is drawn at the row's height; its width follows its own proportions.
        const auto drawIcon = [&](const char* name, float left, float at) -> float {
            const float width = HelpIconWidth(folder, name, m.iconHeight);
            if (width <= 0.0f) return 0.0f;
            Gdiplus::Bitmap* img = FindImage("controller/" + folder + "/" + name);
            g.DrawImage(img, left, at, width, m.iconHeight);
            return width;
        };
        for (const HelpRow& row : m.rows) {
            const float iconY = top + (m.line - m.iconHeight) / 2.0f;
            float left = m.inset;
            left += drawIcon(row.icon1, left, iconY);
            if (row.icon2 != nullptr) left += m.gap + drawIcon(row.icon2, left + m.gap, iconY);
            const std::wstring button = UiText(row.button);
            const std::wstring action = UiText(row.action);
            g.DrawString(button.c_str(), -1, &bold, Gdiplus::PointF(m.textX, top), &gold);
            g.DrawString(action.c_str(), -1, &font, Gdiplus::PointF(m.textX, top + size * 1.1f), &text);
            top += m.line;
        }
    }

    HBITMAP bits = nullptr;
    if (bitmap.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &bits) != Gdiplus::Ok || bits == nullptr) return;
    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    const HGDIOBJ old = SelectObject(memory, bits);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    POINT origin{area.left, area.top};
    POINT source{0, 0};
    SIZE size{w, h};
    UpdateLayeredWindow(helpWindow, screen, &origin, &size, memory, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, old);
    DeleteDC(memory);
    DeleteObject(bits);
    ReleaseDC(nullptr, screen);
}

void OverlayState::SyncHelp() {
    if (!IsWindow(helpWindow)) return;
    const bool show = padRunning && padActive && helpWidth > 0 && IsWindow(window) && IsWindowVisible(window);
    if (!show) {
        if (IsWindowVisible(helpWindow)) ShowWindow(helpWindow, SW_HIDE);
        return;
    }
    PositionHelp();
    RenderHelp();
    if (!IsWindowVisible(helpWindow)) ShowWindow(helpWindow, SW_SHOWNOACTIVATE);
}

// The list shows only each entry's title, wrapped to the row.
void OverlayState::PaintList(HDC dc) {
    const Page& page = CurrentPage();
    PageView& view = CurrentView();
    EnsurePages();
    const std::size_t pages = pageStarts.size();
    view.listPage = std::min(view.listPage, pages - 1);
    const std::vector<RECT> pageRows = PageRects(view.listPage);

    std::wstring countText = UiFormat(L"{0} of {1} entries", {std::to_wstring(filtered.size()), std::to_wstring(page.entries.size())});
    if (!query.empty()) {
        const std::wstring position = filtered.empty() ? L"0" : std::to_wstring(view.selected + 1);
        countText = UiFormat(L"{0} of {1} entries here | {2} total", {position, std::to_wstring(filtered.size()), std::to_wstring(totalMatches)});
    }
    const bool noting = !searchNote.empty() && GetTickCount64() < searchNoteUntil;  // "Filters cleared" after a jump, for a moment
    Text(dc, noting ? searchNote : countText, layout.count, noting ? kGold : kDim, fontSmall, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (!query.empty()) {
        CornerShadow(dc, layout.matchPrev, S(2));
        FillPanel(dc, layout.matchPrev, kPanel, 200);
        FrameColor(dc, layout.matchPrev, kBorder);
        Text(dc, L"<", layout.matchPrev, totalMatches == 0 ? kDim : kText, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        CornerShadow(dc, layout.matchNext, S(2));
        FillPanel(dc, layout.matchNext, kPanel, 200);
        FrameColor(dc, layout.matchNext, kBorder);
        Text(dc, L">", layout.matchNext, totalMatches == 0 ? kDim : kText, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    if (filtered.empty()) {
        Text(dc, UiText(L"No matches"), Rect(listLeftX, listTopY, listRightX, listTopY + std::max(S(24), bodyLine + S(6))), kDim, fontBody, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    for (std::size_t i = 0; i < pageRows.size(); ++i) {
        const std::size_t position = pageStarts[view.listPage] + i;
        if (position >= filtered.size()) break;
        const Entry& entry = page.entries[filtered[position]];
        const RECT row = pageRows[i];
        const bool selected = position == view.selected;
        CornerShadow(dc, row, S(2));
        FillPanel(dc, row, selected ? kSelected : kPanel, 185);
        if (selected) FrameColor(dc, row, kGold);
        const RECT titleRect = Rect(row.left + S(12), row.top + S(4), row.right - S(12), row.bottom - S(4));
        // Wrapped lines, left-aligned, with the block centred vertically in the row.
        const std::wstring rowTitle = UiDataText(Wide(entry.title));
        const int titleHeight = MeasureHeight(dc, rowTitle, titleRect.right - titleRect.left, fontBold);
        RECT centred = titleRect;
        centred.top += std::max<int>(0, (static_cast<int>(titleRect.bottom - titleRect.top) - titleHeight) / 2);
        Text(dc, rowTitle, centred, kHeaderGold, fontBold, DT_LEFT | DT_WORDBREAK);
    }

    const std::wstring pageText = std::to_wstring(view.listPage + 1) + L"/" + std::to_wstring(pages);
    Text(dc, pageText, layout.pageLabel, kText, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    CornerShadow(dc, layout.prev, S(2));
    FillPanel(dc, layout.prev, kPanel, 200);
    FrameColor(dc, layout.prev, kBorder);
    Text(dc, UiText(L"< Prev"), layout.prev, view.listPage > 0 ? kText : kDim, fontBody,
         DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    CornerShadow(dc, layout.next, S(2));
    FillPanel(dc, layout.next, kPanel, 200);
    FrameColor(dc, layout.next, kBorder);
    Text(dc, UiText(L"Next >"), layout.next, view.listPage + 1 < pages ? kText : kDim, fontBody,
         DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

// True when some block in the entry has `value` in `member` (for example a tier name).
static bool AnyBlock(const Entry& entry, std::string Block::*member, const std::string& value) {
    for (const Block& block : entry.blocks) {
        if (block.*member == value) return true;
    }
    return false;
}

// The filter value if this entry has blocks with it, otherwise "" (show all). Keeps a
// filter chosen on one category from hiding everything in another.
static std::string ActiveFilter(const Entry& entry, std::string Block::*member, const std::string& value) {
    return (!value.empty() && AnyBlock(entry, member, value)) ? value : std::string();
}

// Weapon families present in an entry, in alphabetical order.
static std::vector<std::string> DistinctFamilies(const Entry& entry) {
    std::set<std::string> families;
    std::vector<std::string> ordered;
    for (const Block& block : entry.blocks) {
        if (!block.family.empty() && families.insert(block.family).second) ordered.push_back(block.family);
    }
    if (entry.keepOrder) return ordered;
    return std::vector<std::string>(families.begin(), families.end());
}

// Filter and sort bars. Runewords get a socket filter row and socket sorts. Sortable
// item lists (uniques) get Tier and Type filter rows, then level and name sorts. Chips
// wrap onto another line when a row is full.
int OverlayState::BuildControls(HDC dc, const Entry& entry, PageView& view) {
    layout.chips.clear();
    if (SkillsActive()) return layout.detail.top + S(12);  // the Skills page has its own controls (overlay_skills.inc)
    // Class Crafting has no filters or sorts: its cards stay in the order the page gives them.
    if (CurrentPage().id == "class-crafting") return layout.detail.top + S(12);
    const bool runes = HasRuneBlocks(entry) || entry.socketSort;  // gemwords have a Sockets filter but no runes
    // Tier names (unique tiers, or Hireling difficulties) need the controls even without runes or sorting.
    const bool hasTiers = std::any_of(entry.blocks.begin(), entry.blocks.end(),
                                      [](const Block& block) { return !block.tier.empty(); });
    const bool hasFamilies = std::any_of(entry.blocks.begin(), entry.blocks.end(),
                                         [](const Block& block) { return !block.family.empty(); });
    if (!runes && !entry.sortable && !hasTiers && !hasFamilies) return layout.detail.top + S(16);

    const BlockSort active = ResolveBlockSort(entry, view.sort);
    const std::string tierNow = ActiveFilter(entry, &Block::tier, view.tierFilter);
    const std::string familyNow = ActiveFilter(entry, &Block::family, view.familyFilter);
    const std::vector<std::string> families = DistinctFamilies(entry);

    const int left = layout.detail.left + S(16);
    const int top = layout.detail.top + S(16);
    const int rowHeight = std::max(S(26), smallLine + S(10));
    const int right = layout.detail.right - S(22);
    int labelWidth = 0;
    for (const wchar_t* text : {L"Sockets:", L"Tier:", L"Type:", L"Sort:"}) {
        labelWidth = std::max(labelWidth, MeasureWidth(dc, UiText(text), fontSmall));
    }
    labelWidth += S(12);
    const int chipStart = left + labelWidth;

    auto label = [&](int rowY, const wchar_t* text) {
        Text(dc, UiText(text), Rect(left, rowY, left + labelWidth, rowY + rowHeight), kDim, fontSmall,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    };
    auto chip = [&](int& rowY, int& cursor, const std::wstring& text, int minWidth, ChipKind kind,
                    std::size_t value, const std::string& name, bool selected) {
        const int chipWidth = std::max(minWidth, MeasureWidth(dc, text, fontSmall) + S(16));
        if (cursor > chipStart && cursor + chipWidth > right) {
            cursor = chipStart;
            rowY += rowHeight + S(6);
        }
        const RECT rect = Rect(cursor, rowY, cursor + chipWidth, rowY + rowHeight);
        layout.chips.push_back(Chip{rect, kind, value, name});
        CornerShadow(dc, rect, S(2));
        FillPanel(dc, rect, selected ? kSelected : kPanel, 200);
        FrameColor(dc, rect, selected ? kGold : kBorder);
        Text(dc, text, rect, selected ? kGold : kText, fontSmall, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        cursor += chipWidth + S(6);
    };

    int y = top;
    if (runes) {
        label(y, L"Sockets:");
        int rowY = y, cursor = chipStart;
        chip(rowY, cursor, UiText(L"All"), S(44), ChipKind::Socket, 0, "", view.socketFilter == 0);
        for (std::size_t count : SocketCounts(entry)) {
            chip(rowY, cursor, std::to_wstring(count), S(36), ChipKind::Socket, count, "",
                 view.socketFilter == count);
        }
        y = rowY + rowHeight + S(6);
    }
    // Tier filter: unique tiers, or Hireling difficulties. Only the names present are listed.
    const char* const tierNames[] = {"Normal", "Exceptional", "Elite", "Nightmare", "Hell", "Mythic"};
    {
        if (hasTiers) {
            // The known tiers in their order, then any other name the blocks carry (Item Affixes: Prefix, Suffix, Superior).
            std::vector<std::string> tiers;
            for (const char* name : tierNames)
                if (AnyBlock(entry, &Block::tier, name)) tiers.push_back(name);
            const std::size_t known = tiers.size();
            for (const Block& block : entry.blocks)
                if (!block.tier.empty() && std::find(tiers.begin(), tiers.end(), block.tier) == tiers.end()) tiers.push_back(block.tier);
            label(y, tiers.size() > known ? L"Affix:" : L"Tier:");
            int rowY = y, cursor = chipStart;
            chip(rowY, cursor, UiText(L"All"), S(44), ChipKind::Tier, 0, "", tierNow.empty());
            for (const std::string& name : tiers) chip(rowY, cursor, UiText(Wide(name)), S(80), ChipKind::Tier, 0, name, tierNow == name);
            y = rowY + rowHeight + S(6);
        }
    }
    {
        if (!families.empty()) {
            label(y, L"Type:");
            int rowY = y, cursor = chipStart;
            chip(rowY, cursor, UiText(L"All"), S(44), ChipKind::Family, 0, "", familyNow.empty());
            // Helms on the Gemwords page: the country types go in a row of their own at the top, the other
            // types on a second row below.
            static const std::set<std::string> kCountryTypes = {
                "America", "Austria", "Britain", "Canada", "China", "France", "Germany", "Italy", "Japan", "Portugal", "Russia"};
            std::vector<std::string> ordered = families;
            const bool countryRows = CurrentPage().id == "gemwords" && entry.title == "Helms";
            std::size_t countries = 0;
            if (countryRows) {
                std::stable_partition(ordered.begin(), ordered.end(),
                                      [](const std::string& family) { return kCountryTypes.count(family) > 0; });
                countries = static_cast<std::size_t>(std::count_if(ordered.begin(), ordered.end(),
                                      [](const std::string& family) { return kCountryTypes.count(family) > 0; }));
            }
            for (std::size_t i = 0; i < ordered.size(); ++i) {
                if (countryRows && countries > 0 && i == countries) {
                    rowY += rowHeight + S(6);  // the other types start a second row
                    cursor = chipStart;
                }
                chip(rowY, cursor, UiText(Wide(ordered[i])), S(70), ChipKind::Family, 0, ordered[i], familyNow == ordered[i]);
            }
            y = rowY + rowHeight + S(6);
        }
    }

    // Sort chips only do something for level-sorted lists or titled blocks.
    const bool hasTitles = std::any_of(entry.blocks.begin(), entry.blocks.end(),
                                       [](const Block& block) { return !block.title.empty(); });
    if (entry.socketSort || (!entry.sortable && !hasTitles)) return y;  // gemwords: the Sockets filter only
    label(y, L"Sort:");
    int rowY = y, cursor = chipStart;
    if (entry.rowSort) {  // table rows: each chip cycles ascending, descending, then back to names
        const bool ilvlOn = active == BlockSort::RowMinIlvl || active == BlockSort::RowMaxIlvl;
        const BlockSort ilvlNext = active == BlockSort::RowMinIlvl ? BlockSort::RowMaxIlvl
                                 : active == BlockSort::RowMaxIlvl ? BlockSort::Default : BlockSort::RowMinIlvl;
        const std::wstring ilvlText = active == BlockSort::RowMinIlvl ? UiText(L"iLvl \u2191")
                                    : active == BlockSort::RowMaxIlvl ? UiText(L"iLvl \u2193") : UiText(L"iLvl");
        chip(rowY, cursor, ilvlText, S(80), ChipKind::Sort, static_cast<std::size_t>(ilvlNext), "", ilvlOn);
        const bool rarityOn = active == BlockSort::RowRarity || active == BlockSort::RowRarityHigh;
        const BlockSort rarityNext = active == BlockSort::RowRarity ? BlockSort::RowRarityHigh
                                   : active == BlockSort::RowRarityHigh ? BlockSort::Default : BlockSort::RowRarity;
        const std::wstring rarityText = active == BlockSort::RowRarity ? UiText(L"Rarity \u2191")
                                      : active == BlockSort::RowRarityHigh ? UiText(L"Rarity \u2193") : UiText(L"Rarity");
        chip(rowY, cursor, rarityText, S(90), ChipKind::Sort, static_cast<std::size_t>(rarityNext), "", rarityOn);
        return rowY + rowHeight + S(10);
    }
    if (entry.sortable) {
        chip(rowY, cursor, UiText(L"Lowest level"), S(110), ChipKind::Sort,
             static_cast<std::size_t>(BlockSort::LevelLowFirst), "", active == BlockSort::LevelLowFirst);
        chip(rowY, cursor, UiText(L"Highest level"), S(110), ChipKind::Sort,
             static_cast<std::size_t>(BlockSort::LevelHighFirst), "", active == BlockSort::LevelHighFirst);
    }
    chip(rowY, cursor, UiText(L"A-Z"), S(56), ChipKind::Sort, static_cast<std::size_t>(BlockSort::Name), "",
         active == BlockSort::Name);

    return rowY + rowHeight + S(10);
}

// Scrollbar along the right edge of the detail pane. Records the track and thumb
// so mouse input can drag or page through the content.
void OverlayState::DrawScrollbar(HDC dc, int top, int visible, int total, int scroll) {
    layout.scrollable = total > visible && visible > 0;
    layout.scrollTrack = Rect(layout.detail.right - S(16), top, layout.detail.right - S(8), layout.detail.bottom - S(8));
    if (!layout.scrollable) return;
    const RECT track = layout.scrollTrack;
    const int trackHeight = track.bottom - track.top;
    const int thumbHeight = std::max(S(28), trackHeight * visible / total);
    const int maxScroll = total - visible;
    const int thumbTop = track.top + static_cast<int>(static_cast<long long>(trackHeight - thumbHeight) * scroll / maxScroll);
    layout.scrollThumb = Rect(track.left, thumbTop, track.right, thumbTop + thumbHeight);
    CornerShadow(dc, track, S(2));
    FillPanel(dc, track, kSearchFill, 200);
    FillColor(dc, layout.scrollThumb, dragging ? kGold : kBorder);
}

// Height of one item tile: name, art, then its rune icons.
// Height of the tile's tag under the art (for example "(Coupon only)"), with its gap; 0 when there is none.
int OverlayState::TagRowHeight(HDC dc, const Block& block, const TileStyle& style) {
    if (block.tag.empty()) return 0;
    return MeasureHeight(dc, Wide(block.tag), style.tileWidth - S(12), fontSmall) + S(4);
}

// Height of the picture area of a tile: rings, amulets, jewels and small and medium charms use 50% of it.
static int ArtBoxFor(const Block& block, const TileStyle& style) {
    return block.tinyArt ? std::max(1, style.artBox * 50 / 100) : style.artBox;
}

int OverlayState::TileHeight(HDC dc, const Block& block, const TileStyle& style) {
    const int nameHeight = MeasureHeight(dc, Wide(block.title), style.tileWidth - S(12), style.nameFont);
    const int runeRows = RuneRows(block.runes.size(), style);
    return style.padding + nameHeight + style.padding + ArtBoxFor(block, style) + TagRowHeight(dc, block, style) +
           (runeRows > 0 ? S(8) + runeRows * RuneRowHeight(style) : 0) + S(10);
}

// Lays out item blocks as tiles: name over art, with the rune sequence under it.
// Tile hit boxes are added after the tile's rune icons, so a rune icon shows the
// rune tooltip and the rest of the tile shows the item tooltip. Hit boxes that are
// scrolled out of the visible band are skipped.
int OverlayState::RenderTiles(HDC dc, Gdiplus::Graphics* graphics, const Entry& entry,
                              const std::vector<std::size_t>& order, std::size_t begin, std::size_t end, int left,
                              int width_, int y, bool compact, bool draw) {
    gKanjiRuneColors = entry.title != "LoD Runes";
    if (entry.wide) return RenderWideTiles(dc, graphics, entry, order, begin, end, left, width_, y, draw);
    const TileStyle style = MakeTileStyle(width_, compact, entry.fiveAcross);
    const std::size_t step = static_cast<std::size_t>(style.columns);

    for (std::size_t first = begin; first < end; first += step) {
        if (draw && y > clipBottom) break;  // rows below the band are not drawn
        const std::size_t count = std::min<std::size_t>(step, end - first);
        int rowHeight = 0;
        for (std::size_t c = 0; c < count; ++c) {
            rowHeight = std::max(rowHeight, TileHeight(dc, entry.blocks[order[first + c]], style));
        }
        for (std::size_t c = 0; c < count; ++c) {
            const std::size_t index = order[first + c];
            const Block& block = entry.blocks[index];
            const int tileLeft = left + static_cast<int>(c) * (style.tileWidth + style.gap);
            const RECT tile = Rect(tileLeft, y, tileLeft + style.tileWidth, y + rowHeight);
            if (!draw && !query.empty() && BlockMatches(block, query)) matchTops.push_back(y - matchBase);
            if (!draw) blockSpans.push_back({static_cast<int>(index), y - matchBase, y + rowHeight - matchBase});
            const bool onScreen = tile.bottom > clipTop && tile.top < clipBottom;
            if (draw && onScreen) {
                CornerShadow(dc, tile, S(2));
                FillPanel(dc, tile, kTile, 150);
                FrameColor(dc, tile, kBorder);
            }

            const int nameHeight = MeasureHeight(dc, Wide(block.title), style.tileWidth - S(12), style.nameFont);
            // Rings and amulets are shorter than the tallest tile in their row, so their contents sit in the middle.
            const int cy = y + (block.tinyArt ? (rowHeight - TileHeight(dc, block, style)) / 2 : 0);
            if (draw && onScreen && nameHeight > 0) {
                Text(dc, Wide(block.title),
                     Rect(tileLeft + S(6), cy + style.padding, tileLeft + style.tileWidth - S(6),
                          cy + style.padding + nameHeight),
                     entry.title == "LoD Runes" ? CodeColor('8') : itemNameColor, style.nameFont,
                     DT_CENTER | DT_WORDBREAK);
            }

            const int artTop = cy + style.padding + nameHeight + style.padding;
            const int box = ArtBoxFor(block, style);
            const int artSize = block.smallArt ? box / 2 : box;
            const int artLeft = tileLeft + (style.tileWidth - artSize) / 2;
            const int artDrawTop = artTop + (box - artSize) / 2;
            const RECT art = Rect(artLeft, artDrawTop, artLeft + artSize, artDrawTop + artSize);
            if (draw && onScreen) {
                for (const std::string& key : block.images) DrawImageKey(*graphics, key, art);
            }
            const int tagRow = TagRowHeight(dc, block, style);
            if (draw && onScreen && !block.tag.empty()) {
                const RECT tagBox = Rect(tileLeft + S(6), artTop + box + S(4),
                                         tileLeft + style.tileWidth - S(6), artTop + box + tagRow);
                Text(dc, Wide(block.tag), tagBox, kGold, fontSmall, DT_CENTER | DT_WORDBREAK);
            }

            if (!block.runes.empty()) {
                const int inner = style.tileWidth - S(12);
                const int perRow = std::max(1, inner / style.cell);
                const int icon = style.cell - S(4);
                const int runeTop = artTop + box + S(8) + tagRow;
                const int total = static_cast<int>(block.runes.size());
                for (int r = 0; r < total; ++r) {
                    const int row = r / perRow;
                    const int col = r % perRow;
                    const int inRow = std::min(perRow, total - row * perRow);
                    const int x = tileLeft + (style.tileWidth - inRow * style.cell) / 2 + col * style.cell;
                    const int rowTop = runeTop + row * RuneRowHeight(style);
                    if (draw && onScreen) {
                        const RECT iconBox = Rect(x + S(2), rowTop, x + S(2) + icon, rowTop + icon);
                        detailHits.push_back(DetailHit{iconBox, &entry, &block, r});
                        DrawImageKey(*graphics, block.runes[r].imageKey, iconBox);
                        const std::wstring label = Wide(block.runes[r].label);
                        // Jewel and gem sockets show their name in the tooltip only.
                        if (label.rfind(L"Any ", 0) != 0) {
                            Text(dc, label, Rect(x, rowTop + icon, x + style.cell, rowTop + icon + smallLine),
                                 block.runes[r].lod ? CodeColor('8') : KanjiLabelColor(label, kGold), fontSmall,
                                 DT_CENTER | DT_SINGLELINE);
                        }
                    }
                }
            }

            if (draw && tile.bottom > clipTop && tile.top < clipBottom) {
                detailHits.push_back(DetailHit{art, &entry, &block, -1});  // the picture itself, not the whole tile
                cardHits.push_back(CardHit{tile, &entry, &block});
            }
        }
        y += rowHeight + style.gap;
    }
    return y;
}

// Wide entries (mythical uniques): one card per row, with the card's stats and notes
// in a column beside it. The name is drawn in purple to mark the rarity.
int OverlayState::RenderWideTiles(HDC dc, Gdiplus::Graphics* graphics, const Entry& entry,
                                  const std::vector<std::size_t>& order, std::size_t begin, std::size_t end,
                                  int left, int width_, int y, bool draw) {
    const int gap = S(12);
    const int pad = S(6);
    const int sideWidth = std::max(S(220), width_ * 2 / 5);
    const int tileWidth = std::max(S(160), width_ - sideWidth - gap);
    const int sideLeft = left + tileWidth + gap;
    const int inner = tileWidth - S(12);
    const int artBox = std::min(inner, 2 * std::min(S(128), std::max(S(64), inner / 2)));  // twice the usual art size

    for (std::size_t i = begin; i < end; ++i) {
        if (draw && y > clipBottom) break;  // rows below the band are not drawn
        const Block& block = entry.blocks[order[i]];
        const int nameHeight = MeasureHeight(dc, Wide(block.title), inner, fontBold);

        // The side column holds only the notes, laid out like a guide block. Stats are in the tooltip.
        Block side;
        if (!block.note.empty()) side.lines.push_back(Line{LineKind::Text, block.note});
        const int sideTop = y + pad;
        const int sideHeight = RenderGuideBlock(dc, graphics, side, sideLeft + pad, sideWidth - 2 * pad, sideTop, false)
                               - sideTop + pad;
        const int cardHeight = pad + nameHeight + pad + artBox + pad;
        const int rowHeight = std::max(cardHeight, sideHeight);

        if (!draw) blockSpans.push_back({static_cast<int>(order[i]), y - matchBase, y + rowHeight - matchBase});
        const RECT tile = Rect(left, y, left + tileWidth, y + rowHeight);
        const RECT column = Rect(sideLeft, y, sideLeft + sideWidth, y + rowHeight);
        const bool onScreen = tile.bottom > clipTop && tile.top < clipBottom;
        if (draw && onScreen) {
            CornerShadow(dc, tile, S(2));
            FillPanel(dc, tile, kTile, 150);
            FrameColor(dc, tile, kBorder);
            CornerShadow(dc, column, S(2));
            FillPanel(dc, column, kTile, 150);
            FrameColor(dc, column, kBorder);
            Text(dc, Wide(block.title), Rect(left + pad, y + pad, left + tileWidth - pad, y + pad + nameHeight),
                 CodeColor(';'), fontBold, DT_CENTER | DT_WORDBREAK);
            const int artSize = block.smallArt ? artBox / 2 : artBox;
            const int artLeft = left + (tileWidth - artSize) / 2;
            const int artTop = y + pad + nameHeight + pad + (artBox - artSize) / 2;
            const RECT art = Rect(artLeft, artTop, artLeft + artSize, artTop + artSize);
            for (const std::string& key : block.images) DrawImageKey(*graphics, key, art);
            RenderGuideBlock(dc, graphics, side, sideLeft + pad, sideWidth - 2 * pad, sideTop, true);
        }
        if (draw && onScreen) {
            const int artSize = block.smallArt ? artBox / 2 : artBox;
            const int artLeft = left + (tileWidth - artSize) / 2;
            const int artTop = y + pad + nameHeight + pad + (artBox - artSize) / 2;
            detailHits.push_back(DetailHit{Rect(artLeft, artTop, artLeft + artSize, artTop + artSize), &entry, &block, -1});
            cardHits.push_back(CardHit{tile, &entry, &block});
        }
        y += rowHeight + gap;
    }
    return y;
}

// Non-item guide block: its title, subtitle, images, fields and text lines.
// Splits a table row into its cells (U+001F separates them; U+001E inside a cell is a line break).
std::vector<std::string> SplitCells(const std::string& text) {
    std::vector<std::string> cells(1);
    for (const char c : text) {
        if (c == '\x1f') cells.emplace_back();
        else if (c == '\x1e') cells.back().push_back('\n');
        else cells.back().push_back(c);
    }
    return cells;
}

// Draws consecutive table rows as a bordered grid. Each column is as wide as its longest
// cell, so long recipe text gets the room; the header row uses the bold font and gold.
int OverlayState::RenderTable(HDC dc, const std::vector<Line>& lines, std::size_t first, std::size_t last, int left,
                              int width_, int y, bool draw) {
    std::vector<std::vector<std::string>> rows;
    std::size_t columns = 0;
    for (std::size_t i = first; i < last; ++i) {
        rows.push_back(SplitCells(lines[i].text));
        columns = std::max(columns, rows.back().size());
    }
    if (columns == 0) return y;

    std::vector<std::size_t> weight(columns, 1);
    for (const auto& row : rows) {
        for (std::size_t c = 0; c < row.size(); ++c) {
            // Weighted by the longest visible line of the cell (colour codes take no room), since line breaks
            // stack text vertically.
            std::string visible;
            COLORREF ignored = kBody;
            PlainText(row[c], visible, ignored, kBody);
            std::size_t longest = 0, current = 0;
            for (const char ch : visible) {
                if (ch == '\n') current = 0;
                else longest = std::max(longest, ++current);
            }
            weight[c] = std::max(weight[c], longest);
        }
    }
    std::size_t total = 0;
    for (const std::size_t w : weight) total += w;
    std::vector<int> colWidth(columns, 0);
    int used = 0;
    for (std::size_t c = 0; c + 1 < columns; ++c) {
        colWidth[c] = static_cast<int>(static_cast<std::size_t>(width_) * weight[c] / total);
        used += colWidth[c];
    }
    colWidth[columns - 1] = width_ - used;

    const int pad = S(4);
    // Text and colour of one cell; the header row uses gold.
    auto cellText = [&](std::size_t r, std::size_t c, COLORREF& color) {
        const bool header = lines[first + r].kind == LineKind::Header;
        color = header ? kGold : kBody;
        std::string plain;
        if (c < rows[r].size()) PlainText(rows[r][c], plain, color, header ? kGold : kBody);
        if (header) color = kGold;
        return header ? UiText(Wide(plain)) : Wide(plain);
    };

    std::string key = std::to_string(width_);
    for (std::size_t i = first; i < last; ++i) {
        key += '|';
        key += static_cast<char>(lines[i].kind);
        key += lines[i].text;
    }
    // Widen any column whose longest unbreakable word would otherwise be clipped. The extra
    // room comes out of the wider columns in proportion to their width.
    {
        std::vector<int> need(columns, 0);
        const auto knownNeed = tableNeedCache.find(key);
        const bool measureNeed = knownNeed == tableNeedCache.end();
        if (!measureNeed) need = knownNeed->second;
        const HFONT previous = static_cast<HFONT>(SelectObject(dc, fontBody));
        for (std::size_t r = 0; measureNeed && r < rows.size(); ++r) {
            for (std::size_t c = 0; c < columns; ++c) {
                COLORREF color = kBody;
                const std::wstring text = cellText(r, c, color);
                // Each line of the cell stays on one line when it fits in part of the table (up to two fifths
                // of its width), so a short ingredient such as a token name does not wrap.
                std::size_t lineStart = 0;
                while (lineStart <= text.size()) {
                    std::size_t lineEnd = text.find(L'\n', lineStart);
                    if (lineEnd == std::wstring::npos) lineEnd = text.size();
                    if (lineEnd > lineStart) {
                        SIZE size{};
                        GetTextExtentPoint32W(dc, text.c_str() + lineStart, static_cast<int>(lineEnd - lineStart), &size);
                        const int wanted = std::min(static_cast<int>(size.cx) + 2 * pad + S(4), width_ * 2 / 5);
                        need[c] = std::max(need[c], wanted);
                    }
                    lineStart = lineEnd + 1;
                }
                std::size_t begin = 0;
                while (begin <= text.size()) {
                    std::size_t end = text.find(L' ', begin);
                    if (end == std::wstring::npos) end = text.size();
                    if (end > begin) {
                        SIZE size{};
                        GetTextExtentPoint32W(dc, text.c_str() + begin, static_cast<int>(end - begin), &size);
                        need[c] = std::max(need[c], static_cast<int>(size.cx) + 2 * pad + S(4));
                    }
                    begin = end + 1;
                }
            }
        }
        SelectObject(dc, previous);
        if (measureNeed) tableNeedCache.emplace(key, need);
        int deficit = 0;
        for (std::size_t c = 0; c < columns; ++c) {
            if (colWidth[c] < need[c]) { deficit += need[c] - colWidth[c]; colWidth[c] = need[c]; }
        }
        int spare = 0;
        for (std::size_t c = 0; c < columns; ++c) {
            if (colWidth[c] > need[c]) spare += colWidth[c] - need[c];
        }
        if (deficit > 0 && spare > 0) {
            for (std::size_t c = 0; c < columns; ++c) {
                if (colWidth[c] > need[c]) {
                    const int give = std::min(colWidth[c] - need[c], deficit * (colWidth[c] - need[c]) / spare);
                    colWidth[c] -= give;
                }
            }
            int sum = 0;
            for (const int w : colWidth) sum += w;
            colWidth[columns - 1] += width_ - sum;
        }
    }

    // Raw cell text with its colour codes. Non-header cells with colour codes are laid out piece by
    // piece, so each word can keep its own colour (as paragraphs do), not one colour per cell.
    auto rawCell = [&](std::size_t r, std::size_t c) -> std::string {
        return c < rows[r].size() ? rows[r][c] : std::string();
    };
    auto multiColour = [&](std::size_t r, std::size_t c) {
        return lines[first + r].kind != LineKind::Header && HasColorCodes(rawCell(r, c));
    };
    auto cellHeight = [&](std::size_t r, std::size_t c, int w, HFONT f) -> int {
        if (multiColour(r, c)) return LayoutPieces(dc, LinePieces(rawCell(r, c), kBody), kBody, Rect(0, 0, w, 0), f, DT_LEFT, false);
        COLORREF color = kBody;
        return MeasureHeight(dc, cellText(r, c, color), w, f);
    };

    auto cached = tableHeightCache.find(key);
    if (cached == tableHeightCache.end()) {
        std::vector<int> measured;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const HFONT font = lines[first + r].kind == LineKind::Header ? fontBold : fontBody;
            int rowHeight = 0;
            for (std::size_t c = 0; c < columns; ++c) {
                rowHeight = std::max(rowHeight, cellHeight(r, c, std::max(S(20), colWidth[c] - 2 * pad), font));
            }
            measured.push_back(rowHeight + 2 * pad);
        }
        cached = tableHeightCache.emplace(key, std::move(measured)).first;
    }
    const std::vector<int>& heights = cached->second;

    for (std::size_t r = 0; r < rows.size(); ++r) {
        const int rowHeight = heights[r];
        // Rows outside the visible band are positioned but not drawn.
        if (draw && y + rowHeight > clipTop && y < clipBottom) {
            const HFONT font = lines[first + r].kind == LineKind::Header ? fontBold : fontBody;
            int x = left;
            for (std::size_t c = 0; c < columns; ++c) {
                COLORREF color = kBody;
                const std::wstring text = cellText(r, c, color);
                const RECT cell = Rect(x, y, x + colWidth[c], y + rowHeight);
                FrameColor(dc, cell, kBorder);
                RECT inner = Rect(x + pad, y + pad, x + colWidth[c] - pad, y + rowHeight - pad);
                // Centre the cell's text vertically, so a short cell next to a tall one sits mid-row, not at the top.
                const int contentHeight = cellHeight(r, c, std::max(S(20), colWidth[c] - 2 * pad), font);
                const int spare = (inner.bottom - inner.top) - contentHeight;
                if (spare > 0) inner.top += spare / 2;
                if (multiColour(r, c)) LayoutPieces(dc, LinePieces(rawCell(r, c), kBody), kBody, inner, font, DT_LEFT, true);
                else Text(dc, text, inner, color, font, DT_LEFT | DT_WORDBREAK);
                x += colWidth[c];
            }
        }
        y += rowHeight;
    }
    return y + S(6);
}

// A line that is only a web address (https://...) is drawn as a link: a click opens it in the browser.
static bool IsWebLink(const std::string& text) {
    if (text.rfind("https://", 0) != 0 || text.size() < 12) return false;
    for (const char c : text)
        if (static_cast<unsigned char>(c) <= ' ') return false;
    return true;
}

// The link under (x, y) in the page's text, or -1.
int OverlayState::LinkAt(int x, int y) const {
    if (!Inside(layout.detail, x, y) || y < clipTop || y >= clipBottom) return -1;
    for (std::size_t i = 0; i < linkHits.size(); ++i)
        if (Inside(linkHits[i].rect, x, y)) return static_cast<int>(i);
    return -1;
}

bool OverlayState::LinkClick(int x, int y) {
    const int index = LinkAt(x, y);
    if (index < 0) return false;
    const std::wstring url = linkHits[static_cast<std::size_t>(index)].url;
    if (url.rfind(L"https://", 0) != 0) return true;  // only web addresses
    DebugLog("opening link: " + Narrow(url));
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

int OverlayState::RenderGuideBlock(HDC dc, Gdiplus::Graphics* graphics, const Block& block, int left, int width_,
                                   int y, bool draw) {
    if (splitNow != 0 && !block.images.empty()) return RenderSplitBlock(dc, graphics, block, left, width_, y, draw);
    if (!block.title.empty()) {
        const std::wstring wide = Wide(block.title);
        const int h = MeasureHeight(dc, wide, width_, fontBold);
        if (draw) Text(dc, wide, Rect(left, y, left + width_, y + h), block.item ? itemNameColor : kGold, fontBold,
                       DT_LEFT | DT_WORDBREAK);
        y += h + S(2);
    }
    if (!block.subtitle.empty()) {
        const std::wstring wide = Wide(block.subtitle);
        const int h = MeasureHeight(dc, wide, width_, fontSmall);
        if (draw) Text(dc, wide, Rect(left, y, left + width_, y + h), kDim, fontSmall, DT_LEFT | DT_WORDBREAK);
        y += h + S(4);
    }

    if (!block.images.empty() && block.originalSize) {
        // Drawn at the picture's own pixel size (scaled for the display DPI), one per row, and shrunk
        // only when it is wider than the detail panel.
        for (const std::string& key : block.images) {
            Gdiplus::Bitmap* bitmap = FindImage(key);
            int imageW = S(128);
            int imageH = S(128);
            if (bitmap != nullptr && bitmap->GetWidth() > 0 && bitmap->GetHeight() > 0) {
                imageW = S(static_cast<int>(bitmap->GetWidth()));
                imageH = S(static_cast<int>(bitmap->GetHeight()));
                if (imageW > width_) {
                    imageH = imageH * width_ / imageW;
                    imageW = width_;
                }
            }
            const int x = left + (width_ - imageW) / 2;
            if (draw && y + imageH > clipTop && y < clipBottom) DrawImageKey(*graphics, key, Rect(x, y, x + imageW, y + imageH));
            y += imageH + S(8);
        }
        y += S(4);
    } else if (!block.images.empty()) {
        const int imageBox = std::min(S(128), std::max(S(64), width_ / 3));
        const int perRow = std::max(1, width_ / (imageBox + S(10)));
        for (std::size_t i = 0; i < block.images.size(); ++i) {
            const int col = static_cast<int>(i) % perRow;
            const int row = static_cast<int>(i) / perRow;
            const int x = left + col * (imageBox + S(10));
            const int top = y + row * (imageBox + S(8));
            if (draw && top + imageBox > clipTop && top < clipBottom) {
                DrawImageKey(*graphics, block.images[i], Rect(x, top, x + imageBox, top + imageBox));
            }
        }
        const int rows = static_cast<int>((block.images.size() + perRow - 1) / perRow);
        y += rows * (imageBox + S(8)) + S(4);
    }

    for (const Field& field : block.fields) {
        const std::wstring wide = UiDataText(Wide(field.label)) + L": " + Wide(field.value);
        const int h = MeasureHeight(dc, wide, width_, fontBody);
        if (draw) Text(dc, wide, Rect(left, y, left + width_, y + h), kBody, fontBody, DT_LEFT | DT_WORDBREAK);
        y += h + S(3);
    }
    if (!block.fields.empty()) y += S(4);

    const std::vector<Line>* shownLines = &block.lines;
    if (IsRowSort(rowSortNow)) {
        if (sortedRowsFor != rowSortNow || sortedRowsDb != database) {
            sortedRows.clear();
            sortedRowsFor = rowSortNow;
            sortedRowsDb = database;
        }
        auto found = sortedRows.find(&block);
        if (found == sortedRows.end()) found = sortedRows.emplace(&block, SortTableRows(block.lines, rowSortNow)).first;
        shownLines = &found->second;
    }
    const std::vector<Line>& blockLines = *shownLines;
    for (std::size_t i = 0; i < blockLines.size();) {
        const Line& line = blockLines[i];
        if (line.kind == LineKind::Row || line.kind == LineKind::Header) {
            std::size_t end = i;
            while (end < blockLines.size() && (blockLines[end].kind == LineKind::Row ||
                                               blockLines[end].kind == LineKind::Header)) {
                ++end;
            }
            y = RenderTable(dc, blockLines, i, end, left, width_, y, draw);
            i = end;
            continue;
        }
        const bool heading = line.kind == LineKind::Heading;
        const bool groupLabel = line.kind == LineKind::Label;  // a small dim sub-heading (socketable bonus group)
        if (heading) y += S(6);
        const COLORREF base = heading ? kHeaderGold : (groupLabel ? kDim : kBody);
        const HFONT font = heading ? fontBold : (groupLabel ? fontSmall : fontBody);
        std::string plain;
        COLORREF unused = base;
        PlainText(line.text, plain, unused, base);
        const std::wstring wide = Wide(plain);
        if (!heading && !groupLabel && IsWebLink(plain)) {
            const int linkH = MeasureHeight(dc, wide, width_, font);
            if (draw) {
                const bool oneLine = linkH <= bodyLine + S(4);
                const int textW = std::min(NaturalWidth(dc, wide, font), width_);
                const bool hot = static_cast<int>(linkHits.size()) == linkHot;
                const COLORREF linkColor = hot ? RGB(176, 214, 255) : RGB(122, 172, 236);
                Text(dc, wide, Rect(left, y, left + width_, y + linkH), linkColor, font, DT_LEFT | DT_WORDBREAK);
                if (oneLine) FillColor(dc, Rect(left, y + linkH - S(3), left + textW, y + linkH - S(3) + std::max(1, S(1))), linkColor);
                linkHits.push_back({Rect(left, y, left + (oneLine ? textW : width_), y + linkH), wide});
            }
            y += linkH + S(5);
            ++i;
            continue;
        }
        const int h = HasColorCodes(line.text)
                          ? LayoutPieces(dc, LinePieces(line.text, base), base, Rect(0, 0, width_, 0), font, DT_LEFT, false)
                          : MeasureHeight(dc, wide, width_, font);
        if (draw) {
            const RECT box = Rect(left, y, left + width_, y + h);
            if (HasColorCodes(line.text)) LayoutPieces(dc, LinePieces(line.text, base), base, box, font, DT_LEFT, true);
            else Text(dc, wide, box, base, font, DT_LEFT | DT_WORDBREAK);
        }
        y += h + S(5);
        ++i;
    }
    return y;
}

// Lays out one entry: its title, then its blocks in `order`. Item blocks become
// tile grids, split into runs by section header when `headers` is set. With draw ==
// false it only measures, so the caller can clamp the scroll offset first. The
// content right edge leaves room for the scrollbar.
int OverlayState::RenderDetail(HDC dc, const Entry& entry, const std::vector<std::size_t>& order, bool headers,
                               int top, int scroll, bool compact, bool draw) {
    if (!draw) { matchTops.clear(); blockSpans.clear(); campaignSpots.clear(); }
    else {
        campaignHits.clear();
        linkHits.clear();
        linkHot = -1;
    }
    if (CalcActive()) return RenderCalculator(dc, top, scroll, draw);
    if (SpecActive()) return RenderSpecializations(dc, top, scroll, draw);
    if (SkillsActive()) return RenderSkills(dc, entry, top, scroll, draw);
    const RECT area = layout.detail;
    const int left = area.left + S(16);
    const int width_ = area.right - S(22) - left;
    int y = top - scroll;
    const int origin = y;
    if (!draw) matchBase = origin;

    std::unique_ptr<Gdiplus::Graphics> owned;
    Gdiplus::Graphics* graphics = nullptr;
    if (draw) {
        owned = std::make_unique<Gdiplus::Graphics>(dc);
        owned->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        // GDI+ does not always honor the device context clip, so clip it here too.
        owned->SetClip(Gdiplus::Rect(area.left + 1, clipTop, area.right - S(18) - area.left - 1, clipBottom - clipTop));
        graphics = owned.get();
    }

    const int titleHeight = titleLine + S(8);
    if (draw) Text(dc, UiDataText(Wide(entry.title)), Rect(left, y, left + width_, y + titleHeight), kHeaderGold, fontTitle,
                   DT_LEFT | DT_SINGLELINE);
    y += titleHeight + S(2);
    if (!entry.subtitle.empty()) {
        if (draw) Text(dc, TranslateCounts(Wide(entry.subtitle)), Rect(left, y, left + width_, y + S(20)), kDim, fontSmall,
                       DT_LEFT | DT_SINGLELINE);
        y += S(24);
    }
    y += S(4);

    // The Campaign Progression entry: the map is drawn under its text (and stays when the text does not match a search
    // that an area matches).
    const bool campaignHere = CurrentPage().id == "welcome" && IsCampaignEntry(entry);
    if (order.empty() && !(campaignHere && campaignData)) {
        // Tall enough for the font's full line, so the text is not cut off at the bottom.
        const int noMatchHeight = std::max(S(24), bodyLine + S(6));
        if (draw) Text(dc, UiText(L"Nothing matches this filter"), Rect(left, y, left + width_, y + noMatchHeight), kDim, fontBody,
                       DT_LEFT | DT_SINGLELINE);
        return y + noMatchHeight - origin;
    }

    bool firstBlock = true;
    std::size_t i = 0;
    while (i < order.size()) {
        if (draw && y > clipBottom) break;  // everything below is off screen
        const Block& first = entry.blocks[order[i]];
        if (!first.item) {
            if (!firstBlock) {
                if (draw) FillColor(dc, Rect(left, y, left + width_, y + 1), kBorder);
                y += S(12);
            }
            if (!draw && !query.empty() && BlockMatches(first, query)) matchTops.push_back(y - matchBase);
            const int guideTop = y;
            y = RenderGuideBlock(dc, graphics, first, left, width_, y, draw);
            if (!draw) blockSpans.push_back({static_cast<int>(order[i]), guideTop - matchBase, y - matchBase});
            y += S(6);
            firstBlock = false;
            ++i;
            continue;
        }

        if (headers && !first.section.empty()) {
            // A skill tree's title comes from the skills data, already in the language.
            const std::wstring header = SkillsActive() ? UiDataText(Wide(first.section)) : UiText(Wide(first.section));
            const int h = MeasureHeight(dc, header, width_, fontBold);
            if (draw) Text(dc, header, Rect(left, y, left + width_, y + h), greenNames ? CodeColor('2') : kHeaderGold, fontBold, DT_LEFT | DT_WORDBREAK);
            y += h + S(2);
            if (draw) FillColor(dc, Rect(left, y, left + width_, y + 1), kBorder);
            y += S(8);
        }

        std::size_t end = i;
        while (end < order.size()) {
            const Block& candidate = entry.blocks[order[end]];
            if (!candidate.item) break;
            if (headers && candidate.section != first.section) break;
            ++end;
        }
        y = RenderTiles(dc, graphics, entry, order, i, end, left, width_, y, compact || entry.compact, draw);
        y += S(6);
        firstBlock = false;
        i = end;
    }
    if (campaignHere) y = RenderCampaign(dc, graphics, left, width_, y, draw);
    return y - origin;
}

int OverlayState::RenderSplitBlock(HDC dc, Gdiplus::Graphics* graphics, const Block& block, int left, int width_, int y,
                                   bool draw) {
    const std::vector<Line>& lines = block.lines;
    std::size_t first = 0;
    while (first < lines.size() && lines[first].kind != LineKind::Stat) ++first;
    std::size_t last = first;
    while (last < lines.size() && lines[last].kind == LineKind::Stat) ++last;

    // One line, centred in a column. Returns the height it takes.
    const auto centred = [&](const Line& line, HFONT font, int x, int width, int top, bool paint) {
        std::string plain;
        COLORREF unused = kBody;
        PlainText(line.text, plain, unused, kBody);
        const std::wstring wide = Wide(plain);
        const int h = HasColorCodes(line.text)
                          ? LayoutPieces(dc, LinePieces(line.text, kBody), kBody, Rect(0, 0, width, 0), font, DT_CENTER, false)
                          : MeasureHeight(dc, wide, width, font);
        if (paint) {
            const RECT box = Rect(x, top, x + width, top + h);
            if (HasColorCodes(line.text)) LayoutPieces(dc, LinePieces(line.text, kBody), kBody, box, font, DT_CENTER, true);
            else Text(dc, wide, box, kBody, font, DT_CENTER | DT_WORDBREAK);
        }
        return h + S(3);
    };

    // Lines of the block outside the columns, full width and centred; an Input / Output table is centred and no wider
    // than needed.
    const auto flow = [&](std::size_t from, std::size_t to, HFONT font) {
        for (std::size_t i = from; i < to;) {
            if (lines[i].kind == LineKind::Row || lines[i].kind == LineKind::Header) {
                std::size_t end = i;
                while (end < to && (lines[end].kind == LineKind::Row || lines[end].kind == LineKind::Header)) ++end;
                const int tableW = std::min(width_, S(640));
                y = RenderTable(dc, lines, i, end, left + (width_ - tableW) / 2, tableW, y, draw);
                i = end;
            } else {
                y += centred(lines[i], font, left, width_, y, draw);
                ++i;
            }
        }
    };
    flow(0, first, fontBody);
    if (first > 0) y += S(8);

    const int gap = S(16);
    const int columnW = (width_ - gap) / 2;
    const bool pictureLeft = splitNow == 2;
    const int textX = pictureLeft ? left + columnW + gap : left;
    const int pictureX = pictureLeft ? left : left + columnW + gap;

    int textH = 0;
    for (std::size_t i = first; i < last; ++i) textH += centred(lines[i], fontBody, textX, columnW, 0, false);

    const std::wstring splitTitle = Wide(splitTitleNow);
    const int titleH = MeasureHeight(dc, splitTitle, columnW, fontBold);
    int pictureW = std::min(columnW, S(300));
    int pictureH = pictureW;
    Gdiplus::Bitmap* bitmap = FindImage(block.images[0]);
    if (bitmap != nullptr && bitmap->GetWidth() > 0 && bitmap->GetHeight() > 0) {
        pictureH = pictureW * static_cast<int>(bitmap->GetHeight()) / static_cast<int>(bitmap->GetWidth());
    }
    const int pictureColumnH = titleH + S(8) + pictureH;
    const int rowH = std::max(textH, pictureColumnH);

    if (draw) {
        int top = y + (rowH - textH) / 2;
        for (std::size_t i = first; i < last; ++i) top += centred(lines[i], fontBody, textX, columnW, top, true);
        const int pictureTop = y + (rowH - pictureColumnH) / 2;
        Text(dc, splitTitle, Rect(pictureX, pictureTop, pictureX + columnW, pictureTop + titleH), kHeaderGold, fontBold,
             DT_CENTER | DT_WORDBREAK);
        const int imageLeft = pictureX + (columnW - pictureW) / 2;
        const int imageTop = pictureTop + titleH + S(8);
        if (imageTop + pictureH > clipTop && imageTop < clipBottom) {
            DrawImageKey(*graphics, block.images[0], Rect(imageLeft, imageTop, imageLeft + pictureW, imageTop + pictureH));
        }
    }
    y += rowH + S(8);

    flow(last, lines.size(), fontSmall);  // footnotes, then the recipe table
    return y;
}

#include "overlay_calc.inc"
#include "overlay_spec.inc"
#include "overlay_skills.inc"
#include "overlay_campaign.inc"
#include "overlay_campaign_live.inc"
#include "overlay_suggest.inc"
#include "overlay_switch.inc"

std::vector<std::size_t> OverlayState::ArrangedOrder(const Entry& entry, std::size_t entryIndex,
                                                    const PageView& view) {
    ArrangeKey key{database, pageIndex, entryIndex, view.socketFilter,
                   ActiveFilter(entry, &Block::tier, view.tierFilter),
                   ActiveFilter(entry, &Block::family, view.familyFilter), query, view.sort};
    if (!arrangeValid || !(key == arrangeKey)) {
        arrangeOrder = ArrangeBlocks(entry, key.socket, key.tier, key.family, key.sort, key.query);
        arrangeKey = key;
        arrangeValid = true;
    }
    return arrangeOrder;
}

void OverlayState::Paint(HDC target) {
    RECT client{};
    GetClientRect(window, &client);
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;

    const HDC dc = CreateCompatibleDC(target);
    const HBITMAP bitmap = CreateCompatibleBitmap(target, clientWidth, clientHeight);
    const HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    layout = ComputeLayout();
    EnsureBackgroundCache(target, clientWidth, clientHeight);
    BitBlt(dc, 0, 0, clientWidth, clientHeight, backgroundDc, 0, 0, SRCCOPY);

    const Page& page = CurrentPage();
    greenNames = (page.id == "set-items");
    gemwordCards = page.id == "gemwords";
    const bool goldItems = page.id == "uniques" || page.id == "runewords" || page.id == "gemwords";
    itemNameColor = page.id == "set-items" ? CodeColor('2') : page.id == "class-crafting" ? CodeColor('8')
                  : goldItems ? CodeColor('4') : kGold;
    tooltipNameColor = goldItems || page.id == "class-crafting" ? itemNameColor : kGold;

    // Stacked dark copies at growing offsets make a heavier shadow, which shows on the dark header.
    for (int offset : {S(1), S(2), S(3)}) ShadowText(dc, title, layout.title, fontTitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER, offset);
    Text(dc, title, layout.title, kHeaderGold, fontTitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    for (std::size_t i = 0; i < switches.size(); ++i) {
        DropShadow(dc, layout.switchButtons[i], S(2));
        DrawSwitchButton(dc, layout.switchButtons[i], switches[i].label, switches[i].current);
    }
    DropShadow(dc, layout.close, S(2));
    FillPanel(dc, layout.close, kPanel, 200);
    FrameColor(dc, layout.close, kBorder);
    Text(dc, L"X", layout.close, kText, fontBold, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    {
        const RECT box = SearchBoxRect(layout);
        CornerShadow(dc, box, S(2));
        FillColor(dc, box, kSearchFill);
        FrameColor(dc, box, kBorder);
    }
    if (!query.empty()) {
        CornerShadow(dc, layout.clearQuery, S(2));
        FillPanel(dc, layout.clearQuery, kPanel, 200);
        FrameColor(dc, layout.clearQuery, kBorder);
        Text(dc, L"X", layout.clearQuery, kText, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    for (std::size_t i = 0; i < layout.tabs.size(); ++i) {
        const bool active = i == pageIndex;
        const bool matched = !query.empty() && !active && i < tabMatches.size() && tabMatches[i] > 0;
        CornerShadow(dc, layout.tabs[i], S(2));
        FillPanel(dc, layout.tabs[i], active ? kSelected : (matched ? kHighlightFill : kPanel), 200);
        FrameColor(dc, layout.tabs[i], active || matched ? kGold : kBorder);
        // A label that does not fit its tab is drawn in the smaller font, and with an ellipsis if even that is too wide.
        const std::wstring tabLabel = UiText(Wide(database->pages[i].title));
        HFONT tabFont = fontBody;
        UINT tabFlags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
        {
            const int have = layout.tabs[i].right - layout.tabs[i].left - S(8);
            if (MeasureWidth(dc, tabLabel, tabFont) > have) tabFont = fontSmall;
            const int need = MeasureWidth(dc, tabLabel, tabFont);
            if (need > have) {
                tabFlags |= DT_END_ELLIPSIS;
                DebugLogOnce("tab:" + database->pages[i].id, "tab label does not fit even in the small font: '" + database->pages[i].title +
                                                              "' needs " + std::to_string(need) + " px, the tab has " + std::to_string(have) + " px");
            }
        }
        Text(dc, tabLabel, Rect(layout.tabs[i].left + S(4), layout.tabs[i].top, layout.tabs[i].right - S(4), layout.tabs[i].bottom),
             kHeaderGold, tabFont, tabFlags);
        if (active) {  // the launcher marks the open tab with a gold underline
            FillColor(dc, Rect(layout.tabs[i].left + S(6), layout.tabs[i].bottom - S(4), layout.tabs[i].right - S(6),
                               layout.tabs[i].bottom - S(2)), kGold);
        }
    }

    PaintList(dc);
    DrawPadFocus(dc);

    detailHits.clear();
    cardHits.clear();
    CornerShadow(dc, layout.detail, S(2));
    FillPanel(dc, layout.detail, kPanel, 185);
    FrameColor(dc, layout.detail, kBorder);
    PageView& view = CurrentView();
    if (!filtered.empty() && view.selected < filtered.size()) {
        const Entry& entry = page.entries[filtered[view.selected]];
        const int top = BuildControls(dc, entry, view);
        clipTop = top - S(2);
        clipBottom = layout.detail.bottom - 1;
        const std::vector<std::size_t> order = ArrangedOrder(entry, filtered[view.selected], view);
        rowSortNow = ResolveBlockSort(entry, view.sort);
        splitNow = entry.splitLayout;
        splitTitleNow = entry.title + " Ascendancy";
        // Sorted by name, the Skills page's cards leave their trees, so the tree titles go too.
        const bool headers = !entry.sortable && !(HasRuneBlocks(entry) && view.sort == BlockSort::Name) &&
                             !(SkillsActive() && view.sort == BlockSort::Name);
        const int visible = layout.detail.bottom - top - S(10);
        const int total = RenderDetail(dc, entry, order, headers, top, 0, page.compact, false);
        view.detailScroll = std::clamp(view.detailScroll, 0, std::max(0, total - visible));
        scrollTotal = total;
        scrollVisible = visible;
        bodyTop = top;
        ApplyJump(total, visible);
        CampaignApplyReveal(total, visible);
        SkillsApplyReveal(total, visible);

        // Draw the cards onto a copy of the frame, then copy back only the detail band. A DC clip
        // is not reliable here: once GDI+ has drawn art into the DC, later card backgrounds ignore
        // it and spill over the tabs and chip bar.
        const RECT band{layout.detail.left + 1, clipTop, layout.detail.right - S(18), clipBottom};
        const HDC detailDc = CreateCompatibleDC(dc);
        const HBITMAP detailBitmap = CreateCompatibleBitmap(dc, clientWidth, clientHeight);
        const HGDIOBJ previousDetail = SelectObject(detailDc, detailBitmap);
        BitBlt(detailDc, 0, 0, clientWidth, clientHeight, dc, 0, 0, SRCCOPY);
        RenderDetail(detailDc, entry, order, headers, top, view.detailScroll, page.compact, true);
        BitBlt(dc, band.left, band.top, band.right - band.left, band.bottom - band.top, detailDc, band.left, band.top,
               SRCCOPY);
        SelectObject(detailDc, previousDetail);
        DeleteObject(detailBitmap);
        DeleteDC(detailDc);
        DrawScrollbar(dc, top, visible, total, view.detailScroll);
    } else {
        layout.chips.clear();  // no controls are built here, so the last ones must not stay clickable
        Text(dc, UiText(L"Nothing selected"), layout.detail, kDim, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    DrawOsk(dc);
    DrawCardFocus(dc);
    DrawDetailFocus(dc);
    DrawJumpFlash(dc);
    DrawSuggest(dc);
    DrawTooltip(dc);
    DrawSpecTooltip(dc);
    DrawCampaignTooltip(dc);
    SkillsPadAfterPaint();
    BitBlt(target, 0, 0, clientWidth, clientHeight, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

namespace {

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wParam, lParam);
    }
    auto* self = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self == nullptr) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case kMsgToggle:
        self->Toggle();
        return 0;
    case kMsgOpen:
        self->Show();
        return 0;
    case kMsgOpenOsk:
        self->OpenOsk();
        return 0;
    case kMsgReload:
        self->ApplyPendingDatabase();
        return 0;
    case kMsgSpecData:
        self->ApplySpecData();
        return 0;
    case kMsgCampaignData:
        self->ApplyCampaignData();
        return 0;
    case kMsgMapsData:
        self->ApplyMapsData();
        return 0;
    case kMsgSkillsData:
        self->ApplySkillsData();
        return 0;
    case kMsgShutdown:
        if (IsWindow(self->blocker)) DestroyWindow(self->blocker);
        self->blocker = nullptr;
        DestroyWindow(window);
        return 0;
    case WM_CLOSE:
        self->Hide();
        return 0;
    case WM_DESTROY:
        KillTimer(window, kTimerFocus);
        PostQuitMessage(0);
        return 0;
    case WM_SETCURSOR:
        HideSystemCursor();
        return TRUE;
    case WM_ERASEBKGND:
        if (!self->firstPainted && self->backgroundBrush != nullptr) {
            RECT area{};
            GetClientRect(window, &area);
            FillRect(reinterpret_cast<HDC>(wParam), &area, self->backgroundBrush);
        }
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        self->Paint(dc);
        EndPaint(window, &paint);
        self->firstPainted = true;
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTimerFocus) self->SyncVisibility();
        else if (wParam == kTimerHandoff) self->HandoffTick();
        else if (wParam == kTimerPad) self->PadTick();
        else if (wParam == kTimerFlash) {
            KillTimer(window, kTimerFlash);
            self->flashBlock = -1;
            self->specFlashPerk = -1;
            self->campaignFlashArea = 0;
            self->skillsFlash = nullptr;
            InvalidateRect(window, nullptr, FALSE);
        } else if (wParam == kTimerSuggest) {
            KillTimer(window, kTimerSuggest);
            if (IsWindowVisible(window)) self->FlushSuggest();
            else self->suggestDirty = false;  // hidden: ReopenSuggest looks the text up when it is needed
        } else if (wParam == kTimerNote) {
            KillTimer(window, kTimerNote);
            self->searchNote.clear();
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE: {
        // A move the left stick made is not the mouse; the stick moves the pointer on the cursor thread, so a move soon after
        // it is not judged.
        if (self->padActive && GetTickCount64() - self->drive.movedAt.load() > 150) {
            POINT now{};
            if (GetCursorPos(&now) && (std::abs(now.x - self->drive.pointerX.load()) > 3 ||
                                       std::abs(now.y - self->drive.pointerY.load()) > 3)) {
                DebugLog("controller mode ended by a mouse move: pointer " + std::to_string(now.x) + "," + std::to_string(now.y) +
                         ", stick left it at " + std::to_string(self->drive.pointerX.load()) + "," +
                         std::to_string(self->drive.pointerY.load()));
                self->PadMouseUsed();
            }
        }
        self->HandleMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    }
    case WM_MOUSELEAVE:
        self->trackingMouse = false;
        if (!self->dragging && self->skillsBarDragging < 0) self->ClearHover();
        return 0;
    case WM_LBUTTONDOWN:
        self->PadMouseUsed();
        self->HandleClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_LBUTTONUP:
        self->EndDrag();
        return 0;
    case WM_CAPTURECHANGED:
        self->dragging = false;
        self->skillsBarDragging = -1;
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        self->HandleWheel(point.x, point.y, GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kIdSearch && HIWORD(wParam) == EN_CHANGE) {
            self->QueryChanged();
        }
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wParam), kText);
        SetBkColor(reinterpret_cast<HDC>(wParam), kSearchFill);
        return reinterpret_cast<LRESULT>(self->searchBrush);
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

// Owns the drawn-cursor window and its message loop, separate from the wiki's UI thread.
DWORD WINAPI CursorThread(LPVOID parameter) {
    auto* self = static_cast<OverlayState*>(parameter);
    tCursorDrive = &self->drive;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW cursorClass{};
    cursorClass.cbSize = sizeof(cursorClass);
    cursorClass.lpfnWndProc = CursorProc;
    cursorClass.hInstance = instance;
    cursorClass.lpszClassName = self->cursorClassName.c_str();
    RegisterClassExW(&cursorClass);
    self->cursorWindow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                             WS_EX_TOPMOST,
                                         self->cursorClassName.c_str(), L"", WS_POPUP, 0, 0, self->S(kCursorWidth), self->S(kCursorHeight),
                                         nullptr, nullptr, instance, nullptr);
    if (self->cursorWindow != nullptr) {
        SetWindowLongPtrW(self->cursorWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&self->lastCursor));
        SetTimer(self->cursorWindow, 1, 8, nullptr);  // about 120 updates a second while shown
    }

    // The 8 ms WM_TIMER above is rounded up to the system tick (about 15.6 ms), so on its own the drawn cursor
    // follows the pointer at only ~64 updates a second and trails it. While the cursor is shown, a 1 ms
    // high-resolution waitable timer moves the window to the pointer instead (the game's own cursor is
    // drawn once per frame, so this matches it). Without the timer (older Windows) the WM_TIMER path remains.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    HANDLE fastTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    bool fastOn = false;
    const auto setFast = [&](bool on) {
        if (fastTimer == nullptr || on == fastOn) return;
        if (on) {
            LARGE_INTEGER due;
            due.QuadPart = -10000;  // 1 ms, then every 1 ms
            SetWaitableTimer(fastTimer, &due, 1, nullptr, nullptr, FALSE);
        } else {
            CancelWaitableTimer(fastTimer);
        }
        fastOn = on;
    };

    // The left stick moves the real pointer from here, on the same 1 ms timer that moves the drawn cursor, so it glides
    // at the display's pace instead of stepping at the controller poll's. The speed eases toward what the stick asks for
    // (esrwiki::pad::SmoothToward) and the pointer moves in whole pixels with the remainder carried over.
    LARGE_INTEGER ticksPerSecond{};
    QueryPerformanceFrequency(&ticksPerSecond);
    LARGE_INTEGER lastTick{};
    double velocityX = 0, velocityY = 0, carryX = 0, carryY = 0;
    const auto stepStick = [&]() {
        LARGE_INTEGER nowTick{};
        QueryPerformanceCounter(&nowTick);
        const double dt = (lastTick.QuadPart == 0 || ticksPerSecond.QuadPart == 0)
                              ? 0.001
                              : std::min(0.05, static_cast<double>(nowTick.QuadPart - lastTick.QuadPart) /
                                                   static_cast<double>(ticksPerSecond.QuadPart));
        lastTick = nowTick;
        CursorDrive& d = self->drive;
        const bool driving = d.driving.load();
        const float sx = driving ? d.stickX.load() : 0.0f;
        const float sy = driving ? d.stickY.load() : 0.0f;
        const double push = std::min(1.0, std::sqrt(static_cast<double>(sx) * sx + static_cast<double>(sy) * sy));
        const double speed = d.speed.load();
        const float targetX = static_cast<float>(sx * push * speed);
        const float targetY = static_cast<float>(-sy * push * speed);
        velocityX = esrwiki::pad::SmoothToward(static_cast<float>(velocityX), targetX, static_cast<float>(dt));
        velocityY = esrwiki::pad::SmoothToward(static_cast<float>(velocityY), targetY, static_cast<float>(dt));
        if (targetX == 0.0f && targetY == 0.0f && std::fabs(velocityX) < 4.0 && std::fabs(velocityY) < 4.0) {
            velocityX = velocityY = carryX = carryY = 0.0;  // at rest
            return;
        }
        carryX += velocityX * dt;
        carryY += velocityY * dt;
        const int stepX = static_cast<int>(carryX);
        const int stepY = static_cast<int>(carryY);
        carryX -= stepX;
        carryY -= stepY;
        if (stepX == 0 && stepY == 0) return;
        POINT point{};
        RECT area{};
        if (self->window == nullptr || !GetCursorPos(&point) || !GetWindowRect(self->window, &area)) return;
        point.x = std::clamp<LONG>(point.x + static_cast<LONG>(stepX), area.left, area.right - 1);
        point.y = std::clamp<LONG>(point.y + static_cast<LONG>(stepY), area.top, area.bottom - 1);
        SetCursorPos(point.x, point.y);
        d.pointerX.store(point.x);
        d.pointerY.store(point.y);
        d.movedAt.store(GetTickCount64());
    };
    // The pointer is moved about 125 times a second at most (the sub-pixel remainder carries over), which is still smoother
    // than the display; moving it every millisecond made the wiki repaint for every move.
    LARGE_INTEGER lastMove{};
    const auto stepStickPaced = [&]() {
        LARGE_INTEGER nowTick{};
        QueryPerformanceCounter(&nowTick);
        if (lastMove.QuadPart != 0 && ticksPerSecond.QuadPart != 0 &&
            (nowTick.QuadPart - lastMove.QuadPart) * 1000 < 8 * ticksPerSecond.QuadPart)
            return;
        lastMove = nowTick;
        stepStick();
    };

    for (bool quit = false; !quit;) {
        const DWORD handles = fastTimer != nullptr ? 1 : 0;
        const DWORD woke = MsgWaitForMultipleObjectsEx(handles, handles != 0 ? &fastTimer : nullptr, INFINITE, QS_ALLINPUT,
                                                       MWMO_INPUTAVAILABLE);
        if (woke == WAIT_FAILED) break;
        if (handles != 0 && woke == WAIT_OBJECT_0) {
            stepStickPaced();
            if (self->cursorWindow != nullptr && IsWindowVisible(self->cursorWindow)) MoveCursorToPointer(self->cursorWindow);
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (handles == 0) stepStick();  // no high-resolution timer: the stick moves at the thread's slower wake-ups
        setFast((self->cursorWindow != nullptr && IsWindowVisible(self->cursorWindow)) || self->drive.driving.load());
    }
    if (fastTimer != nullptr) CloseHandle(fastTimer);
    if (IsWindow(self->cursorWindow)) DestroyWindow(self->cursorWindow);
    self->cursorWindow = nullptr;
    UnregisterClassW(self->cursorClassName.c_str(), instance);
    return 0;
}

DWORD WINAPI UiThread(LPVOID parameter) {
    auto* self = static_cast<OverlayState*>(parameter);
    Gdiplus::GdiplusStartupInput startup;
    if (Gdiplus::GdiplusStartup(&self->gdiplusToken, &startup, nullptr) != Gdiplus::Ok) {
        SetEvent(self->ready);
        return 1;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    windowClass.hbrBackground = CreateSolidBrush(kBackground);
    windowClass.lpszClassName = self->windowClassName.c_str();
    RegisterClassExW(&windowClass);

    WNDCLASSEXW blockerClass{};
    blockerClass.cbSize = sizeof(blockerClass);
    blockerClass.lpfnWndProc = BlockerProc;
    blockerClass.hInstance = windowClass.hInstance;
    blockerClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    blockerClass.lpszClassName = self->blockerClassName.c_str();
    RegisterClassExW(&blockerClass);

    WNDCLASSEXW helpClass{};
    helpClass.cbSize = sizeof(helpClass);
    helpClass.lpfnWndProc = HelpProc;
    helpClass.hInstance = windowClass.hInstance;
    helpClass.lpszClassName = self->helpClassName.c_str();
    RegisterClassExW(&helpClass);

    self->CreateFonts();
    self->searchBrush = CreateSolidBrush(kSearchFill);
    self->backgroundBrush = CreateSolidBrush(kBackground);
    self->AdoptSkillsData();    // given before Start: the Skills page is there from the first paint
    self->PrepareSkillsPage();
    self->views.assign(self->database->pages.size(), PageView{});

    self->window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, self->windowClassName.c_str(), self->title.c_str(),
                                   WS_POPUP | WS_CLIPCHILDREN, 0, 0, self->S(960), self->S(620),
                                   nullptr, nullptr, windowClass.hInstance, self);
    if (self->window == nullptr) {
        UnregisterClassW(self->blockerClassName.c_str(), blockerClass.hInstance);
        UnregisterClassW(self->windowClassName.c_str(), windowClass.hInstance);
        Gdiplus::GdiplusShutdown(self->gdiplusToken);
        SetEvent(self->ready);
        return 1;
    }

    SetLayeredWindowAttributes(self->window, 0, 255, LWA_ALPHA);

    // Covers every monitor. Hidden until the overlay opens; a missing blocker only
    // loses the input capture, the overlay still works.
    self->blocker = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, self->blockerClassName.c_str(), L"", WS_POPUP, 0, 0, 0, 0,
                                    nullptr, nullptr, windowClass.hInstance, nullptr);
    if (self->blocker != nullptr) {
        SetLayeredWindowAttributes(self->blocker, 0, kBlockerAlpha, LWA_ALPHA);
        SetWindowPos(self->blocker, nullptr, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // The controller help picture: click-through, never active, shown only while a controller is in use.
    self->helpWindow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                       self->helpClassName.c_str(), L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                       windowClass.hInstance, nullptr);

    SetTimer(self->window, kTimerFocus, kTimerInterval, nullptr);
    self->FitToGame();
    const RECT editRect = SearchEditRect(self->layout, self->bodyLine, self->S(6));
    self->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   editRect.left, editRect.top, editRect.right - editRect.left,
                                   editRect.bottom - editRect.top, self->window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSearch)),
                                   windowClass.hInstance, nullptr);
    SendMessageW(self->search, WM_SETFONT, reinterpret_cast<WPARAM>(self->fontBody), TRUE);
    SendMessageW(self->search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
    gSearchProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(self->search, GWLP_WNDPROC,
                                                               reinterpret_cast<LONG_PTR>(SearchEditProc)));
    self->Refilter();
    self->started = true;
    SetEvent(self->ready);
    self->EnsureSearchIndex();  // the suggestions' index, built when the data loads rather than on the first key typed

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // The wiki key toggles: pressing it again closes the overlay. Alt combinations arrive as
        // WM_SYSKEYDOWN. continue skips TranslateMessage, so the key does not also type into the search box.
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && IsWindowVisible(self->window) &&
            self->isCloseKey && self->isCloseKey(static_cast<unsigned int>(message.wParam))) {
            self->Hide();
            continue;
        }
        if (message.message == WM_KEYDOWN && IsWindowVisible(self->window)) {
            if ((message.wParam == VK_ESCAPE || message.wParam == VK_UP || message.wParam == VK_DOWN || message.wParam == VK_RETURN ||
                 message.wParam == VK_NEXT || message.wParam == VK_PRIOR || message.wParam == VK_TAB) &&
                self->SuggestKey(message.wParam)) {
                continue;  // the suggestions list took it (continue: no WM_CHAR, so Tab and Enter type nothing)
            }
            if (message.wParam == VK_F3) {  // F3 / Shift+F3: the next / previous match on the page, list open or not
                self->CloseSuggest();
                self->StepMatch((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
                continue;
            }
            if (message.wParam == VK_ESCAPE) {
                if (!self->CampaignEscape()) self->Hide();  // the map's area details close first
                continue;
            }
            if (message.wParam == VK_UP || message.wParam == VK_DOWN || message.wParam == VK_NEXT ||
                message.wParam == VK_PRIOR) {
                self->HandleKey(message.wParam);
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    self->ReleaseBackgroundCache();
    self->ReleaseSkillsClip();
    self->images.clear();
    if (self->searchBrush) DeleteObject(self->searchBrush);
    if (self->backgroundBrush) DeleteObject(self->backgroundBrush);
    if (self->fontBody) DeleteObject(self->fontBody);
    if (self->fontSmall) DeleteObject(self->fontSmall);
    if (self->fontBold) DeleteObject(self->fontBold);
    if (self->fontTitle) DeleteObject(self->fontTitle);
    if (self->fontSnippet) DeleteObject(self->fontSnippet);
    if (self->fontSnippetBold) DeleteObject(self->fontSnippetBold);
    self->gamepad.Stop();
    if (self->helpWindow != nullptr) DestroyWindow(self->helpWindow);
    UnregisterClassW(self->helpClassName.c_str(), helpClass.hInstance);
    UnregisterClassW(self->blockerClassName.c_str(), blockerClass.hInstance);
    UnregisterClassW(self->windowClassName.c_str(), windowClass.hInstance);
    Gdiplus::GdiplusShutdown(self->gdiplusToken);
    return 0;
}

}  // namespace

Overlay::Overlay() : impl_(std::make_unique<OverlayState>()) {}

Overlay::~Overlay() { Stop(); }

bool Overlay::Start(const Database& database, const std::wstring& dataDir, const std::wstring& title,
                    const std::wstring& instanceId, const std::vector<SwitchButton>& switches) {
    if (impl_->started || database.pages.empty()) return false;
    impl_->database = &database;
    impl_->dataDir = dataDir;
    impl_->title = title;
    impl_->windowClassName = L"EsrWikiOverlayWindow-" + instanceId;
    impl_->blockerClassName = L"EsrWikiInputBlocker-" + instanceId;
    impl_->cursorClassName = L"EsrWikiCursor-" + instanceId;
    impl_->helpClassName = L"EsrWikiPadHelp-" + instanceId;
    impl_->switches = switches;
    impl_->cursorThread = CreateThread(nullptr, 0, CursorThread, impl_.get(), 0, &impl_->cursorThreadId);
    impl_->ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->ready == nullptr) return false;
    impl_->thread = CreateThread(nullptr, 0, UiThread, impl_.get(), 0, nullptr);
    if (impl_->thread == nullptr) {
        CloseHandle(impl_->ready);
        impl_->ready = nullptr;
        return false;
    }
    WaitForSingleObject(impl_->ready, 10000);
    if (!impl_->started) {
        Stop();
        return false;
    }
    return true;
}

bool Overlay::ReplaceDatabase(std::unique_ptr<Database> next) {
    if (!impl_->started || impl_->window == nullptr || !next || next->pages.empty()) return false;
    {
        std::lock_guard<std::mutex> guard(impl_->pendingLock);
        impl_->pendingDb = std::move(next);
    }
    PostMessageW(impl_->window, kMsgReload, 0, 0);
    return true;
}

void Overlay::Stop() {
    if (impl_->cursorThread != nullptr) {
        PostThreadMessageW(impl_->cursorThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(impl_->cursorThread, 2000);
        CloseHandle(impl_->cursorThread);
        impl_->cursorThread = nullptr;
        impl_->cursorThreadId = 0;
    }
    if (impl_->thread == nullptr) return;
    if (impl_->window != nullptr) PostMessageW(impl_->window, kMsgShutdown, 0, 0);
    WaitForSingleObject(impl_->thread, 5000);
    CloseHandle(impl_->thread);
    impl_->thread = nullptr;
    if (impl_->ready != nullptr) CloseHandle(impl_->ready);
    impl_->ready = nullptr;
    impl_->window = nullptr;
    impl_->blocker = nullptr;
    impl_->started = false;
}

void Overlay::Toggle() {
    if (impl_->window != nullptr) PostMessageW(impl_->window, kMsgToggle, 0, 0);
}

void Overlay::SetCloseKey(std::function<bool(unsigned int)> isCloseKey) {
    impl_->isCloseKey = std::move(isCloseKey);
}

void Overlay::Open() {
    if (impl_->window != nullptr) PostMessageW(impl_->window, kMsgOpen, 0, 0);
}

bool Overlay::IsShown() const {
    return impl_ != nullptr && impl_->window != nullptr && IsWindowVisible(impl_->window) != FALSE;
}

std::string UiTextUtf8(const std::string& english) {
    const std::wstring text = UiText(Wide(english));
    return text == Wide(english) ? std::string() : Narrow(text);  // empty: no translation
}

}  // namespace esrwiki
