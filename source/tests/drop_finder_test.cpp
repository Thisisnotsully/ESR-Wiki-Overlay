// Checks the C++ Drop Finder engine against the chances the website's own script gives. The expected values come
// from tests/drop_finder_reference.js (run with Node on the saved page's data) and are stored in
// tests/drop_finder/expected.json.
//   drop_finder_test <data/drop-finder folder> <expected.json>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "../src/drop_finder.hpp"
#include "../src/json_lite.hpp"

namespace {

using esrwiki::drop::Bases;
using esrwiki::drop::Data;
using esrwiki::drop::Engine;
using esrwiki::drop::OutMap;
using esrwiki::drop::Selection;
using esrwiki::drop::Settings;
using esrwiki::json::Value;

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    out = text.str();
    return true;
}

int failures = 0;

void Fail(const std::string& what) {
    ++failures;
    if (failures <= 20) std::fprintf(stderr, "FAIL %s\n", what.c_str());
}

bool Close(double a, double b) {
    return std::fabs(a - b) <= 1e-12 * std::max(1.0, std::fabs(b));
}

int Int(const Value& v) {
    return static_cast<int>(v.number);
}

Settings ReadSettings(const Value& v) {
    Settings s;
    s.players = Int(v.Field("players"));
    s.mf = Int(v.Field("mf"));
    s.iq = Int(v.Field("iq"));
    return s;
}

// Compares a produced map with the expected [key, chance] pairs of the saved file.
void CompareMap(const OutMap& got, const Value& want, const std::string& label) {
    if (got.size() != want.items.size()) {
        Fail(label + ": key count " + std::to_string(got.size()) + " expected " + std::to_string(want.items.size()));
        return;
    }
    for (const Value& kv : want.items) {
        const std::string key = kv.items[0].text;
        const double p = kv.items[1].number;
        const auto found = got.find(key);
        if (found == got.end() || !Close(found->second, p)) {
            Fail(label + ": key " + key + " expected " + std::to_string(p));
            return;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: drop_finder_test <data/drop-finder folder> <expected.json>\n");
        return 2;
    }
    std::string dataText;
    std::string expectedText;
    if (!ReadFile(std::string(argv[1]) + "/df-data.json", dataText) || !ReadFile(argv[2], expectedText)) {
        std::fprintf(stderr, "cannot read the data or the expected file\n");
        return 2;
    }
    Data data;
    std::string error;
    if (!esrwiki::drop::LoadData(dataText, data, error)) {
        std::fprintf(stderr, "df-data.json: %s\n", error.c_str());
        return 2;
    }
    Value expected;
    if (!esrwiki::json::Parse(expectedText, expected, error)) {
        std::fprintf(stderr, "expected.json: %s\n", error.c_str());
        return 2;
    }

    Engine engine(std::move(data));
    std::size_t monsterCount = 0;
    Settings current{};
    bool haveSettings = false;
    for (const Value& c : expected.Field("monsters").items) {
        const Settings s = ReadSettings(c.Field("settings"));
        if (!haveSettings || s.players != current.players || s.mf != current.mf || s.iq != current.iq) {
            engine.SetSettings(s);
            current = s;
            haveSettings = true;
        }
        const std::string mon = c.Field("mon").text;
        Selection sel;
        sel.superunique = mon[0] == 's';
        sel.i = std::stoi(mon.substr(1));
        sel.d = Int(c.Field("diff"));
        sel.t = Int(c.Field("type"));
        sel.x = Int(c.Field("terror"));
        sel.z = Int(c.Field("zone"));
        const std::string label = "monster " + mon + " zone " + std::to_string(sel.z) + " d" +
                                  std::to_string(sel.d) + " t" + std::to_string(sel.t) + " x" + std::to_string(sel.x);

        const esrwiki::drop::Resolved r = engine.Resolve(sel);
        if (r.tc != Int(c.Field("tc")) || r.qtc != Int(c.Field("qtc")) || r.ilvl != Int(c.Field("ilvl")) ||
            r.pe != c.Field("pe").boolean) {
            Fail(label + ": resolved treasure class or level differs");
            continue;
        }
        const OutMap main = r.tc >= 0 ? engine.TopMap(r.tc, r.c, r.pe) : OutMap{};
        CompareMap(main, c.Field("main"), label + " main");
        const Value& quest = c.Field("quest");
        if (quest.kind == Value::Kind::Null) {
            if (r.qtc >= 0) Fail(label + ": quest expected none");
        } else {
            CompareMap(r.qtc >= 0 ? engine.TopMap(r.qtc, r.c, r.pe) : OutMap{}, quest, label + " quest");
        }
        ++monsterCount;
    }

    // "Dropped by" lookups: the chance a monster drops an item, at the default settings.
    engine.SetSettings(Settings{});
    std::size_t whoCount = 0;
    for (const Value& w : expected.Field("who").items) {
        const std::string item = w.Field("item").text;
        const Bases B = engine.BasesOf(item);
        const std::string mon = w.Field("mon").text;
        Selection sel;
        sel.superunique = mon[0] == 's';
        sel.i = std::stoi(mon.substr(1));
        sel.d = Int(w.Field("diff"));
        sel.t = Int(w.Field("type"));
        sel.x = 0;
        sel.z = -1;
        const esrwiki::drop::Resolved r = engine.Resolve(sel);
        const double a = engine.Top1(r.tc, r.c, r.pe, item, B);
        const double b = engine.Top1(r.qtc, r.c, r.pe, item, B);
        const std::string label = "who " + item + " " + mon;
        if (!Close(a, w.Field("a").number) || !Close(b, w.Field("b").number)) {
            Fail(label + ": chance differs");
        }
        ++whoCount;
    }

    if (failures) {
        std::fprintf(stderr, "%d failure(s) in %zu monster and %zu item cases\n", failures, monsterCount, whoCount);
        return 1;
    }
    std::printf("drop finder: %zu monster and %zu item cases match the website\n", monsterCount, whoCount);
    return 0;
}
