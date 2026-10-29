#include "web_uniques_live.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

namespace esrwiki {
namespace {

// ---- Text helpers (the Python twin uses str.split(), html.unescape and re) -----------------------------------

const char kColorLead[] = "\xC3\xBF" "c";  // the colour-code lead character, then 'c'

std::size_t SpaceLength(const std::string& s, std::size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F)) return 1;
    const auto at = [&](std::size_t k) { return i + k < s.size() ? static_cast<unsigned char>(s[i + k]) : 0; };
    if (c == 0xC2 && (at(1) == 0x85 || at(1) == 0xA0)) return 2;
    if (c == 0xE1 && at(1) == 0x9A && at(2) == 0x80) return 3;
    if (c == 0xE2 && at(1) == 0x80 && ((at(2) >= 0x80 && at(2) <= 0x8A) || at(2) == 0xA8 || at(2) == 0xA9 || at(2) == 0xAF)) return 3;
    if (c == 0xE2 && at(1) == 0x81 && at(2) == 0x9F) return 3;
    if (c == 0xE3 && at(1) == 0x80 && at(2) == 0x80) return 3;
    return 0;
}

void AppendUtf8(std::string& out, unsigned long code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

std::string Unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const std::size_t end = s.find(';', i);
        if (end == std::string::npos || end - i > 10) {
            out += s[i];
            continue;
        }
        const std::string name = s.substr(i + 1, end - i - 1);
        bool done = true;
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name == "nbsp") AppendUtf8(out, 0xA0);
        else if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string digits = name.substr(hex ? 2 : 1);
            bool valid = !digits.empty();
            unsigned long value = 0;
            for (char d : digits) {
                const int v = (d >= '0' && d <= '9') ? d - '0' : hex && d >= 'a' && d <= 'f' ? d - 'a' + 10
                              : hex && d >= 'A' && d <= 'F' ? d - 'A' + 10 : -1;
                if (v < 0) { valid = false; break; }
                value = value * (hex ? 16 : 10) + static_cast<unsigned long>(v);
            }
            if (valid && value > 0 && value <= 0x10FFFF) AppendUtf8(out, value);
            else done = false;
        } else {
            done = false;
        }
        if (done) i = end;
        else out += s[i];
    }
    return out;
}

// html.unescape, then whitespace runs (Unicode spaces too) become one space and the ends are trimmed.
std::string Clean(const std::string& raw) {
    const std::string s = Unescape(raw);
    std::string out;
    bool pending = false;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = SpaceLength(s, i);
        if (n != 0) {
            pending = !out.empty();
            i += n;
            continue;
        }
        if (pending) out += ' ';
        pending = false;
        out += s[i++];
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string Upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::string Norm(const std::string& s) {
    std::string out;
    for (char c : Lower(s)) if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
    return out;
}

std::string Trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && SpaceLength(s, a) != 0) a += SpaceLength(s, a);
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool StartsWith(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }
bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = text.find('\n', start);
        if (at == std::string::npos) { lines.push_back(text.substr(start)); break; }
        lines.push_back(text.substr(start, at - start));
        start = at + 1;
    }
    return lines;
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = line.find('\t', start);
        if (at == std::string::npos) { parts.push_back(line.substr(start)); break; }
        parts.push_back(line.substr(start, at - start));
        start = at + 1;
    }
    return parts;
}

// Case-insensitive find.
std::size_t FindCI(const std::string& hay, const std::string& needle, std::size_t from = 0) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    const std::string lower = Lower(needle);
    for (std::size_t i = from; i + needle.size() <= hay.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size()) {
            char c = hay[i + k];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != lower[k]) break;
            ++k;
        }
        if (k == needle.size()) return i;
    }
    return std::string::npos;
}

// Text between tags, as tag-free pieces: re.split(r"<[^>]+>", s) with each piece cleaned, empty ones dropped.
std::vector<std::string> Pieces(const std::string& cell) {
    std::vector<std::string> out;
    std::string current;
    for (std::size_t i = 0; i < cell.size();) {
        if (cell[i] == '<') {
            const std::size_t end = cell.find('>', i + 1);
            if (end != std::string::npos && end > i + 1) {
                const std::string text = Clean(current);
                if (!text.empty()) out.push_back(text);
                current.clear();
                i = end + 1;
                continue;
            }
        }
        current += cell[i++];
    }
    const std::string text = Clean(current);
    if (!text.empty()) out.push_back(text);
    return out;
}

// re.sub(r"<[^>]+>", " ", s), cleaned.
std::string TextOf(const std::string& cell) {
    std::string spaced;
    for (std::size_t i = 0; i < cell.size();) {
        if (cell[i] == '<') {
            const std::size_t end = cell.find('>', i + 1);
            if (end != std::string::npos && end > i + 1) {
                spaced += ' ';
                i = end + 1;
                continue;
            }
        }
        spaced += cell[i++];
    }
    return Clean(spaced);
}

// "(10 to 20)" -> "10-20"
bool Number(const std::string& s, std::size_t& i) {
    std::size_t k = i;
    if (k < s.size() && s[k] == '-') ++k;
    const std::size_t digits = k;
    while (k < s.size() && s[k] >= '0' && s[k] <= '9') ++k;
    if (k == digits) return false;
    if (k + 1 < s.size() && s[k] == '.' && s[k + 1] >= '0' && s[k + 1] <= '9') {
        ++k;
        while (k < s.size() && s[k] >= '0' && s[k] <= '9') ++k;
    }
    i = k;
    return true;
}

std::string DashRanges(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '(') {
            std::size_t k = i + 1;
            const std::size_t a0 = k;
            if (Number(s, k) && s.compare(k, 4, " to ") == 0) {
                const std::size_t a1 = k;
                k += 4;
                const std::size_t b0 = k;
                if (Number(s, k) && k < s.size() && s[k] == ')') {
                    out += s.substr(a0, a1 - a0) + "-" + s.substr(b0, k - b0);
                    i = k + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

bool HiddenLine(const std::string& text) {
    static const std::string kHidden = "tinkering points";
    if (text.size() < kHidden.size() || Lower(text.substr(0, kHidden.size())) != kHidden) return false;
    if (text.size() == kHidden.size()) return true;
    const char c = text[kHidden.size()];
    return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
}

// ---- Website pages --------------------------------------------------------------------------------------------

struct Line {
    char color;
    std::string text;
};

struct Record {
    std::string page, heading, group, title, base, code, gamble, ilvl, level, note;
    bool coupon = false;
    std::vector<Line> lines;
};

char ColorOf(const std::string& name) {
    const std::string upper = Upper(name);
    if (upper == "4850B8") return '3';
    if (upper == "ORANGE") return '8';
    if (upper == "WHITE") return '0';
    if (upper == "RED") return '1';
    return '3';
}

std::vector<Line> PropertyLines(const std::string& cell) {
    std::vector<Line> lines;
    std::vector<char> stack;  // 0 = no colour of its own
    std::string current;
    char lineColor = '3';
    auto color = [&]() {
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) if (*it != 0) return *it;
        return '3';
    };
    auto flush = [&]() {
        const std::string text = Clean(current);
        current.clear();
        if (!text.empty()) lines.push_back({lineColor, text});
    };
    for (std::size_t i = 0; i < cell.size();) {
        if (cell[i] == '<') {
            const std::size_t end = cell.find('>', i);
            if (end != std::string::npos) {
                const std::string token = cell.substr(i, end - i + 1);
                std::size_t k = 1;
                const bool closing = k < token.size() && token[k] == '/';
                if (closing) ++k;
                while (k < token.size() && (token[k] == ' ' || token[k] == '\t' || token[k] == '\n' || token[k] == '\r')) ++k;
                std::string name;
                while (k < token.size() && ((token[k] >= 'a' && token[k] <= 'z') || (token[k] >= 'A' && token[k] <= 'Z') ||
                                            (token[k] >= '0' && token[k] <= '9')))
                    name += token[k++];
                name = Lower(name);
                if (name == "br") {
                    flush();
                } else if (name == "font") {
                    if (closing) {
                        if (!stack.empty()) stack.pop_back();
                    } else {
                        char value = 0;
                        const std::size_t at = FindCI(token, "color");
                        if (at != std::string::npos) {
                            std::size_t p = at + 5;
                            while (p < token.size() && (token[p] == ' ' || token[p] == '\t')) ++p;
                            if (p < token.size() && token[p] == '=') {
                                ++p;
                                while (p < token.size() && (token[p] == ' ' || token[p] == '\t')) ++p;
                                if (p < token.size() && token[p] == '"') ++p;
                                if (p < token.size() && token[p] == '#') ++p;
                                std::string word;
                                while (p < token.size() && ((token[p] >= '0' && token[p] <= '9') || (token[p] >= 'a' && token[p] <= 'z') ||
                                                            (token[p] >= 'A' && token[p] <= 'Z')))
                                    word += token[p++];
                                if (!word.empty()) value = ColorOf(word);
                            }
                        }
                        stack.push_back(value);
                    }
                }
                i = end + 1;
                continue;
            }
        }
        // A run of text up to the next tag.
        std::size_t stop = cell.find('<', i + 1);
        if (stop == std::string::npos) stop = cell.size();
        const std::string token = cell.substr(i, stop - i);
        if (!Clean(token).empty() && Clean(current).empty()) lineColor = color();
        current += token;
        i = stop;
    }
    flush();
    std::vector<Line> out;
    for (const Line& line : lines) {
        const std::string text = DashRanges(line.text);
        if (HiddenLine(text)) continue;
        out.push_back({line.color, text});
    }
    return out;
}

// The text of each <tag ...>...</tag> found in `html`, in order. `from`/`to` are positions of the inner text.
struct Span {
    std::size_t begin, end;
};

std::vector<Span> Elements(const std::string& html, const std::string& open, const std::string& close) {
    std::vector<Span> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t start = FindCI(html, open, pos);
        if (start == std::string::npos) break;
        const std::size_t gt = html.find('>', start);
        if (gt == std::string::npos) break;
        const std::size_t stop = FindCI(html, close, gt + 1);
        if (stop == std::string::npos) break;
        out.push_back({gt + 1, stop});
        pos = stop + close.size();
    }
    return out;
}

const char* HeadingGroup(const std::string& page, const std::string& heading) {
    if (page == "unique_mythicals.htm") return "Mythical";
    static const std::map<std::string, const char*> table = {
        {"Large Charm", "Charm"}, {"Grand Charm", "Charm"}, {"Odd Charm", "Charm"}, {"Small Charm", "Charm"},
        {"Jewel", "Jewel"}, {"Bow Quiver", "Quiver"}, {"Crossbow Quiver", "Quiver"},
        {"Helm", "Helm"}, {"Pelt", "Helm"}, {"Primal Helm", "Helm"}, {"Circlet", "Helm"}, {"Spirit Crown", "Helm"},
        {"Body Armor", "Torso"}, {"Cloak", "Torso"}, {"Robe", "Torso"},
        {"Shield", "Shield"}, {"Auric Shields", "Shield"}, {"Voodoo Heads", "Shield"}, {"Grimoire", "Shield"},
        {"Gloves", "Gloves"}, {"Boots", "Boots"}, {"Belt", "Belt"}};
    const auto found = table.find(heading);
    if (found != table.end()) return found->second;
    if (EndsWith(heading, "Ring")) return "Ring";
    if (EndsWith(heading, "Amulet")) return "Amulet";
    if (page == "unique_weapons.htm") return "Weapon";
    return "";
}

bool IsCode(const std::string& s) {
    if (s.size() != 3) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    return true;
}

// "Mancatcher (7br)" -> base "Mancatcher", code "7br"; no code leaves the text as it is.
std::string StripCode(const std::string& text, std::string* code = nullptr) {
    if (text.size() >= 5 && text.back() == ')' && text[text.size() - 5] == '(' && IsCode(text.substr(text.size() - 4, 3))) {
        if (code != nullptr) *code = text.substr(text.size() - 4, 3);
        return Trim(text.substr(0, text.size() - 5));
    }
    return text;
}

std::string LevelValue(const std::string& text, const std::string& label) {
    std::string found;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t at = text.find(label + ":", pos);
        if (at == std::string::npos) break;
        std::size_t k = at + label.size() + 1;
        while (k < text.size() && SpaceLength(text, k) != 0) k += SpaceLength(text, k);
        const std::size_t digits = k;
        while (k < text.size() && text[k] >= '0' && text[k] <= '9') ++k;
        if (k > digits) found = text.substr(digits, k - digits);
        pos = at + label.size() + 1;
    }
    return found;
}

std::vector<Record> ParsePage(const std::string& name, const std::string& html) {
    std::vector<Record> records;
    std::string heading;
    for (const Span& row : Elements(html, "<tr", "</tr>")) {
        const std::string rowText = html.substr(row.begin, row.end - row.begin);
        std::vector<std::string> cells;
        for (const Span& cell : Elements(rowText, "<td", "</td>")) cells.push_back(rowText.substr(cell.begin, cell.end - cell.begin));
        if (cells.size() == 1 && FindCI(rowText, "colspan") != std::string::npos) {
            heading = TextOf(cells[0]);
            continue;
        }
        if (cells.size() != 4 || cells[1].find("Item Level") == std::string::npos) continue;
        // The part of the first cell before the coupon text.
        const std::size_t cut = FindCI(cells[0], "<font face=\"arial,helvetica\" size=\"-1\" color");
        const std::string head = cut == std::string::npos ? cells[0] : cells[0].substr(0, cut);
        std::vector<std::string> parts = Pieces(head);
        Record rec;
        const auto gamble = std::find(parts.begin(), parts.end(), "Gamble Item:");
        if (gamble != parts.end()) {
            if (gamble + 1 != parts.end()) rec.gamble = *(gamble + 1);
            parts.erase(gamble, parts.end());
        }
        if (parts.empty()) continue;
        rec.title = parts[0];
        if (EndsWith(rec.title, "(") && parts.size() >= 4) {
            rec.title += parts[1] + ")";
            rec.base = parts[3];
        } else {
            rec.base = parts.size() > 1 ? parts[1] : "";
        }
        rec.base = StripCode(rec.base, &rec.code);
        const std::string levels = TextOf(cells[1]);
        rec.page = name;
        rec.heading = heading;
        rec.group = HeadingGroup(name, heading);
        rec.coupon = cells[0].find("Ancient Coupon Unique") != std::string::npos;
        rec.ilvl = LevelValue(levels, "Item Level");
        rec.level = LevelValue(levels, "Required Level");
        rec.lines = PropertyLines(cells[2]);
        rec.note = TextOf(cells[3]);
        records.push_back(rec);
    }
    return records;
}

// ---- The page ---------------------------------------------------------------------------------------------------

struct Group {
    const char* key;
    const char* title;
};
const Group kGroups[] = {{"Mythical", "Mythical Uniques"}, {"Weapon", "Weapons"}, {"Helm", "Helms"}, {"Torso", "Torso"},
                         {"Shield", "Shields"}, {"Belt", "Belts"}, {"Boots", "Boots"}, {"Gloves", "Gloves"},
                         {"Ring", "Rings"}, {"Amulet", "Amulets"}, {"Jewel", "Jewels"}, {"Charm", "Charms"},
                         {"Quiver", "Quivers"}};

const char kPageId[] = "uniques";
const char kPageTitle[] = "Uniques";

// Page span [begin, end) of each page id in `lines`.
std::map<std::string, std::pair<std::size_t, std::size_t>> PageSpans(const std::vector<std::string>& lines) {
    std::map<std::string, std::pair<std::size_t, std::size_t>> spans;
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < lines.size(); ++i) if (StartsWith(lines[i], "PAGE\t")) starts.push_back(i);
    for (std::size_t n = 0; n < starts.size(); ++n) {
        const std::vector<std::string> parts = SplitTabs(lines[starts[n]]);
        if (parts.size() > 1) spans[parts[1]] = {starts[n], n + 1 < starts.size() ? starts[n + 1] : lines.size()};
    }
    return spans;
}

// "Tathamet's Awakening (Fire Variant)" -> "Tathamet's Awakening"
std::string WithoutVariant(const std::string& title) {
    if (!EndsWith(title, "Variant)")) return title;
    const std::size_t open = title.rfind('(');
    if (open == std::string::npos || title.find(')', open) != title.size() - 1) return title;
    std::size_t end = open;
    while (end > 0 && title[end - 1] == ' ') --end;
    return title.substr(0, end);
}

}  // namespace

bool BuildWebUniquesWikiData(const std::string& shipped, const std::vector<std::string>& pages,
                             const std::string& artTable, std::string& out, std::string& error) {
    if (pages.size() != 4) {
        error = "expected the four website pages";
        return false;
    }
    std::vector<Record> records;
    for (std::size_t n = 0; n < 4; ++n) {
        if (FindCI(pages[n], "</html") == std::string::npos) {
            error = std::string(kWebUniquePageNames[n]) + " is incomplete";
            return false;
        }
        std::vector<Record> found = ParsePage(kWebUniquePageNames[n], pages[n]);
        if (found.empty()) {
            error = std::string("no items found in ") + kWebUniquePageNames[n];
            return false;
        }
        records.insert(records.end(), found.begin(), found.end());
    }
    if (records.size() < 900) {
        error = "only " + std::to_string(records.size()) + " items found; the website pages look broken";
        return false;
    }

    std::vector<std::string> lines = SplitLines(shipped);
    const bool trailing = !lines.empty() && lines.back().empty();
    if (trailing) lines.pop_back();

    // web-unique-art.txt: the art, tier and weapon family the website does not give, by unique name and by base code.
    struct Art {
        std::string img, tier, family;  // img is a whole IMG line, or empty
    };
    std::map<std::string, Art> byTitle, byTitleNorm, byCode;
    for (const std::string& line : SplitLines(artTable)) {
        const std::vector<std::string> parts = SplitTabs(line);
        if (parts.size() < 6 || (parts[0] != "UNIQUE" && parts[0] != "BASE")) continue;
        Art art;
        if (!parts[2].empty()) art.img = "IMG\t" + parts[2] + (parts[3].empty() ? std::string() : "\t" + parts[3]);
        art.tier = parts[4];
        art.family = parts[5];
        (parts[0] == "UNIQUE" ? byTitle : byCode)[parts[1]] = art;
    }
    for (const auto& entry : byTitle) byTitleNorm[Norm(entry.first)] = entry.second;

    std::vector<std::vector<std::vector<std::string>>> entries(sizeof(kGroups) / sizeof(kGroups[0]));
    for (const Record& rec : records) {
        std::size_t g = 0;
        while (g < entries.size() && rec.group != kGroups[g].key) ++g;
        if (g == entries.size()) {
            error = "an item sits under an unknown table heading: " + rec.title;
            return false;
        }
        const bool mythical = rec.group == "Mythical";
        // The unique's own art first, else the art of its base item (by the code the website shows).
        const Art* own = nullptr;
        if (auto it = byTitle.find(rec.title); it != byTitle.end()) own = &it->second;
        else if (auto itn = byTitleNorm.find(Norm(rec.title)); itn != byTitleNorm.end()) own = &itn->second;
        else if (auto itp = byTitle.find(WithoutVariant(rec.title)); itp != byTitle.end()) own = &itp->second;
        const auto byBase = byCode.find(rec.code);
        const Art* baseArt = byBase != byCode.end() ? &byBase->second : nullptr;
        std::string img = own != nullptr ? own->img : std::string();
        if (img.empty() && baseArt != nullptr) img = baseArt->img;
        // Tier and weapon family belong to the base item.
        std::pair<std::string, std::string> sort;
        if (!mythical) {
            if (baseArt != nullptr) sort = {baseArt->tier, baseArt->family};
            else if (own != nullptr) sort = {own->tier, own->family};
        }
        // The website's own categories (weapon types, helm and shield kinds).
        if (rec.group == "Weapon" || rec.group == "Helm" || rec.group == "Torso" || rec.group == "Shield") sort.second = rec.heading;
        std::vector<std::string> block;
        block.push_back("BLOCK\t1\t" + rec.title + "\t" + (mythical ? rec.base : std::string()) + "\t\t" + sort.first +
                        "\t" + sort.second + "\t" +
                        (mythical ? std::string("0") : rec.level.empty() ? std::string("0") : rec.level));
        if (rec.group == "Charm") {
            // charms cannot be gambled and are not coupon items
        } else if (rec.coupon) {
            block.push_back("TAG\t(Coupon only)");
        } else if (!rec.gamble.empty() && Upper(rec.gamble) != "N/A") {
            block.push_back("TAG\tGamble Item:\\n" + StripCode(rec.gamble) + " - ilvl: " + rec.ilvl);
        }
        if (!mythical && !rec.base.empty()) block.push_back("FIELD\tBase\t" + rec.base);
        if (!rec.level.empty()) block.push_back("FIELD\tRequired Level\t" + rec.level);
        if (!img.empty()) block.push_back(img);
        for (const Line& line : rec.lines) block.push_back(std::string("LINE\tS\t") + kColorLead + line.color + line.text);
        if (!rec.note.empty()) block.push_back("NOTE\t" + rec.note);
        entries[g].push_back(std::move(block));
    }

    std::vector<std::string> page;
    page.push_back(std::string("PAGE\t") + kPageId + "\t" + kPageTitle + "\tWiki");
    for (std::size_t g = 0; g < entries.size(); ++g) {
        if (entries[g].empty()) continue;
        page.push_back(std::string("ENTRY\t") + kGroups[g].title + "\t" + std::to_string(entries[g].size()) + " items\t" +
                       (std::string(kGroups[g].key) == "Mythical" ? "wide" : "sortable"));
        for (const auto& block : entries[g]) page.insert(page.end(), block.begin(), block.end());
    }

    // Replace the page with this id; a file without one gets it before 'set-items' (else at the end).
    const auto spans = PageSpans(lines);
    std::vector<std::string> kept;
    const auto own = spans.find(kPageId);
    if (own != spans.end()) {
        kept.assign(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(own->second.first));
        kept.insert(kept.end(), page.begin(), page.end());
        kept.insert(kept.end(), lines.begin() + static_cast<std::ptrdiff_t>(own->second.second), lines.end());
    } else {
        const auto next = spans.find("set-items");
        const std::size_t at = next != spans.end() ? next->second.first : lines.size();
        kept.assign(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(at));
        kept.insert(kept.end(), page.begin(), page.end());
        kept.insert(kept.end(), lines.begin() + static_cast<std::ptrdiff_t>(at), lines.end());
    }

    std::string text;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (i != 0) text += '\n';
        text += kept[i];
    }
    if (trailing) text += '\n';
    out = std::move(text);
    return true;
}

}  // namespace esrwiki
