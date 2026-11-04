// Tests of src/pad_logic.hpp: PlayStation report parsing, button repeat, tab-grid and keyboard navigation.
#include <cstdio>
#include <cstring>

#include "pad_logic.hpp"

using namespace esrwiki::pad;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                   \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static void Ds4Usb() {
    std::uint8_t r[64] = {};
    r[0] = 0x01;
    r[1] = r[2] = r[3] = r[4] = 128;
    r[5] = 0x08;  // hat centred
    State s;
    CHECK(ParseSonyReport(SonyModel::DualShock4, r, sizeof r, s));
    CHECK(s.kind == Kind::PlayStation);
    CHECK(!Active(s));
    r[5] = 0x08 | 0x20;  // Cross
    r[6] = 0x20;         // Options
    r[8] = 255;          // L2
    r[3] = 255;          // right stick fully right
    r[4] = 0;            // and up
    CHECK(ParseSonyReport(SonyModel::DualShock4, r, sizeof r, s));
    CHECK(s.Down(Button::A) && s.Down(Button::Start) && !s.Down(Button::B));
    CHECK(s.lt > 0.99f && s.rt == 0.0f);
    CHECK(s.rx > 0.6f && s.ry > 0.6f);  // up is positive (a full diagonal is 0.7 on each axis)
    CHECK(Active(s));
    r[5] = 0x02;  // hat east
    CHECK(ParseSonyReport(SonyModel::DualShock4, r, sizeof r, s));
    CHECK(s.Down(Button::Right) && !s.Down(Button::Up) && !s.Down(Button::Down));
    r[5] = 0x07 | 0x80;  // hat north-west, Triangle
    CHECK(ParseSonyReport(SonyModel::DualShock4, r, sizeof r, s));
    CHECK(s.Down(Button::Up) && s.Down(Button::Left) && s.Down(Button::Y));
    r[6] = 0x80 | 0x02;  // R3 and R1
    CHECK(ParseSonyReport(SonyModel::DualShock4, r, sizeof r, s));
    CHECK(s.Down(Button::R3) && s.Down(Button::RB));
}

static void Ds4Bluetooth() {
    std::uint8_t r[78] = {};
    r[0] = 0x11;
    r[3] = r[4] = r[5] = r[6] = 128;
    r[7] = 0x08 | 0x40;  // Circle
    r[10] = 0;
    r[11] = 200;  // R2
    State s;
    CHECK(ParseSonyReport(SonyModel::DualShock4, r, sizeof r, s));
    CHECK(s.Down(Button::B) && s.rt > 0.7f);
    CHECK(!ParseSonyReport(SonyModel::DualSense, r, sizeof r, s));  // 0x11 is not a DualSense report
}

static void DualSense() {
    std::uint8_t r[64] = {};
    r[0] = 0x01;
    r[1] = 0;    // left stick full left
    r[2] = 128;
    r[3] = r[4] = 128;
    r[5] = 255;  // L2
    r[6] = 0;
    r[8] = 0x08 | 0x10;  // Square
    r[9] = 0x20 | 0x01;  // Options, L1
    State s;
    CHECK(ParseSonyReport(SonyModel::DualSense, r, sizeof r, s));
    CHECK(s.lx < -0.9f && s.lt > 0.99f);
    CHECK(s.Down(Button::X) && s.Down(Button::Start) && s.Down(Button::LB));
    // The basic Bluetooth report is short and has the DualShock 4 layout.
    std::uint8_t b[10] = {0x01, 128, 128, 128, 128, 0x08 | 0x20, 0, 0, 0, 255};
    CHECK(ParseSonyReport(SonyModel::DualSense, b, sizeof b, s));
    CHECK(s.Down(Button::A) && s.rt > 0.99f);
    // Bluetooth full report.
    std::uint8_t f[78] = {};
    f[0] = 0x31;
    f[2] = f[3] = f[4] = f[5] = 128;
    f[9] = 0x08 | 0x80;  // Triangle
    f[10] = 0x80;        // R3
    CHECK(ParseSonyReport(SonyModel::DualSense, f, sizeof f, s));
    CHECK(s.Down(Button::Y) && s.Down(Button::R3));
    CHECK(!ParseSonyReport(SonyModel::DualSense, f, 5, s));  // too short
    f[0] = 0x07;
    CHECK(!ParseSonyReport(SonyModel::DualSense, f, sizeof f, s));  // unknown report
}

static void Products() {
    SonyModel m;
    CHECK(SonyModelOf(0x05C4, m) && m == SonyModel::DualShock4);
    CHECK(SonyModelOf(0x09CC, m) && m == SonyModel::DualShock4);
    CHECK(SonyModelOf(0x0CE6, m) && m == SonyModel::DualSense);
    CHECK(SonyModelOf(0x0DF2, m) && m == SonyModel::DualSense);
    CHECK(!SonyModelOf(0x1234, m));
}

static void DeadZoneTest() {
    float x = 0.1f, y = 0.1f;
    DeadZone(x, y, 0.22f);
    CHECK(x == 0 && y == 0);
    x = 1.0f; y = 0.0f;
    DeadZone(x, y, 0.22f);
    CHECK(x > 0.99f);
    x = 0.3f; y = 0.0f;
    DeadZone(x, y, 0.22f);
    CHECK(x > 0.0f && x < 0.2f);
}

static void Repeat() {
    Repeater rep;
    State s;
    bool p[kButtonCount], r[kButtonCount];
    const int down = static_cast<int>(Button::Down), a = static_cast<int>(Button::A);
    s.down[down] = true;
    s.down[a] = true;
    rep.Step(s, 1000, p, r);
    CHECK(p[down] && p[a]);
    rep.Step(s, 1100, p, r);
    CHECK(!p[down] && !p[a]);  // still inside the first delay
    rep.Step(s, 1000 + Repeater::kFirstDelay, p, r);
    CHECK(p[down] && !p[a]);  // the D-pad repeats, A does not
    rep.Step(s, 1000 + Repeater::kFirstDelay + 20, p, r);
    CHECK(!p[down]);
    rep.Step(s, 1000 + Repeater::kFirstDelay + Repeater::kRepeat, p, r);
    CHECK(p[down]);
    s.down[down] = false;
    rep.Step(s, 2000, p, r);
    CHECK(r[down] && !r[a]);
}

static std::vector<Box> TwoRows() {
    // Row 0: three tabs; row 1: two tabs (as in the wiki: pages are grouped into rows).
    return {{0, 0, 100, 30}, {110, 0, 210, 30}, {220, 0, 320, 30}, {0, 40, 150, 70}, {160, 40, 320, 70}};
}

static void Tabs() {
    const auto b = TwoRows();
    CHECK(Neighbor(b, 0, 1, 0) == 1);
    CHECK(Neighbor(b, 1, 1, 0) == 2);
    CHECK(Neighbor(b, 2, 1, 0) == -1);   // no wrap
    CHECK(Neighbor(b, 0, -1, 0) == -1);
    CHECK(Neighbor(b, 3, 1, 0) == 4);    // stays in its row
    CHECK(Neighbor(b, 0, 0, 1) == 3);    // down from the first tab
    CHECK(Neighbor(b, 2, 0, 1) == 4);    // down from the last tab of row 0
    CHECK(Neighbor(b, 1, 0, 1) == 3 || Neighbor(b, 1, 0, 1) == 4);
    CHECK(Neighbor(b, 3, 0, 1) == -1);   // nothing below: the caller moves on to the list
    CHECK(Neighbor(b, 4, 0, -1) == 2);   // up goes back
    CHECK(Neighbor(b, 0, 0, -1) == -1);
    CHECK(Neighbor(b, 9, 1, 0) == -1);   // out of range
}

static void Keyboard() {
    OskRows rows = OskLayout(false);
    CHECK(rows.size() == 5);
    CHECK(rows[0].size() == 10 && rows[1].size() == 10 && rows[3].size() == 10);
    CHECK(rows[4].size() == 4 && rows[4][3].kind == KeyKind::Done);
    for (const auto& row : rows) CHECK(RowSpan(row) == 20);
    CHECK(RowSpan(rows[4]) == 20);
    int row = 1, col = 0;
    OskMove(rows, row, col, -1, 0);
    CHECK(row == 1 && col == 9);                 // past the left end: the right end of the row
    OskMove(rows, row, col, 1, 0);
    CHECK(row == 1 && col == 0);                 // and back round
    OskMove(rows, row, col, 1, 0);
    CHECK(col == 1);
    OskMove(rows, row, col, 0, 1);
    CHECK(row == 2 && col == 1);                 // 'w' to 's'
    CHECK(rows[2][static_cast<std::size_t>(col)].ch == L's');
    OskMove(rows, row, col, 0, -1);
    CHECK(row == 1 && col == 1);
    // Down into the bottom row lands on a key under the current one.
    row = 3; col = 0;
    OskMove(rows, row, col, 0, 1);
    CHECK(row == 4 && col == 0);                 // 'z' above the accents key
    row = 3; col = 9;
    OskMove(rows, row, col, 0, 1);
    CHECK(row == 4 && rows[4][static_cast<std::size_t>(col)].kind == KeyKind::Done);
    row = 3; col = 5;
    OskMove(rows, row, col, 0, 1);
    CHECK(row == 4 && (rows[4][static_cast<std::size_t>(col)].kind == KeyKind::Space ||
                       rows[4][static_cast<std::size_t>(col)].kind == KeyKind::Clear));
    row = 4; col = 3;
    OskMove(rows, row, col, 0, 1);
    CHECK(row == 0);                             // below the bottom row: the top row
    row = 0; col = 4;
    OskMove(rows, row, col, 0, -1);
    CHECK(row == 4);                             // above the top row: the bottom row
    OskRows accents = OskLayout(true);
    CHECK(accents.size() == 5 && accents[0].size() == 10 && accents[4][0].kind == KeyKind::Accents);
}

static void Smoothing() {
    // Speeding up approaches the target and never passes it.
    float v = 0.0f;
    float last = 0.0f;
    for (int i = 0; i < 400; ++i) {
        v = SmoothToward(v, 1000.0f, 0.001f);
        CHECK(v >= last && v <= 1000.0f);
        last = v;
    }
    CHECK(v > 990.0f);                                         // 400 ms later it is at speed
    CHECK(SmoothToward(0.0f, 1000.0f, 0.001f) < 30.0f);        // but one millisecond in, it has barely moved
    // Slowing down is quicker than speeding up.
    const float up = SmoothToward(0.0f, 1000.0f, 0.02f);
    const float down = 1000.0f - SmoothToward(1000.0f, 0.0f, 0.02f);
    CHECK(down > up);
    // A reversal counts as slowing down.
    CHECK(SmoothToward(500.0f, -500.0f, 0.02f) < 500.0f - 400.0f * 0.5f);
    // Zero target comes to rest and a zero time step changes nothing.
    float w = 800.0f;
    for (int i = 0; i < 300; ++i) w = SmoothToward(w, 0.0f, 0.001f);
    CHECK(std::fabs(w) < 1.0f);
    CHECK(SmoothToward(123.0f, 0.0f, 0.0f) == 123.0f);
    // The result does not depend on how the time is cut up (to within rounding).
    float a = SmoothToward(0.0f, 1000.0f, 0.016f);
    float b = 0.0f;
    for (int i = 0; i < 16; ++i) b = SmoothToward(b, 1000.0f, 0.001f);
    CHECK(std::fabs(a - b) < 1.0f);
}

static void NumberSteps() {
    CHECK(NumberStep(0) == 1 && NumberStep(5) == 1);
    CHECK(NumberStep(6) == 5 && NumberStep(15) == 5);
    CHECK(NumberStep(16) == 10 && NumberStep(200) == 10);
}

void KeyboardLanguages() {
    for (const char* code : {"", "deDE", "frFR", "esES", "esMX", "itIT", "ptBR", "plPL", "ruRU", "jaJP", "koKR", "zhCN", "zhTW"}) {
        for (bool second : {false, true}) {
            const OskRows rows = OskLayout(second, code);
            CHECK(rows.size() >= 5);
            int widest = 0;
            for (const auto& row : rows)
                if (&row != &rows.back()) widest = std::max(widest, RowSpan(row));
            CHECK(RowSpan(rows.back()) == std::max(20, widest));  // the bottom row is as wide as the widest
            CHECK(rows.back().size() == 4);
        }
    }
    CHECK(OskLayout(false, "ruRU")[1][0].ch == L'\u0439');  // Cyrillic first
    CHECK(OskLayout(false, "frFR")[1][0].ch == L'a');        // AZERTY
    CHECK(OskLayout(false, "deDE")[1][5].ch == L'z');        // QWERTZ
    CHECK(OskLayout(false, "jaJP")[1][0].ch == L'q');        // composed languages keep the English layout
}

int main() {
    KeyboardLanguages();
    Smoothing();
    NumberSteps();
    Ds4Usb();
    Ds4Bluetooth();
    DualSense();
    Products();
    DeadZoneTest();
    Repeat();
    Tabs();
    Keyboard();
    if (failures == 0) std::printf("pad_logic_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
