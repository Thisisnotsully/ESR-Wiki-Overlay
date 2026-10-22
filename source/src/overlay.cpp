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
// GDI+ needs the COM types (IStream, PROPID) that objidl.h declares. Include it first.
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <set>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "overlay.hpp"
#include "wiki_model.hpp"

namespace esrwiki {
namespace {

constexpr int kIdSearch = 1001;
constexpr UINT kMsgToggle = WM_APP + 1;
constexpr UINT kMsgShutdown = WM_APP + 2;
constexpr UINT kMsgOpen = WM_APP + 3;
constexpr UINT_PTR kTimerFocus = 1;
constexpr UINT kMsgCursorShow = WM_APP + 10;  // to the cursor thread
constexpr UINT kMsgCursorHide = WM_APP + 11;
constexpr UINT kTimerInterval = 150;
constexpr BYTE kBlockerAlpha = 110;  // how strongly the game is dimmed while the overlay is open

constexpr COLORREF kBackground = RGB(14, 14, 14);
constexpr COLORREF kPanel = RGB(26, 26, 26);
constexpr COLORREF kTile = RGB(22, 22, 22);
constexpr COLORREF kSearchFill = RGB(42, 42, 42);
constexpr COLORREF kSelected = RGB(62, 62, 62);
constexpr COLORREF kBorder = RGB(84, 84, 84);
constexpr COLORREF kGold = RGB(212, 212, 212);  // headers and accents: light grey
constexpr COLORREF kText = RGB(238, 238, 238);
constexpr COLORREF kDim = RGB(150, 150, 150);
constexpr COLORREF kBody = RGB(216, 216, 216);

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
    RECT close{};
    RECT search{};
    RECT clearQuery{};  // the X beside the search box; clears the query
    RECT count{};
    std::vector<RECT> rows;
    RECT prev{};
    RECT next{};
    RECT pageLabel{};
    RECT detail{};
    RECT switchButton{};
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
    for (const RuneNameGroup& rune : kRuneNameGroups) {
        if (label == rune.name) return RuneGroupColor(rune.group);
    }
    return fallback;
}

// One piece of a line: plain text in the line's colour, or a rune name in its own colour.
struct RunePiece { std::wstring text; COLORREF color; bool named; bool colored = false; };

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
int LayoutPieces(HDC dc, const std::vector<RunePiece>& pieces, COLORREF base, RECT rect, HFONT font,
                 UINT format, bool draw) {
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    const int lineHeight = std::max(1, static_cast<int>(metrics.tmHeight));
    const int maxWidth = std::max(1, static_cast<int>(rect.right - rect.left));

    struct Placed { int x; int line; int width; std::wstring text; COLORREF color; };
    std::vector<Placed> placed;
    std::vector<int> lineWidths;
    int x = 0;
    int line = 0;
    for (const RunePiece& piece : pieces) {
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
                lineWidths.push_back(x);
                x = 0;
                ++line;
                continue;
            }
            const bool isSpace = !piece.named && token[0] == L' ';
            if (isSpace && x == 0) continue;  // no leading spaces on a wrapped line
            SIZE size = {};
            GetTextExtentPoint32W(dc, token.c_str(), static_cast<int>(token.size()), &size);
            if (!isSpace && x > 0 && x + size.cx > maxWidth) {
                lineWidths.push_back(x);
                x = 0;
                ++line;
            }
            placed.push_back({x, line, static_cast<int>(size.cx), token, color});
            x += static_cast<int>(size.cx);
        }
    }
    lineWidths.push_back(x);
    const int height = placed.empty() ? 0 : (line + 1) * lineHeight;

    if (draw && !placed.empty()) {
        int top = rect.top;
        if (format & DT_VCENTER) top += (static_cast<int>(rect.bottom - rect.top) - height) / 2;
        SetBkMode(dc, TRANSPARENT);
        for (const Placed& item : placed) {
            int offset = 0;
            const int used = lineWidths[static_cast<std::size_t>(item.line)];
            if (format & DT_CENTER) offset = (maxWidth - used) / 2;
            else if (format & DT_RIGHT) offset = maxWidth - used;
            SetTextColor(dc, item.color);
            TextOutW(dc, rect.left + offset + item.x, top + item.line * lineHeight, item.text.c_str(),
                     static_cast<int>(item.text.size()));
        }
    }
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
    if (FindRuneNames(text, pieces)) {
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
    if (FindRuneNames(text, pieces)) return LayoutPieces(dc, pieces, kBody, Rect(0, 0, width, 0), font, DT_LEFT, false);
    RECT measured = Rect(0, 0, width, 0);
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, previous);
    return measured.bottom - measured.top;
}

// Height that `text` needs when wrapped to `width` in `font`, remembered for repeat measures.
int MeasureHeight(HDC dc, const std::wstring& text, int width, HFONT font) {
    if (text.empty()) return 0;
    const MeasureKey key{std::hash<std::wstring>{}(text), width, font};
    const auto cached = gMeasureCache.find(key);
    if (cached != gMeasureCache.end()) return cached->second;
    const int height = MeasureHeightUncached(dc, text, width, font);
    if (gMeasureCache.size() > 50000) gMeasureCache.clear();
    gMeasureCache.emplace(key, height);
    return height;
}

// Width of `text` on one line in `font`.
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

// What the pointer is over; only item blocks and their runes ever get a hover.
struct Hover { const Entry* entry = nullptr; const Block* block = nullptr; int rune = -1; POINT at{0, 0}; };

// Clicks on the blocker must not activate it (that would move keyboard focus off
// the panel), and closing it is ignored so Alt+F4 cannot remove the input block.
// The game keeps hiding and showing the system cursor, which flickers. Over the wiki the system
// cursor is hidden instead, and a drawn arrow (CursorProc) follows the pointer.
void HideSystemCursor() {
    SetCursor(nullptr);
}

// Moves the cursor window to the pointer, but only when the pointer has moved.
void MoveCursorToPointer(HWND window) {
    POINT point{};
    if (!GetCursorPos(&point)) return;
    POINT* last = reinterpret_cast<POINT*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (last != nullptr) {
        if (last->x == point.x && last->y == point.y) return;
        *last = point;
    }
    SetWindowPos(window, HWND_TOPMOST, point.x, point.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

// Click-through window that draws the arrow at the pointer. It lives on its own thread (CursorThread),
// so repaints of the wiki cannot delay it.
LRESULT CALLBACK CursorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case kMsgCursorShow:
        if (IsWindow(window)) {
            POINT* last = reinterpret_cast<POINT*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (last != nullptr) *last = POINT{-1, -1};
        }
        ShowWindow(window, SW_SHOWNOACTIVATE);
        MoveCursorToPointer(window);
        return 0;
    case kMsgCursorHide:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_TIMER:
        if (IsWindowVisible(window)) MoveCursorToPointer(window);
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        HideSystemCursor();
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT box{};
        GetClientRect(window, &box);
        const HBRUSH key = CreateSolidBrush(RGB(0, 0, 0));  // black is the transparent colour key
        FillRect(dc, &box, key);
        DeleteObject(key);
        const int dpi = static_cast<int>(GetDpiForWindow(window));
        // Arrow drawn in a 20 x 30 box at 96 dpi; the tip is at (0, 0).
        const POINT units[7] = {{0, 0}, {0, 22}, {6, 17}, {10, 26}, {13, 25}, {9, 16}, {16, 16}};
        POINT points[7]{};
        for (int i = 0; i < 7; ++i) {
            points[i].x = MulDiv(units[i].x, dpi, 96);
            points[i].y = MulDiv(units[i].y, dpi, 96);
        }
        const HPEN pen = CreatePen(PS_SOLID, 1, RGB(10, 10, 10));
        const HBRUSH fill = CreateSolidBrush(RGB(255, 255, 255));
        const HGDIOBJ oldPen = SelectObject(dc, pen);
        const HGDIOBJ oldBrush = SelectObject(dc, fill);
        Polygon(dc, points, 7);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(fill);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
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

struct OverlayState {
    const Database* database = nullptr;
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
    HBRUSH searchBrush = nullptr;
    int dpi = 96;
    double uiScale = 1.0;  // font and text scale from the window size, on top of DPI
    int bodyLine = 16;     // line heights in pixels for the current fonts
    int smallLine = 14;
    int boldLine = 18;
    int width = 0;
    int height = 0;
    int rowsPerPage = 1;
    std::size_t pageIndex = 0;
    std::vector<PageView> views;
    std::string query;
    std::vector<std::size_t> filtered;
    std::map<std::string, std::unique_ptr<Gdiplus::Bitmap>> images;
    Layout layout;
    bool started = false;
    bool open = false;  // user wants it open; it only shows while the game is active
    std::wstring title = L"ESR Wiki";
    std::wstring windowClassName;
    std::wstring blockerClassName;
    std::wstring cursorClassName;
    std::wstring switchLabel;          // empty when the overlay has no switch button
    std::function<void()> onSwitch;
    bool greenNames = false;  // set-item pages draw names and headers in green
    COLORREF itemNameColor = kGold;     // item names on this page: gold uniques/runewords/gemwords, green sets, orange class crafting
    COLORREF tooltipNameColor = kGold;  // tooltip heading colour on this page (sets keep the plain gold)
    int titleLine = 18;       // line height of fontTitle
    bool trackingMouse = false;
    HWND gameWindow = nullptr;
    RECT fittedGameRect{};
    std::vector<DetailHit> detailHits;
    Hover hover;
    int clipTop = 0;        // visible band of the detail content, in client coordinates
    int clipBottom = 0;
    int scrollTotal = 0;    // content height and visible height from the last paint
    int scrollVisible = 0;
    bool dragging = false;  // dragging the scrollbar thumb
    int dragStartY = 0;
    int dragStartScroll = 0;

    const Page& CurrentPage() const { return database->pages[pageIndex]; }
    PageView& CurrentView() { return views[pageIndex]; }
    int S(int value) const { return MulDiv(value, dpi, 96); }
    int RuneRowHeight(const TileStyle& style) const { return style.cell + smallLine + S(4); }
    int RuneRows(std::size_t count, const TileStyle& style) const;
    TileStyle MakeTileStyle(int width_, bool compact, bool fiveAcross = false) const;

    HFONT MakeFont(int points, int weight) const;
    void CreateFonts();
    void ApplyFonts();
    Layout ComputeLayout();
    void FitToGame();
    void Refilter();
    void SetPage(std::size_t index);
    void SelectFiltered(std::size_t index);
    void MovePage(int direction);
    void Show();
    void Hide();
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
    int RenderTable(HDC dc, const std::vector<Line>& lines, std::size_t first, std::size_t last, int left,
                    int width_, int y, bool draw);
    // Row heights per table, keyed by width and text, so scrolling does not re-measure every cell.
    std::unordered_map<std::string, std::vector<int>> tableHeightCache;
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
    void DrawBackground(Gdiplus::Graphics& graphics, RECT client);
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
    style.columns = fiveAcross ? 5 : compact ? (width_ >= S(560) ? 6 : 4) : 3;
    style.gap = compact || fiveAcross ? S(6) : S(12);
    style.tileWidth = std::max(S(40), (width_ - style.gap * (style.columns - 1)) / style.columns);
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

HFONT OverlayState::MakeFont(int points, int weight) const {
    const int pixelHeight = -static_cast<int>(std::lround(points * uiScale * dpi / 72.0));
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
    for (HFONT* font : {&fontBody, &fontSmall, &fontBold, &fontTitle}) {
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

    const HDC screen = GetDC(nullptr);
    bodyLine = LineHeight(screen, fontBody);
    smallLine = LineHeight(screen, fontSmall);
    boldLine = LineHeight(screen, fontBold);
    titleLine = LineHeight(screen, fontTitle);
    ReleaseDC(nullptr, screen);
    if (IsWindow(search)) SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(fontBody), TRUE);
}

// Positions everything for the current width and height. Row heights come from
// the bold font, so two-line titles fit at any scale, and the list length follows
// the window height.
Layout OverlayState::ComputeLayout() {
    Layout computed;
    const auto& pages = database->pages;
    const int margin = S(16);

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

    const int tabRowHeight = std::max(S(30), bodyLine + S(10));
    const int tabRowGap = S(4);
    const int tabTop = S(44);
    computed.tabs.resize(pages.size());
    std::vector<int> seenInRow(rowNames.size(), 0);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const auto row = static_cast<std::size_t>(rowOfPage[i]);
        const int count = pagesInRow[row];
        const int column = seenInRow[row]++;
        const int tabWidth = (width - 2 * margin - S(5) - (count - 1) * S(6)) / count;  // right edge 5px in
        const int top = tabTop + static_cast<int>(row) * (tabRowHeight + tabRowGap);
        const int left = margin + column * (tabWidth + S(6));
        computed.tabs[i] = Rect(left, top, left + tabWidth, top + tabRowHeight);
    }

    const int contentTop = tabTop + static_cast<int>(rowNames.size()) * (tabRowHeight + tabRowGap) + S(8);
    // The category list is 15% narrower than before; the detail panel takes the space and starts 5px further left.
    const int listWidth = Bounded(static_cast<int>(S(340) * uiScale), S(260), std::max(S(260), width * 38 / 100)) * 85 / 100 * 70 / 100;
    const int rowHeight = std::max(S(48), boldLine * 2 + S(10));
    const int rowGap = S(2);
    const int pagingHeight = std::max(S(30), bodyLine + S(10));
    const int pagingGap = S(6);
    const int listTop = contentTop + S(56);
    const int bottom = height - S(12);
    rowsPerPage = std::max(3, (bottom - pagingGap - pagingHeight - listTop) / rowHeight);

    // Close lines up with the right edge of the tabs; the switch button sits beside it.
    const int tabsRight = width - margin - S(5);
    computed.close = Rect(tabsRight - S(32), S(14), tabsRight, S(42));
    computed.switchButton = Rect(tabsRight - S(182), S(14), tabsRight - S(42), S(42));
    computed.search = Rect(margin, contentTop, margin + listWidth, contentTop + S(26));
    computed.clearQuery = Rect(margin + listWidth - S(26), contentTop, margin + listWidth, contentTop + S(26));
    computed.count = Rect(margin, contentTop + S(30), margin + listWidth, contentTop + S(50));
    for (int i = 0; i < rowsPerPage; ++i) {
        const int top = listTop + i * rowHeight;
        computed.rows.push_back(Rect(margin, top, margin + listWidth, top + rowHeight - rowGap));
    }
    const int pagingTop = listTop + rowsPerPage * rowHeight + pagingGap;
    computed.prev = Rect(margin, pagingTop, margin + S(80), pagingTop + pagingHeight);
    computed.pageLabel = Rect(margin + S(84), pagingTop, margin + listWidth - S(84), pagingTop + pagingHeight);
    computed.next = Rect(margin + listWidth - S(80), pagingTop, margin + listWidth, pagingTop + pagingHeight);
    computed.detail = Rect(margin + listWidth + S(24) - S(5), contentTop, width - margin - S(5), height - margin);
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
    if (IsWindow(gameWindow)) {
        const RECT game = GameScreenRect(gameWindow);
        const int cw = game.right - game.left;
        const int ch = game.bottom - game.top;
        w = std::min(Bounded(cw * 4 / 5, S(960), S(1500)), cw);
        h = std::min(Bounded(ch * 9 / 10, S(620), S(1000)), ch);
        left = game.left + (cw - w) / 2;
        top = game.top + (ch - h) / 2;
        fittedGameRect = game;
    } else {
        left = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        top = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        fittedGameRect = RECT{};
    }

    const double scale = std::clamp(static_cast<double>(w) / S(960), 0.85, 1.5);
    if (std::fabs(scale - uiScale) > 0.02) {
        uiScale = scale;
        ApplyFonts();
    }

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
        MoveWindow(search, layout.search.left, layout.search.top, layout.search.right - layout.search.left - S(26),
                   layout.search.bottom - layout.search.top, TRUE);
    }
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

// Background art: cover-fit to the window and centred, with a dark tint so text stays readable.
void OverlayState::DrawBackground(Gdiplus::Graphics& graphics, RECT client) {
    Gdiplus::Bitmap* bitmap = FindImage("background");
    const int areaWidth = client.right - client.left;
    const int areaHeight = client.bottom - client.top;
    if (bitmap == nullptr || areaWidth <= 0 || areaHeight <= 0) return;
    const double imageWidth = bitmap->GetWidth();
    const double imageHeight = bitmap->GetHeight();
    if (imageWidth <= 0 || imageHeight <= 0) return;
    const double scale = std::max(areaWidth / imageWidth, areaHeight / imageHeight);
    const int drawWidth = static_cast<int>(imageWidth * scale);
    const int drawHeight = static_cast<int>(imageHeight * scale);
    graphics.DrawImage(bitmap, (areaWidth - drawWidth) / 2, (areaHeight - drawHeight) / 2, drawWidth, drawHeight);
    Gdiplus::SolidBrush tint(Gdiplus::Color(150, 14, 14, 14));
    graphics.FillRectangle(&tint, 0, 0, areaWidth, areaHeight);
}

// Builds the background once for the current window size, so repaints only copy it.
void OverlayState::EnsureBackgroundCache(HDC target, int areaWidth, int areaHeight) {
    if (backgroundBitmap != nullptr && areaWidth == backgroundWidth && areaHeight == backgroundHeight) return;
    ReleaseBackgroundCache();
    backgroundDc = CreateCompatibleDC(target);
    backgroundBitmap = CreateCompatibleBitmap(target, areaWidth, areaHeight);
    backgroundPrevious = SelectObject(backgroundDc, backgroundBitmap);
    const RECT rect{0, 0, areaWidth, areaHeight};
    FillColor(backgroundDc, rect, kBackground);
    {
        Gdiplus::Graphics graphics(backgroundDc);
        DrawBackground(graphics, rect);
    }
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

void OverlayState::DrawImageKey(Gdiplus::Graphics& graphics, const std::string& key, RECT box) {
    Gdiplus::Bitmap* bitmap = FindImage(key);
    if (bitmap == nullptr) return;
    const double imageWidth = bitmap->GetWidth();
    const double imageHeight = bitmap->GetHeight();
    if (imageWidth <= 0 || imageHeight <= 0) return;
    const double boxWidth = box.right - box.left;
    const double boxHeight = box.bottom - box.top;
    const double scale = std::min(boxWidth / imageWidth, boxHeight / imageHeight);
    const int drawWidth = static_cast<int>(imageWidth * scale);
    const int drawHeight = static_cast<int>(imageHeight * scale);
    const int left = box.left + (static_cast<int>(boxWidth) - drawWidth) / 2;
    const int top = box.top + (static_cast<int>(boxHeight) - drawHeight) / 2;
    graphics.DrawImage(bitmap, left, top, drawWidth, drawHeight);
}

void OverlayState::Refilter() {
    wchar_t buffer[256] = {};
    GetWindowTextW(search, buffer, 255);
    query = Narrow(buffer);
    filtered = FilterEntries(CurrentPage(), query);
    PageView& view = CurrentView();
    view.selected = 0;
    view.listPage = 0;
    view.detailScroll = 0;
}

void OverlayState::SetPage(std::size_t index) {
    if (index >= database->pages.size() || index == pageIndex) return;
    pageIndex = index;
    Refilter();
}

void OverlayState::SelectFiltered(std::size_t index) {
    if (index >= filtered.size()) return;
    PageView& view = CurrentView();
    view.selected = index;
    view.listPage = index / rowsPerPage;
    view.detailScroll = 0;
}

void OverlayState::MovePage(int direction) {
    if (filtered.empty()) return;
    PageView& view = CurrentView();
    const auto pages = PageCount(filtered.size(), rowsPerPage);
    const auto current = view.listPage;
    if (direction < 0 && current == 0) return;
    if (direction > 0 && current + 1 >= pages) return;
    view.listPage = direction < 0 ? current - 1 : current + 1;
    view.selected = view.listPage * rowsPerPage;
    view.detailScroll = 0;
}

void OverlayState::Show() {
    open = true;
    previousForeground = GetForegroundWindow();
    FitToGame();
    SyncVisibility();
    if (IsWindowVisible(window)) {
        SetForegroundWindow(window);
        SetFocus(search);
    }
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::Hide() {
    open = false;
    EndDrag();
    ClearHover();
    ShowBlocker(false);
    ShowWindow(window, SW_HIDE);
    if (previousForeground != nullptr && previousForeground != window && IsWindow(previousForeground)) {
        SetForegroundWindow(previousForeground);
    }
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
    SetWindowPos(blocker, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
    const bool shouldShow = open && gameActive;
    const bool shown = IsWindowVisible(window) != FALSE;
    if (!shouldShow) {
        if (shown) {
            EndDrag();
            ClearHover();
            ShowBlocker(false);
            ShowWindow(window, SW_HIDE);
        }
        return;
    }
    if (!shown) {
        FitToGame();
        ShowBlocker(true);
        SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
}

void OverlayState::EndDrag() {
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
    if (!Inside(layout.detail, x, y)) {
        ClearHover();
        return;
    }
    for (const DetailHit& hit : detailHits) {
        if (!Inside(hit.rect, x, y)) continue;
        // Same target: keep the tooltip where it is and do not repaint on every pixel of movement.
        if (hover.entry == hit.entry && hover.block == hit.block && hover.rune == hit.rune) return;
        hover.entry = hit.entry;
        hover.block = hit.block;
        hover.rune = hit.rune;
        hover.at = POINT{x, y};
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    ClearHover();
}

void OverlayState::ClearHover() {
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
        rows.push_back({Wide(block.runes[hover.rune].label) + L" Rune", kGold, fontBold});
        rows.push_back({L"Socketed in " + Wide(block.title.empty() ? hover.entry->title : block.title), kBody,
                        fontBody});
    } else {
        const std::string heading = block.title.empty() ? hover.entry->title : block.title;
        // Mythicals (the wide cards) are purple, set items green; the rest follow the page's name colour.
        COLORREF headingColor = tooltipNameColor;
        if (hover.entry->wide) headingColor = CodeColor(';');
        else if (CurrentPage().id == "set-items") headingColor = CodeColor('2');
        rows.push_back({Wide(heading), headingColor, fontBold});
        if (!block.subtitle.empty()) rows.push_back({Wide(block.subtitle), kDim, fontSmall});
        for (const Field& field : block.fields) {
            rows.push_back({Wide(field.label + ": " + field.value), kBody, fontBody});
        }
        for (const Line& line : block.lines) {
            std::string plain;
            COLORREF color = kBody;
            PlainText(line.text, plain, color, kBody);
            const bool isHeading = line.kind == LineKind::Heading;
            rows.push_back({Wide(plain), isHeading ? kGold : color, isHeading ? fontBold : fontBody});
        }
    }

    const int gap = S(4);
    RECT client{};
    GetClientRect(window, &client);
    // Start at the normal width and widen only when the text would run past the bottom of the
    // window (long weapon gemwords), so most tips keep their usual shape.
    const int availableHeight = static_cast<int>(client.bottom) - S(16);
    const int maxTipWidth = std::max(S(340), static_cast<int>(client.right) - S(16));
    int tipWidth = S(340);
    std::vector<int> heights;
    int totalHeight = S(16);
    // Mythical cards (wide entries) start at the wider size.
    const std::vector<int> widths = hover.entry->wide ? std::vector<int>{S(520), S(640)}
                                                      : std::vector<int>{S(340), S(420), S(520), S(640)};
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

    int left = hover.at.x + S(18);
    int top = hover.at.y + S(18);
    if (left + tipWidth > client.right - S(8)) left = hover.at.x - tipWidth - S(8);
    left = std::max(S(8), left);
    if (top + totalHeight > client.bottom - S(8)) top = client.bottom - totalHeight - S(8);
    top = std::max(S(8), top);

    // Long items are clipped to the window rather than pushed off screen.
    const int visibleHeight = std::min(totalHeight, static_cast<int>(client.bottom) - top - S(8));
    const RECT box = Rect(left, top, left + tipWidth, top + visibleHeight);
    // Tooltips sit on a see-through black so the wiki behind them still shows.
    FillPanel(dc, box, RGB(0, 0, 0), 204);
    FrameColor(dc, box, kGold);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, box.left, box.top, box.right, box.bottom);
    int y = top + S(8);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const RECT target = Rect(left + S(12), y, left + tipWidth - S(12), y + heights[i]);
        Text(dc, rows[i].text, target, rows[i].color, rows[i].font, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        y += heights[i] + gap;
    }
    RestoreDC(dc, saved);
}

void OverlayState::QueryChanged() {
    Refilter();
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::HandleClick(int x, int y) {
    PageView& view = CurrentView();
    if (!query.empty() && Inside(layout.clearQuery, x, y)) {
        SetWindowTextW(search, L"");
        QueryChanged();
        SetFocus(search);
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
    for (const Chip& chip : layout.chips) {
        if (!Inside(chip.rect, x, y)) continue;
        switch (chip.kind) {
            case ChipKind::Socket: view.socketFilter = chip.value; break;
            case ChipKind::Sort: view.sort = static_cast<BlockSort>(chip.value); break;
            case ChipKind::Tier: view.tierFilter = chip.text; break;
            case ChipKind::Family: view.familyFilter = chip.text; break;
        }
        view.detailScroll = 0;
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    if (!switchLabel.empty() && Inside(layout.switchButton, x, y)) {
        // Hide this overlay first so the other one takes over cleanly.
        Hide();
        if (onSwitch) onSwitch();
        return;
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
    for (std::size_t i = 0; i < layout.rows.size(); ++i) {
        if (!Inside(layout.rows[i], x, y)) continue;
        SelectFiltered(view.listPage * rowsPerPage + i);
        break;
    }
    if (Inside(layout.prev, x, y)) MovePage(-1);
    if (Inside(layout.next, x, y)) MovePage(1);
    InvalidateRect(window, nullptr, FALSE);
}

void OverlayState::HandleWheel(int x, int y, int delta) {
    PageView& view = CurrentView();
    const int steps = delta / WHEEL_DELTA;
    if (Inside(layout.detail, x, y)) {
        view.detailScroll = std::max(0, view.detailScroll - steps * S(40));
    } else if (x < layout.detail.left && y >= layout.search.top) {
        MovePage(steps < 0 ? 1 : -1);
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

// The list shows only each entry's title, wrapped to the row.
void OverlayState::PaintList(HDC dc) {
    const Page& page = CurrentPage();
    PageView& view = CurrentView();
    const std::size_t pages = PageCount(filtered.size(), rowsPerPage);
    view.listPage = std::min(view.listPage, pages - 1);

    const std::wstring countText = std::to_wstring(filtered.size()) + L" of " + std::to_wstring(page.entries.size()) +
                                   L" entries";
    Text(dc, countText, layout.count, kDim, fontSmall, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    if (filtered.empty()) {
        Text(dc, L"No matches", layout.rows[0], kDim, fontBody, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    for (std::size_t i = 0; i < layout.rows.size(); ++i) {
        const std::size_t position = view.listPage * rowsPerPage + i;
        if (position >= filtered.size()) break;
        const Entry& entry = page.entries[filtered[position]];
        const RECT row = layout.rows[i];
        const bool selected = position == view.selected;
        FillPanel(dc, row, selected ? kSelected : kPanel, 185);
        if (selected) FrameColor(dc, row, kGold);
        const RECT titleRect = Rect(row.left + S(10), row.top + S(5), row.right - S(10), row.bottom - S(2));
        Text(dc, Wide(entry.title), titleRect, selected ? kGold : kText, fontBold, DT_LEFT | DT_WORDBREAK);
    }

    const std::wstring pageText = L"Page " + std::to_wstring(view.listPage + 1) + L" of " + std::to_wstring(pages);
    Text(dc, pageText, layout.pageLabel, kText, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    FillPanel(dc, layout.prev, kPanel, 200);
    FrameColor(dc, layout.prev, kBorder);
    Text(dc, L"< Prev", layout.prev, view.listPage > 0 ? kText : kDim, fontBody,
         DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    FillPanel(dc, layout.next, kPanel, 200);
    FrameColor(dc, layout.next, kBorder);
    Text(dc, L"Next >", layout.next, view.listPage + 1 < pages ? kText : kDim, fontBody,
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
    for (const Block& block : entry.blocks) {
        if (!block.family.empty()) families.insert(block.family);
    }
    return std::vector<std::string>(families.begin(), families.end());
}

// Filter and sort bars. Runewords get a socket filter row and socket sorts. Sortable
// item lists (uniques) get Tier and Type filter rows, then level and name sorts. Chips
// wrap onto another line when a row is full.
int OverlayState::BuildControls(HDC dc, const Entry& entry, PageView& view) {
    layout.chips.clear();
    const bool runes = HasRuneBlocks(entry);
    // Tier names (unique tiers, or Hireling difficulties) need the controls even without runes or sorting.
    const bool hasTiers = std::any_of(entry.blocks.begin(), entry.blocks.end(),
                                      [](const Block& block) { return !block.tier.empty(); });
    const bool hasFamilies = std::any_of(entry.blocks.begin(), entry.blocks.end(),
                                         [](const Block& block) { return !block.family.empty(); });
    if (!runes && !entry.sortable && !hasTiers && !hasFamilies) return layout.detail.top + S(12);

    const BlockSort active = ResolveBlockSort(entry, view.sort);
    const std::string tierNow = ActiveFilter(entry, &Block::tier, view.tierFilter);
    const std::string familyNow = ActiveFilter(entry, &Block::family, view.familyFilter);
    const std::vector<std::string> families = DistinctFamilies(entry);

    const int left = layout.detail.left + S(16);
    const int top = layout.detail.top + S(10);
    const int rowHeight = std::max(S(26), smallLine + S(10));
    const int right = layout.detail.right - S(22);
    int labelWidth = 0;
    for (const wchar_t* text : {L"Sockets:", L"Tier:", L"Type:", L"Sort:"}) {
        labelWidth = std::max(labelWidth, MeasureWidth(dc, text, fontSmall));
    }
    labelWidth += S(12);
    const int chipStart = left + labelWidth;

    auto label = [&](int rowY, const wchar_t* text) {
        Text(dc, text, Rect(left, rowY, left + labelWidth, rowY + rowHeight), kDim, fontSmall,
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
        FillPanel(dc, rect, selected ? kSelected : kPanel, 200);
        FrameColor(dc, rect, selected ? kGold : kBorder);
        Text(dc, text, rect, selected ? kGold : kText, fontSmall, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        cursor += chipWidth + S(6);
    };

    int y = top;
    if (runes) {
        label(y, L"Sockets:");
        int rowY = y, cursor = chipStart;
        chip(rowY, cursor, L"All", S(44), ChipKind::Socket, 0, "", view.socketFilter == 0);
        for (std::size_t count : SocketCounts(entry)) {
            chip(rowY, cursor, std::to_wstring(count), S(36), ChipKind::Socket, count, "",
                 view.socketFilter == count);
        }
        y = rowY + rowHeight + S(6);
    }
    // Tier filter: unique tiers, or Hireling difficulties. Only the names present are listed.
    const char* const tierNames[] = {"Normal", "Exceptional", "Elite", "Nightmare", "Hell"};
    {
        if (hasTiers) {
            label(y, L"Tier:");
            int rowY = y, cursor = chipStart;
            chip(rowY, cursor, L"All", S(44), ChipKind::Tier, 0, "", tierNow.empty());
            for (const char* name : tierNames) {
                if (!AnyBlock(entry, &Block::tier, name)) continue;
                chip(rowY, cursor, Wide(name), S(80), ChipKind::Tier, 0, name, tierNow == name);
            }
            y = rowY + rowHeight + S(6);
        }
    }
    {
        if (!families.empty()) {
            label(y, L"Type:");
            int rowY = y, cursor = chipStart;
            chip(rowY, cursor, L"All", S(44), ChipKind::Family, 0, "", familyNow.empty());
            for (const std::string& family : families) {
                chip(rowY, cursor, Wide(family), S(70), ChipKind::Family, 0, family, familyNow == family);
            }
            y = rowY + rowHeight + S(6);
        }
    }

    // Sort chips only do something for level-sorted lists or titled blocks.
    const bool hasTitles = std::any_of(entry.blocks.begin(), entry.blocks.end(),
                                       [](const Block& block) { return !block.title.empty(); });
    if (!entry.sortable && !hasTitles) return y;
    label(y, L"Sort:");
    int rowY = y, cursor = chipStart;
    if (entry.sortable) {
        chip(rowY, cursor, L"Lowest level", S(110), ChipKind::Sort,
             static_cast<std::size_t>(BlockSort::LevelLowFirst), "", active == BlockSort::LevelLowFirst);
        chip(rowY, cursor, L"Highest level", S(110), ChipKind::Sort,
             static_cast<std::size_t>(BlockSort::LevelHighFirst), "", active == BlockSort::LevelHighFirst);
    }
    chip(rowY, cursor, L"A-Z", S(56), ChipKind::Sort, static_cast<std::size_t>(BlockSort::Name), "",
         active == BlockSort::Name);

    return rowY + rowHeight + S(10);
}

// Scrollbar along the right edge of the detail pane. Records the track and thumb
// so mouse input can drag or page through the content.
void OverlayState::DrawScrollbar(HDC dc, int top, int visible, int total, int scroll) {
    layout.scrollable = total > visible && visible > 0;
    layout.scrollTrack = Rect(layout.detail.right - S(14), top, layout.detail.right - S(6), layout.detail.bottom - S(8));
    if (!layout.scrollable) return;
    const RECT track = layout.scrollTrack;
    const int trackHeight = track.bottom - track.top;
    const int thumbHeight = std::max(S(28), trackHeight * visible / total);
    const int maxScroll = total - visible;
    const int thumbTop = track.top + static_cast<int>(static_cast<long long>(trackHeight - thumbHeight) * scroll / maxScroll);
    layout.scrollThumb = Rect(track.left, thumbTop, track.right, thumbTop + thumbHeight);
    FillPanel(dc, track, kSearchFill, 200);
    FillColor(dc, layout.scrollThumb, dragging ? kGold : kBorder);
}

// Height of one item tile: name, art, then its rune icons.
// Height of the tile's tag under the art (for example "(Coupon only)"), with its gap; 0 when there is none.
int OverlayState::TagRowHeight(HDC dc, const Block& block, const TileStyle& style) {
    if (block.tag.empty()) return 0;
    return MeasureHeight(dc, Wide(block.tag), style.tileWidth - S(12), fontSmall) + S(4);
}

int OverlayState::TileHeight(HDC dc, const Block& block, const TileStyle& style) {
    const int nameHeight = MeasureHeight(dc, Wide(block.title), style.tileWidth - S(12), style.nameFont);
    const int runeRows = RuneRows(block.runes.size(), style);
    return style.padding + nameHeight + style.padding + style.artBox + TagRowHeight(dc, block, style) +
           (runeRows > 0 ? S(8) + runeRows * RuneRowHeight(style) : 0) + S(10);
}

// Lays out item blocks as tiles: name over art, with the rune sequence under it.
// Tile hit boxes are added after the tile's rune icons, so a rune icon shows the
// rune tooltip and the rest of the tile shows the item tooltip. Hit boxes that are
// scrolled out of the visible band are skipped.
int OverlayState::RenderTiles(HDC dc, Gdiplus::Graphics* graphics, const Entry& entry,
                              const std::vector<std::size_t>& order, std::size_t begin, std::size_t end, int left,
                              int width_, int y, bool compact, bool draw) {
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
            const bool onScreen = tile.bottom > clipTop && tile.top < clipBottom;
            if (draw && onScreen) {
                FillPanel(dc, tile, kTile, 150);
                FrameColor(dc, tile, kBorder);
            }

            const int nameHeight = MeasureHeight(dc, Wide(block.title), style.tileWidth - S(12), style.nameFont);
            if (draw && onScreen && nameHeight > 0) {
                Text(dc, Wide(block.title),
                     Rect(tileLeft + S(6), y + style.padding, tileLeft + style.tileWidth - S(6),
                          y + style.padding + nameHeight),
                     itemNameColor, style.nameFont, DT_CENTER | DT_WORDBREAK);
            }

            const int artTop = y + style.padding + nameHeight + style.padding;
            const int artSize = block.smallArt ? style.artBox / 2 : style.artBox;
            const int artLeft = tileLeft + (style.tileWidth - artSize) / 2;
            const int artDrawTop = artTop + (style.artBox - artSize) / 2;
            const RECT art = Rect(artLeft, artDrawTop, artLeft + artSize, artDrawTop + artSize);
            if (draw && onScreen) {
                for (const std::string& key : block.images) DrawImageKey(*graphics, key, art);
            }
            const int tagRow = TagRowHeight(dc, block, style);
            if (draw && onScreen && !block.tag.empty()) {
                const RECT tagBox = Rect(tileLeft + S(6), artTop + style.artBox + S(4),
                                         tileLeft + style.tileWidth - S(6), artTop + style.artBox + tagRow);
                Text(dc, Wide(block.tag), tagBox, kGold, fontSmall, DT_CENTER | DT_WORDBREAK);
            }

            if (!block.runes.empty()) {
                const int inner = style.tileWidth - S(12);
                const int perRow = std::max(1, inner / style.cell);
                const int icon = style.cell - S(4);
                const int runeTop = artTop + style.artBox + S(8) + tagRow;
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
                        Text(dc, label, Rect(x, rowTop + icon, x + style.cell, rowTop + icon + smallLine),
                             KanjiLabelColor(label, kGold), fontSmall, DT_CENTER | DT_SINGLELINE);
                    }
                }
            }

            if (draw && tile.bottom > clipTop && tile.top < clipBottom) {
                detailHits.push_back(DetailHit{tile, &entry, &block, -1});
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

        const RECT tile = Rect(left, y, left + tileWidth, y + rowHeight);
        const RECT column = Rect(sideLeft, y, sideLeft + sideWidth, y + rowHeight);
        const bool onScreen = tile.bottom > clipTop && tile.top < clipBottom;
        if (draw && onScreen) {
            FillPanel(dc, tile, kTile, 150);
            FrameColor(dc, tile, kBorder);
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
        if (draw && onScreen) detailHits.push_back(DetailHit{tile, &entry, &block, -1});
        y += rowHeight + gap;
    }
    return y;
}

// Non-item guide block: its title, subtitle, images, fields and text lines.
// Splits a table row into its cells (U+001F separates them).
std::vector<std::string> SplitCells(const std::string& text) {
    std::vector<std::string> cells(1);
    for (const char c : text) {
        if (c == '\x1f') cells.emplace_back();
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
        for (std::size_t c = 0; c < row.size(); ++c) weight[c] = std::max(weight[c], row[c].size());
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
        return Wide(plain);
    };

    // Widen any column whose longest unbreakable word would otherwise be clipped. The extra
    // room comes out of the wider columns in proportion to their width.
    {
        std::vector<int> need(columns, 0);
        const HFONT previous = static_cast<HFONT>(SelectObject(dc, fontBody));
        for (std::size_t r = 0; r < rows.size(); ++r) {
            for (std::size_t c = 0; c < columns; ++c) {
                COLORREF color = kBody;
                const std::wstring text = cellText(r, c, color);
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

    std::string key = std::to_string(width_);
    for (std::size_t i = first; i < last; ++i) {
        key += '|';
        key += static_cast<char>(lines[i].kind);
        key += lines[i].text;
    }
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
                const RECT inner = Rect(x + pad, y + pad, x + colWidth[c] - pad, y + rowHeight - pad);
                if (multiColour(r, c)) LayoutPieces(dc, LinePieces(rawCell(r, c), kBody), kBody, inner, font, DT_LEFT, true);
                else Text(dc, text, inner, color, font, DT_LEFT | DT_WORDBREAK);
                x += colWidth[c];
            }
        }
        y += rowHeight;
    }
    return y + S(6);
}

int OverlayState::RenderGuideBlock(HDC dc, Gdiplus::Graphics* graphics, const Block& block, int left, int width_,
                                   int y, bool draw) {
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
        const std::wstring wide = Wide(field.label + ": " + field.value);
        const int h = MeasureHeight(dc, wide, width_, fontBody);
        if (draw) Text(dc, wide, Rect(left, y, left + width_, y + h), kBody, fontBody, DT_LEFT | DT_WORDBREAK);
        y += h + S(3);
    }
    if (!block.fields.empty()) y += S(4);

    for (std::size_t i = 0; i < block.lines.size();) {
        const Line& line = block.lines[i];
        if (line.kind == LineKind::Row || line.kind == LineKind::Header) {
            std::size_t end = i;
            while (end < block.lines.size() && (block.lines[end].kind == LineKind::Row ||
                                                block.lines[end].kind == LineKind::Header)) {
                ++end;
            }
            y = RenderTable(dc, block.lines, i, end, left, width_, y, draw);
            i = end;
            continue;
        }
        const bool heading = line.kind == LineKind::Heading;
        if (heading) y += S(6);
        const COLORREF base = heading ? kGold : kBody;
        const HFONT font = heading ? fontBold : fontBody;
        std::string plain;
        COLORREF unused = base;
        PlainText(line.text, plain, unused, base);
        const std::wstring wide = Wide(plain);
        const int h = MeasureHeight(dc, wide, width_, font);
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
    const RECT area = layout.detail;
    const int left = area.left + S(16);
    const int width_ = area.right - S(22) - left;
    int y = top - scroll;
    const int origin = y;

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
    if (draw) Text(dc, Wide(entry.title), Rect(left, y, left + width_, y + titleHeight), kGold, fontTitle,
                   DT_LEFT | DT_SINGLELINE);
    y += titleHeight + S(2);
    if (!entry.subtitle.empty()) {
        if (draw) Text(dc, Wide(entry.subtitle), Rect(left, y, left + width_, y + S(20)), kDim, fontSmall,
                       DT_LEFT | DT_SINGLELINE);
        y += S(24);
    }
    y += S(4);

    if (order.empty()) {
        if (draw) Text(dc, L"Nothing matches this filter", Rect(left, y, left + width_, y + S(24)), kDim, fontBody,
                       DT_LEFT | DT_SINGLELINE);
        return y + S(24) - origin;
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
            y = RenderGuideBlock(dc, graphics, first, left, width_, y, draw);
            y += S(6);
            firstBlock = false;
            ++i;
            continue;
        }

        if (headers && !first.section.empty()) {
            const std::wstring header = Wide(first.section);
            const int h = MeasureHeight(dc, header, width_, fontBold);
            if (draw) Text(dc, header, Rect(left, y, left + width_, y + h), greenNames ? CodeColor('2') : kGold, fontBold, DT_LEFT | DT_WORDBREAK);
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
    return y - origin;
}

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
    EnsureBackgroundCache(target, clientWidth, clientHeight);
    BitBlt(dc, 0, 0, clientWidth, clientHeight, backgroundDc, 0, 0, SRCCOPY);

    layout = ComputeLayout();
    const Page& page = CurrentPage();
    greenNames = (page.id == "set-items");
    const bool goldItems = page.id == "uniques" || page.id == "runewords" || page.id == "gemwords";
    itemNameColor = page.id == "set-items" ? CodeColor('2') : page.id == "class-crafting" ? CodeColor('8')
                  : goldItems ? CodeColor('4') : kGold;
    tooltipNameColor = goldItems || page.id == "class-crafting" ? itemNameColor : kGold;

    Text(dc, title, Rect(S(16), S(14), S(300), S(42)), kGold, fontTitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    if (!switchLabel.empty()) {
        FillPanel(dc, layout.switchButton, kPanel, 200);
        FrameColor(dc, layout.switchButton, kGold);
        Text(dc, switchLabel, layout.switchButton, kGold, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    FillPanel(dc, layout.close, kPanel, 200);
    FrameColor(dc, layout.close, kBorder);
    Text(dc, L"X", layout.close, kText, fontBold, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (!query.empty()) {
        FillPanel(dc, layout.clearQuery, kPanel, 200);
        FrameColor(dc, layout.clearQuery, kBorder);
        Text(dc, L"X", layout.clearQuery, kText, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    for (std::size_t i = 0; i < layout.tabs.size(); ++i) {
        const bool active = i == pageIndex;
        FillPanel(dc, layout.tabs[i], active ? kSelected : kPanel, 200);
        FrameColor(dc, layout.tabs[i], active ? kGold : kBorder);
        Text(dc, Wide(database->pages[i].title), layout.tabs[i], active ? kGold : kText, fontBody,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    PaintList(dc);

    detailHits.clear();
    FillPanel(dc, layout.detail, kPanel, 185);
    FrameColor(dc, layout.detail, kBorder);
    PageView& view = CurrentView();
    if (!filtered.empty() && view.selected < filtered.size()) {
        const Entry& entry = page.entries[filtered[view.selected]];
        const int top = BuildControls(dc, entry, view);
        clipTop = top - S(2);
        clipBottom = layout.detail.bottom - 1;
        const std::vector<std::size_t> order = ArrangedOrder(entry, filtered[view.selected], view);
        const bool headers = !entry.sortable && !(HasRuneBlocks(entry) && view.sort == BlockSort::Name);
        const int visible = layout.detail.bottom - top - S(10);
        const int total = RenderDetail(dc, entry, order, headers, top, 0, page.compact, false);
        view.detailScroll = std::clamp(view.detailScroll, 0, std::max(0, total - visible));
        scrollTotal = total;
        scrollVisible = visible;

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
        Text(dc, L"Nothing selected", layout.detail, kDim, fontBody, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    DrawTooltip(dc);
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
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        self->Paint(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTimerFocus) self->SyncVisibility();
        return 0;
    case WM_MOUSEMOVE:
        self->HandleMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSELEAVE:
        self->trackingMouse = false;
        if (!self->dragging) self->ClearHover();
        return 0;
    case WM_LBUTTONDOWN:
        self->HandleClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_LBUTTONUP:
        self->EndDrag();
        return 0;
    case WM_CAPTURECHANGED:
        self->dragging = false;
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        self->HandleWheel(point.x, point.y, GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kIdSearch && HIWORD(wParam) == EN_CHANGE) self->QueryChanged();
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
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW cursorClass{};
    cursorClass.cbSize = sizeof(cursorClass);
    cursorClass.lpfnWndProc = CursorProc;
    cursorClass.hInstance = instance;
    cursorClass.lpszClassName = self->cursorClassName.c_str();
    RegisterClassExW(&cursorClass);
    self->cursorWindow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                             WS_EX_TOPMOST,
                                         self->cursorClassName.c_str(), L"", WS_POPUP, 0, 0, self->S(20), self->S(30),
                                         nullptr, nullptr, instance, nullptr);
    if (self->cursorWindow != nullptr) {
        SetLayeredWindowAttributes(self->cursorWindow, RGB(0, 0, 0), 0, LWA_COLORKEY);
        SetWindowLongPtrW(self->cursorWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&self->lastCursor));
        SetTimer(self->cursorWindow, 1, 8, nullptr);  // about 120 updates a second while shown
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
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
    windowClass.lpszClassName = self->windowClassName.c_str();
    RegisterClassExW(&windowClass);

    WNDCLASSEXW blockerClass{};
    blockerClass.cbSize = sizeof(blockerClass);
    blockerClass.lpfnWndProc = BlockerProc;
    blockerClass.hInstance = windowClass.hInstance;
    blockerClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    blockerClass.lpszClassName = self->blockerClassName.c_str();
    RegisterClassExW(&blockerClass);

    self->CreateFonts();
    self->searchBrush = CreateSolidBrush(kSearchFill);
    self->views.assign(self->database->pages.size(), PageView{});

    self->window = CreateWindowExW(WS_EX_TOOLWINDOW, self->windowClassName.c_str(), self->title.c_str(),
                                   WS_POPUP | WS_CLIPCHILDREN | WS_BORDER, 0, 0, self->S(960), self->S(620),
                                   nullptr, nullptr, windowClass.hInstance, self);
    if (self->window == nullptr) {
        UnregisterClassW(self->blockerClassName.c_str(), blockerClass.hInstance);
        UnregisterClassW(self->windowClassName.c_str(), windowClass.hInstance);
        Gdiplus::GdiplusShutdown(self->gdiplusToken);
        SetEvent(self->ready);
        return 1;
    }

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

    SetTimer(self->window, kTimerFocus, kTimerInterval, nullptr);
    self->FitToGame();
    self->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                   self->layout.search.left, self->layout.search.top,
                                   self->layout.search.right - self->layout.search.left - self->S(26),
                                   self->layout.search.bottom - self->layout.search.top, self->window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSearch)),
                                   windowClass.hInstance, nullptr);
    SendMessageW(self->search, WM_SETFONT, reinterpret_cast<WPARAM>(self->fontBody), TRUE);
    self->Refilter();
    self->started = true;
    SetEvent(self->ready);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && IsWindowVisible(self->window)) {
            if (message.wParam == VK_ESCAPE) {
                self->Hide();
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
    self->images.clear();
    if (self->searchBrush) DeleteObject(self->searchBrush);
    if (self->fontBody) DeleteObject(self->fontBody);
    if (self->fontSmall) DeleteObject(self->fontSmall);
    if (self->fontBold) DeleteObject(self->fontBold);
    if (self->fontTitle) DeleteObject(self->fontTitle);
    UnregisterClassW(self->blockerClassName.c_str(), blockerClass.hInstance);
    UnregisterClassW(self->windowClassName.c_str(), windowClass.hInstance);
    Gdiplus::GdiplusShutdown(self->gdiplusToken);
    return 0;
}

}  // namespace

Overlay::Overlay() : impl_(std::make_unique<OverlayState>()) {}

Overlay::~Overlay() { Stop(); }

bool Overlay::Start(const Database& database, const std::wstring& dataDir, const std::wstring& title,
                    const std::wstring& instanceId, const std::wstring& switchLabel, std::function<void()> onSwitch) {
    if (impl_->started || database.pages.empty()) return false;
    impl_->database = &database;
    impl_->dataDir = dataDir;
    impl_->title = title;
    impl_->windowClassName = L"EsrWikiOverlayWindow-" + instanceId;
    impl_->blockerClassName = L"EsrWikiInputBlocker-" + instanceId;
    impl_->cursorClassName = L"EsrWikiCursor-" + instanceId;
    impl_->switchLabel = switchLabel;
    impl_->onSwitch = std::move(onSwitch);
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

void Overlay::Open() {
    if (impl_->window != nullptr) PostMessageW(impl_->window, kMsgOpen, 0, 0);
}

}  // namespace esrwiki
