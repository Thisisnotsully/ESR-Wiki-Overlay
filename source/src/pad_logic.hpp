// Controller logic that does not touch Windows, so it can be tested anywhere (tests/pad_logic_test.cpp):
// the controller state, the PlayStation report layouts, button repeat, and the navigation rules for the tab grid and
// the on-screen keyboard. The Windows side (reading the devices) is in gamepad.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esrwiki::pad {

// Buttons by their Xbox names; a PlayStation controller maps Cross=A, Circle=B, Square=X, Triangle=Y, L1=LB, R1=RB,
// Options=Start, Share/Create=Back. The triggers are analog and live in State.
enum class Button { A, B, X, Y, LB, RB, L3, R3, Start, Back, Up, Down, Left, Right, Count };
constexpr int kButtonCount = static_cast<int>(Button::Count);

enum class Kind { None, Xbox, PlayStation };

struct State {
    bool down[kButtonCount] = {};
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, up and right positive
    float lt = 0, rt = 0;                  // 0..1
    Kind kind = Kind::None;
    bool Down(Button b) const { return down[static_cast<int>(b)]; }
};

inline float Stick(int raw) {  // an unsigned byte, centre 128, to -1..1
    return std::clamp((static_cast<float>(raw) - 128.0f) / 127.0f, -1.0f, 1.0f);
}

// A circular dead zone, rescaled so the stick starts from 0 at the edge of the zone.
inline void DeadZone(float& x, float& y, float zone) {
    const float length = std::sqrt(x * x + y * y);
    if (length <= zone) {
        x = y = 0;
        return;
    }
    const float scaled = std::min(1.0f, (length - zone) / (1.0f - zone));
    x = x / length * scaled;
    y = y / length * scaled;
}

// Something a person did on the controller (not stick drift, not the idle reports a PlayStation pad sends all day).
inline bool Active(const State& s) {
    for (bool b : s.down) if (b) return true;
    return std::fabs(s.lx) > 0.45f || std::fabs(s.ly) > 0.45f || std::fabs(s.rx) > 0.45f || std::fabs(s.ry) > 0.45f ||
           s.lt > 0.25f || s.rt > 0.25f;
}

// ---- PlayStation reports ------------------------------------------------------------------------------------------
// Raw HID input reports as Windows gives them (first byte is the report id). Layouts:
//   DualShock 4, report 1 (USB, or Bluetooth in its basic mode): sticks at 1..4, buttons at 5..6, triggers at 8..9.
//   DualShock 4, report 0x11 (Bluetooth): the same, two bytes later.
//   DualSense, report 1 over USB: sticks 1..4, triggers 5..6, buttons at 8..9. Over Bluetooth in basic mode the report
//     is only 10 bytes long and has the DualShock 4 layout. Report 0x31 (Bluetooth): sticks at 2..5, triggers 6..7,
//     buttons at 9..10.
// The hat (D-pad) is the low nibble of the first button byte: 0 up, then clockwise, 8 centred.
enum class SonyModel { DualShock4, DualSense };

inline void SonyButtons(State& s, std::uint8_t face, std::uint8_t shoulder) {
    const int hat = face & 0x0F;
    s.down[static_cast<int>(Button::Up)] = hat == 0 || hat == 1 || hat == 7;
    s.down[static_cast<int>(Button::Right)] = hat == 1 || hat == 2 || hat == 3;
    s.down[static_cast<int>(Button::Down)] = hat == 3 || hat == 4 || hat == 5;
    s.down[static_cast<int>(Button::Left)] = hat == 5 || hat == 6 || hat == 7;
    s.down[static_cast<int>(Button::X)] = (face & 0x10) != 0;  // Square
    s.down[static_cast<int>(Button::A)] = (face & 0x20) != 0;  // Cross
    s.down[static_cast<int>(Button::B)] = (face & 0x40) != 0;  // Circle
    s.down[static_cast<int>(Button::Y)] = (face & 0x80) != 0;  // Triangle
    s.down[static_cast<int>(Button::LB)] = (shoulder & 0x01) != 0;
    s.down[static_cast<int>(Button::RB)] = (shoulder & 0x02) != 0;
    s.down[static_cast<int>(Button::Back)] = (shoulder & 0x10) != 0;
    s.down[static_cast<int>(Button::Start)] = (shoulder & 0x20) != 0;
    s.down[static_cast<int>(Button::L3)] = (shoulder & 0x40) != 0;
    s.down[static_cast<int>(Button::R3)] = (shoulder & 0x80) != 0;
}

// Fills `s` from one report; false when the report is not one of the known layouts.
inline bool ParseSonyReport(SonyModel model, const std::uint8_t* d, std::size_t n, State& s) {
    if (d == nullptr || n < 10) return false;
    int sticks = 0, triggers = 0, buttons = 0;
    bool triggersAfterButtons = false;
    if (d[0] == 0x01) {
        if (model == SonyModel::DualSense && n >= 48) {  // USB
            sticks = 1; triggers = 5; buttons = 8;
        } else {  // DualShock 4, or the basic Bluetooth layout of either
            sticks = 1; buttons = 5; triggers = 8; triggersAfterButtons = true;
        }
    } else if (d[0] == 0x11 && model == SonyModel::DualShock4 && n >= 12) {
        sticks = 3; buttons = 7; triggers = 10; triggersAfterButtons = true;
    } else if (d[0] == 0x31 && model == SonyModel::DualSense && n >= 12) {
        sticks = 2; triggers = 6; buttons = 9;
    } else {
        return false;
    }
    (void)triggersAfterButtons;
    s = State{};
    s.lx = Stick(d[sticks]);
    s.ly = -Stick(d[sticks + 1]);  // the controller reports down as larger
    s.rx = Stick(d[sticks + 2]);
    s.ry = -Stick(d[sticks + 3]);
    DeadZone(s.lx, s.ly, 0.22f);
    DeadZone(s.rx, s.ry, 0.22f);
    s.lt = static_cast<float>(d[triggers]) / 255.0f;
    s.rt = static_cast<float>(d[triggers + 1]) / 255.0f;
    SonyButtons(s, d[buttons], d[buttons + 1]);
    s.kind = Kind::PlayStation;
    return true;
}

// The vendor id of Sony, and the products we know.
constexpr std::uint16_t kSonyVendor = 0x054C;
inline bool SonyModelOf(std::uint16_t product, SonyModel& model) {
    switch (product) {
    case 0x05C4: case 0x09CC: case 0x0BA0:  // DualShock 4 (two revisions, and the USB wireless adapter)
        model = SonyModel::DualShock4; return true;
    case 0x0CE6: case 0x0DF2:  // DualSense, DualSense Edge
        model = SonyModel::DualSense; return true;
    default:
        return false;
    }
}

// ---- Button edges and repeat ---------------------------------------------------------------------------------------
// Turns the held buttons into presses. The D-pad repeats while held (after a pause, then quickly); other buttons
// press once. now is in milliseconds.
class Repeater {
public:
    void Step(const State& state, std::uint64_t now, bool (&pressed)[kButtonCount], bool (&released)[kButtonCount]) {
        for (int i = 0; i < kButtonCount; ++i) {
            const bool held = state.down[i];
            pressed[i] = false;
            released[i] = false;
            if (held && !wasDown_[i]) {
                pressed[i] = true;
                nextRepeat_[i] = now + kFirstDelay;
            } else if (held && IsDirection(i) && now >= nextRepeat_[i]) {
                pressed[i] = true;
                nextRepeat_[i] = now + kRepeat;
            } else if (!held && wasDown_[i]) {
                released[i] = true;
            }
            wasDown_[i] = held;
        }
    }
    void Reset() { for (bool& b : wasDown_) b = false; }

    static constexpr std::uint64_t kFirstDelay = 380;
    static constexpr std::uint64_t kRepeat = 85;

private:
    static bool IsDirection(int i) {
        return i == static_cast<int>(Button::Up) || i == static_cast<int>(Button::Down) ||
               i == static_cast<int>(Button::Left) || i == static_cast<int>(Button::Right);
    }
    bool wasDown_[kButtonCount] = {};
    std::uint64_t nextRepeat_[kButtonCount] = {};
};

// ---- Tab grid --------------------------------------------------------------------------------------------------
struct Box { int left = 0, top = 0, right = 0, bottom = 0; };

// The box a direction leads to from `current`: sideways stays in the same row; up and down go to the nearest row in
// that direction and, in it, the box whose centre is closest horizontally. -1 when there is none.
inline int Neighbor(const std::vector<Box>& boxes, int current, int dx, int dy) {
    if (current < 0 || current >= static_cast<int>(boxes.size())) return -1;
    const Box& from = boxes[static_cast<std::size_t>(current)];
    const int fromX = (from.left + from.right) / 2;
    const int fromY = (from.top + from.bottom) / 2;
    int best = -1;
    long long bestScore = 0;
    for (int i = 0; i < static_cast<int>(boxes.size()); ++i) {
        if (i == current) continue;
        const Box& b = boxes[static_cast<std::size_t>(i)];
        const int x = (b.left + b.right) / 2;
        const int y = (b.top + b.bottom) / 2;
        long long score = 0;
        if (dx != 0) {
            if (std::abs(y - fromY) > (from.bottom - from.top) / 2) continue;  // another row
            if ((x - fromX) * dx <= 0) continue;
            score = std::abs(x - fromX);
        } else {
            if ((y - fromY) * dy <= 0) continue;
            if (std::abs(y - fromY) <= (from.bottom - from.top) / 2) continue;  // same row
            score = static_cast<long long>(std::abs(y - fromY)) * 100000 + std::abs(x - fromX);
        }
        if (best < 0 || score < bestScore) { best = i; bestScore = score; }
    }
    return best;
}

// ---- On-screen keyboard -----------------------------------------------------------------------------------------
enum class KeyKind { Char, Backspace, Space, Clear, Done, Accents };

struct OskKey {
    std::wstring label;
    wchar_t ch = 0;
    KeyKind kind = KeyKind::Char;
    int span = 2;  // width in half-key units; a normal key is 2
};

using OskRows = std::vector<std::vector<OskKey>>;

inline std::vector<OskKey> CharRow(const wchar_t* chars) {
    std::vector<OskKey> row;
    for (const wchar_t* c = chars; *c != 0; ++c) row.push_back(OskKey{std::wstring(1, *c), *c, KeyKind::Char, 2});
    return row;
}

// The letters page, or the second page (accented letters; for Russian, the Latin letters). `locale` is the loader's
// language code (deDE, frFR, ruRU ...); English and the languages typed with composed characters (Japanese, Korean,
// Chinese) keep the English layout, since a controller cannot compose those.
inline std::vector<OskKey> ControlRow(const std::wstring& toggle, int units) {
    return {OskKey{toggle, 0, KeyKind::Accents, 3}, OskKey{L"Space", L' ', KeyKind::Space, units - 11},
            OskKey{L"Clear", 0, KeyKind::Clear, 4}, OskKey{L"Done", 0, KeyKind::Done, 4}};
}

inline OskRows OskLayout(bool accents, const std::string& locale = std::string()) {
    // letters of a page: rows 2-4 as strings (the third row gets the backspace key), the digits row, the toggle label
    struct Page { std::wstring digits, r1, r2, r3, r4, toggle; };
    const std::wstring digits = L"1234567890";
    const std::wstring commonAccents = L"àáâãäåæçèéêëìíîïñòóôõöøùúûüýÿœßąćęłńśźżšžč";
    const auto accentPage = [&](const std::wstring& specials, const std::wstring& toggle) {
        std::wstring pool = specials;
        for (wchar_t c : commonAccents)
            if (pool.find(c) == std::wstring::npos) pool += c;
        return Page{pool.substr(0, 10), pool.substr(10, 10), pool.substr(20, 9), pool.substr(29, 10), L"", toggle};
    };
    const std::wstring latin1 = L"qwertyuiop", latin2 = L"asdfghjkl", latin3 = L"zxcvbnm-'.";
    Page page;
    if (locale == "ruRU") {
        page = accents ? Page{digits, latin1, latin2, latin3, L"", L"АБВ"}  // the Latin letters, for names in English
                       : Page{L"1234567890-.", L"йцукенгшщзхъ", L"фывапролджэ", L"ячсмитьбюё.,", L"", L"ABC"};
    } else if (locale == "deDE") {
        page = accents ? accentPage(L"äöüß", L"ABC") : Page{digits, L"qwertzuiop", L"asdfghjkl", L"yxcvbnm-'.", L"", L"äö"};
    } else if (locale == "frFR") {
        page = accents ? accentPage(L"éèêàçùœâîôû", L"ABC") : Page{digits, L"azertyuiop", L"qsdfghjkl", L"mwxcvbn'-.", L"", L"éà"};
    } else if (locale == "esES" || locale == "esMX") {
        page = accents ? accentPage(L"ñáéíóúü", L"ABC") : Page{digits, latin1, latin2, latin3, L"", L"ñá"};
    } else if (locale == "itIT") {
        page = accents ? accentPage(L"àèéìòù", L"ABC") : Page{digits, latin1, latin2, latin3, L"", L"àè"};
    } else if (locale == "ptBR") {
        page = accents ? accentPage(L"ãõçáâéêíóôú", L"ABC") : Page{digits, latin1, latin2, latin3, L"", L"ãç"};
    } else if (locale == "plPL") {
        page = accents ? accentPage(L"ąćęłńóśźż", L"ABC") : Page{digits, latin1, latin2, latin3, L"", L"ął"};
    } else {
        // English (and any language typed with composed characters)
        OskRows rows;
        if (!accents) {
            rows.push_back(CharRow(L"1234567890"));
            rows.push_back(CharRow(L"qwertyuiop"));
            std::vector<OskKey> third = CharRow(L"asdfghjkl");
            third.push_back(OskKey{L"⌫", 0, KeyKind::Backspace, 2});
            rows.push_back(third);
            rows.push_back(CharRow(L"zxcvbnm-'."));
        } else {
            rows.push_back(CharRow(L"àáâãäåæçñß"));  // a-grave .. sharp s
            rows.push_back(CharRow(L"èéêëìíîïœÿ"));
            std::vector<OskKey> third = CharRow(L"òóôõöøùúû");
            third.push_back(OskKey{L"⌫", 0, KeyKind::Backspace, 2});
            rows.push_back(third);
            rows.push_back(CharRow(L"üłąęśźżćńž"));
        }
        rows.push_back({OskKey{accents ? L"ABC" : L"éà", 0, KeyKind::Accents, 3},
                        OskKey{L"Space", L' ', KeyKind::Space, 9},
                        OskKey{L"Clear", 0, KeyKind::Clear, 4},
                        OskKey{L"Done", 0, KeyKind::Done, 4}});
        return rows;
    }
    OskRows rows;
    rows.push_back(CharRow(page.digits.c_str()));
    rows.push_back(CharRow(page.r1.c_str()));
    std::vector<OskKey> third = CharRow(page.r2.c_str());
    third.push_back(OskKey{L"⌫", 0, KeyKind::Backspace, 2});
    rows.push_back(third);
    rows.push_back(CharRow(page.r3.c_str()));
    if (!page.r4.empty()) rows.push_back(CharRow(page.r4.c_str()));
    int units = 20;
    for (const auto& row : rows) {
        int total = 0;
        for (const OskKey& key : row) total += key.span;
        units = std::max(units, total);
    }
    rows.push_back(ControlRow(page.toggle, units));
    return rows;
}

inline int RowSpan(const std::vector<OskKey>& row) {
    int total = 0;
    for (const OskKey& key : row) total += key.span;
    return total;
}

// Moves the highlight. Sideways stays in the row; up and down pick, in the next row, the key under the current key's
// centre. Stops at the edges.
inline void OskMove(const OskRows& rows, int& row, int& col, int dx, int dy) {
    if (rows.empty()) return;
    row = std::clamp(row, 0, static_cast<int>(rows.size()) - 1);
    col = std::clamp(col, 0, static_cast<int>(rows[static_cast<std::size_t>(row)].size()) - 1);
    if (dx != 0) {  // past either end of a row: on to the other end of it
        const int size = static_cast<int>(rows[static_cast<std::size_t>(row)].size());
        col = ((col + dx) % size + size) % size;
        return;
    }
    if (dy == 0) return;
    const int count = static_cast<int>(rows.size());
    const int target = ((row + dy) % count + count) % count;  // past the top or bottom row: on to the other end
    const auto& from = rows[static_cast<std::size_t>(row)];
    int centre = 0;  // in half-key units, scaled to the target row's width
    for (int i = 0; i < col; ++i) centre += from[static_cast<std::size_t>(i)].span;
    centre = centre * 2 + from[static_cast<std::size_t>(col)].span;  // twice the centre
    const auto& to = rows[static_cast<std::size_t>(target)];
    const double scaled = static_cast<double>(centre) * RowSpan(to) / (2.0 * std::max(1, RowSpan(from)));
    int start = 0, pick = 0;
    for (int i = 0; i < static_cast<int>(to.size()); ++i) {
        const int end = start + to[static_cast<std::size_t>(i)].span;
        pick = i;
        if (scaled < end) break;
        start = end;
    }
    row = target;
    col = pick;
}

// ---- Cursor and number smoothing ---------------------------------------------------------------------------------
// Eases a speed toward its target over `dt` seconds (an exponential approach, so it never overshoots). It is quicker to
// slow down than to speed up, so the cursor stops soon after the stick is let go instead of drifting on.
inline float SmoothToward(float velocity, float target, float dt, float riseSeconds = 0.05f, float fallSeconds = 0.025f) {
    if (dt <= 0.0f) return velocity;
    const bool slowing = std::fabs(target) < std::fabs(velocity) || target * velocity < 0.0f;
    const float tau = slowing ? fallSeconds : riseSeconds;
    const float k = 1.0f - std::exp(-dt / tau);
    return velocity + (target - velocity) * k;
}

// How far one press changes a number held down with the D-pad: 1 at first, then 5, then 10. `repeats` counts the
// presses already made in this hold (the first press is 0).
inline int NumberStep(int repeats) { return repeats < 6 ? 1 : (repeats < 16 ? 5 : 10); }

}  // namespace esrwiki::pad
