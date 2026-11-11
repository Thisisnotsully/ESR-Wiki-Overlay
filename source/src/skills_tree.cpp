// The Skills tab's portable view model: see skills_tree.hpp.
#include "skills_tree.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

namespace esrwiki::skills {

namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

std::string LowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string TrimSpaces(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

double Round6(double v) {
    if (!std::isfinite(v)) return v;
    const double r = std::round(v * 1e6) / 1e6;
    return r == 0 ? 0 : r;  // no -0
}

// A skill by display name: same class first, then anywhere.
const Skill* FindByName(const Data& data, const std::string& name, const std::string& classId) {
    if (name.empty()) return nullptr;
    const Skill* any = nullptr;
    for (const Skill& s : data.skills) {
        if (s.name != name) continue;
        if (s.cls == classId) return &s;
        if (any == nullptr) any = &s;
    }
    return any;
}

// ---------------------------------------------------------------------------------------------------------------
// Line texts, including the ones the card hides
// ---------------------------------------------------------------------------------------------------------------

constexpr double kMarker = 7777777;
const char kMarkerText[] = "7777777";

struct LineText {
    bool present = false;
    bool shown = false;
    std::string text;
};

// The line's text at lvl. A line the card hides (a percentage that comes out as 0, "0% Physical Damage Converted
// to Magic Damage") is formatted again with each zero value replaced by a marker number, and the marker turned
// back into 0: the same text the formatter would give if it did not hide the line.
LineText TextAt(Calculator& calc, const Record& rec, const Line& line, int lvl) {
    LineText out;
    if (line.t == 0) return out;
    std::optional<std::string> text = calc.FormatLine(rec, line, lvl);
    if (text) {
        if (text->empty()) return out;
        out.present = true;
        out.shown = true;
        out.text = std::move(*text);
        return out;
    }
    const bool divided = line.t == 36 || line.t == 79;
    const double b = line.y.Empty() ? 0 : calc.Run(rec, line.y, lvl);
    Line copy = line;
    bool marked = false;
    const auto mark = [&calc, &rec, lvl, &marked](Formula& f, double scale) {
        if (f.Empty()) return;
        if (calc.Run(rec, f, lvl) != 0) return;
        Formula number;
        number.kind = Formula::Kind::Number;
        number.number = kMarker * scale;
        f = number;
        marked = true;
    };
    mark(copy.x, divided && b != 0 ? b : 1);
    if (!divided) mark(copy.y, 1);
    mark(copy.z, 1);
    if (!marked) return out;
    text = calc.FormatLine(rec, copy, lvl);
    if (!text) return out;
    std::string fixed;
    bool replaced = false;
    const std::string marker(kMarkerText);
    for (std::size_t i = 0; i < text->size();) {
        if (text->compare(i, marker.size(), marker) == 0) {
            fixed += '0';
            i += marker.size();
            replaced = true;
        } else {
            fixed += (*text)[i++];
        }
    }
    if (!replaced || fixed.empty()) return out;
    out.present = true;
    out.text = std::move(fixed);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Label / value split
// ---------------------------------------------------------------------------------------------------------------

struct Parts {
    std::size_t p = 0;  // common prefix length
    std::size_t q = 0;  // common suffix length
};

Parts CommonParts(const std::vector<const std::string*>& texts) {
    Parts parts;
    if (texts.empty()) return parts;
    std::size_t minLen = texts[0]->size();
    for (const std::string* t : texts) minLen = std::min(minLen, t->size());
    const std::string& s0 = *texts[0];
    std::size_t p = 0;
    while (p < minLen) {
        bool same = true;
        for (const std::string* t : texts)
            if ((*t)[p] != s0[p]) {
                same = false;
                break;
            }
        if (!same) break;
        ++p;
    }
    std::size_t q = 0;
    while (q < minLen - p) {
        const char c = s0[s0.size() - 1 - q];
        bool same = true;
        for (const std::string* t : texts)
            if ((*t)[t->size() - 1 - q] != c) {
                same = false;
                break;
            }
        if (!same) break;
        ++q;
    }
    // The prefix bytes are the same in every text: cut only at a character boundary.
    {
        std::size_t boundary = 0;
        std::size_t i = 0;
        while (i < p) {
            const unsigned char lead = static_cast<unsigned char>(s0[i]);
            const std::size_t len = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (i + len > p) break;
            i += len;
            boundary = i;
        }
        p = boundary;
    }
    while (q > 0 && IsContinuation(s0[s0.size() - q])) --q;
    // Never cut a number: take its sign, digits, decimals and range into the value.
    while (p > 0) {
        const char c = s0[p - 1];
        const char before = p >= 2 ? s0[p - 2] : '\0';
        if (IsDigit(c) || (c == '.' && IsDigit(before)) || ((c == '+' || c == '-') && !IsAsciiLetter(before)) ||
            ((c == '/' || c == '~') && IsDigit(before))) {
            --p;
        } else {
            break;
        }
    }
    while (q > 0) {
        const std::size_t j = s0.size() - q;
        const char c = s0[j];
        const char next = q >= 2 ? s0[j + 1] : '\0';
        if (IsDigit(c) || c == '%' || ((c == '.' || c == '-' || c == '+' || c == '/' || c == '~') && IsDigit(next))) {
            --q;
        } else {
            break;
        }
    }
    parts.p = p;
    parts.q = q;
    return parts;
}

// text without prefix / suffix, or empty when it does not have them.
std::string Middle(const std::string& text, const std::string& prefix, const std::string& suffix) {
    if (text.size() < prefix.size() + suffix.size()) return std::string();
    if (text.compare(0, prefix.size(), prefix) != 0) return std::string();
    if (text.compare(text.size() - suffix.size(), suffix.size(), suffix) != 0) return std::string();
    return text.substr(prefix.size(), text.size() - prefix.size() - suffix.size());
}

std::string StripColon(std::string s) {
    for (;;) {
        s = TrimSpaces(s);
        if (!s.empty() && s.back() == ':') {
            s.pop_back();
            continue;
        }
        static const std::string kWideColon = "\xEF\xBC\x9A";
        if (s.size() >= kWideColon.size() && s.compare(s.size() - kWideColon.size(), kWideColon.size(), kWideColon) == 0) {
            s.resize(s.size() - kWideColon.size());
            continue;
        }
        return s;
    }
}

std::string MakeLabel(const std::string& prefix, const std::string& suffix) {
    std::string a = StripColon(prefix);
    std::string b = TrimSpaces(suffix);
    std::string label = a.empty() ? b : b.empty() ? a : a + " " + b;
    for (char& c : label)
        if (c == '\n') c = ' ';
    return StripColon(label);
}

bool IsNumericValue(const std::string& v) {
    if (v.empty()) return false;
    bool digit = false;
    for (const char c : v) {
        if (IsDigit(c)) {
            digit = true;
            continue;
        }
        if (c != '.' && c != '+' && c != '-' && c != '%' && c != '/' && c != '~' && c != ' ') return false;
    }
    return digit;
}

// Numbers with more than one decimal rounded to one, as the page's table shows them ("5.75" -> "5.8").
std::string RoundDecimals(const std::string& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size();) {
        if (!IsDigit(v[i])) {
            out += v[i++];
            continue;
        }
        std::size_t j = i;
        while (j < v.size() && IsDigit(v[j])) ++j;
        if (j + 2 < v.size() && v[j] == '.' && IsDigit(v[j + 1]) && IsDigit(v[j + 2])) {
            std::size_t k = j + 1;
            while (k < v.size() && IsDigit(v[k])) ++k;
            std::string r = js::ToFixed(js::StringToNumber(v.substr(i, k - i)), 1);
            if (r.size() > 2 && r.compare(r.size() - 2, 2, ".0") == 0) r.resize(r.size() - 2);
            out += r;
            i = k;
        } else {
            out.append(v, i, j - i);
            i = j;
        }
    }
    return out;
}

// Splits a cell's text with the row's label parts. Returns whether the number part is only a number.
bool SplitCell(const ValueRow& row, ValueCell& c) {
    const std::string middle = Middle(c.text, row.prefix, row.suffix);
    c.split = !middle.empty();
    if (!c.split) {
        c.exactValue = c.text;  // the left-out form, or no match: the full text
        c.value = RoundDecimals(c.exactValue);
        return false;
    }
    c.exactValue = TrimSpaces(row.suffixInValue ? middle + row.suffix : middle);
    c.value = RoundDecimals(c.exactValue);
    return IsNumericValue(TrimSpaces(middle));
}

int ClampLevel(const Data& data, int level) { return std::clamp(level, 1, std::max(1, data.levels.max)); }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Trees
// ---------------------------------------------------------------------------------------------------------------

int TreeLayout::IndexOf(const Skill* skill) const {
    for (std::size_t i = 0; i < cells.size(); ++i)
        if (cells[i].skill == skill) return static_cast<int>(i);
    return -1;
}

int TreeLayout::CellAt(int row, int col) const {
    for (std::size_t i = 0; i < cells.size(); ++i)
        if (cells[i].row == row && cells[i].col == col) return static_cast<int>(i);
    return -1;
}

TreeLayout BuildTree(const Data& data, const std::string& classId, int treeN, bool dropEmptyRows) {
    TreeLayout layout;
    layout.classId = classId;
    layout.tree = treeN;
    const std::vector<const Skill*> skills = data.SkillsOfTree(classId, treeN);
    for (const Skill* s : skills) {
        layout.maxDataRow = std::max(layout.maxDataRow, s->row);
        layout.maxDataCol = std::max(layout.maxDataCol, s->col);
    }
    // Data row -> grid row.
    std::vector<int> rowOf(static_cast<std::size_t>(layout.maxDataRow) + 1, -1);
    {
        std::set<int> used;
        for (const Skill* s : skills)
            if (s->row >= 1) used.insert(s->row);
        int next = 0;
        for (int r = 1; r <= layout.maxDataRow; ++r) {
            if (dropEmptyRows) {
                if (used.count(r) != 0) rowOf[static_cast<std::size_t>(r)] = next++;
            } else {
                rowOf[static_cast<std::size_t>(r)] = r - 1;
            }
        }
        layout.rows = dropEmptyRows ? next : layout.maxDataRow;
    }
    std::set<std::pair<int, int>> taken;
    std::vector<std::size_t> pending;
    layout.cells.reserve(skills.size());
    for (const Skill* s : skills) {
        TreeCell cell;
        cell.skill = s;
        cell.dataRow = s->row;
        cell.dataCol = s->col;
        cell.row = -1;
        cell.col = -1;
        const bool valid = s->row >= 1 && s->col >= 1 && s->col <= kTreeColumns;
        if (valid) {
            const std::pair<int, int> at(rowOf[static_cast<std::size_t>(s->row)], s->col - 1);
            if (taken.insert(at).second) {
                cell.row = at.first;
                cell.col = at.second;
            }
        }
        if (cell.row < 0) pending.push_back(layout.cells.size());
        layout.cells.push_back(cell);
    }
    for (const std::size_t i : pending) {
        TreeCell& cell = layout.cells[i];
        int start = cell.dataRow >= 1 ? rowOf[static_cast<std::size_t>(cell.dataRow)] : layout.rows;
        if (start < 0) start = layout.rows;
        bool placed = false;
        for (int r = start; !placed; ++r) {
            for (int c = 0; c < kTreeColumns && !placed; ++c) {
                if (!taken.insert({r, c}).second) continue;
                cell.row = r;
                cell.col = c;
                placed = true;
            }
        }
        cell.moved = true;
        layout.rows = std::max(layout.rows, cell.row + 1);
        std::ostringstream msg;
        msg << classId << " tree " << treeN << ": " << cell.skill->key << " at row " << cell.dataRow << " col "
            << cell.dataCol << (cell.dataRow >= 1 && cell.dataCol >= 1 && cell.dataCol <= kTreeColumns
                                    ? " (cell taken)" : " (no / bad position)")
            << " placed at grid " << cell.row << "," << cell.col;
        layout.issues.push_back(msg.str());
    }
    for (const TreeCell& cell : layout.cells) layout.cols = std::max(layout.cols, cell.col + 1);
    // Prerequisite links within the tree.
    std::set<std::pair<std::size_t, std::size_t>> links;
    for (std::size_t to = 0; to < layout.cells.size(); ++to) {
        const Skill& s = *layout.cells[to].skill;
        for (const std::string& name : s.prerequisites) {
            int from = -1;
            for (std::size_t k = 0; k < layout.cells.size(); ++k)
                if (layout.cells[k].skill->name == name) {
                    from = static_cast<int>(k);
                    break;
                }
            if (from < 0 || static_cast<std::size_t>(from) == to) {
                layout.outsidePrerequisites.emplace_back(&s, name);
                continue;
            }
            links.insert({to, static_cast<std::size_t>(from)});
        }
    }
    for (const auto& [to, from] : links) layout.links.push_back(TreeLink{from, to});
    return layout;
}

std::vector<const Skill*> ItemSkillList(const Data& data) { return data.SkillsOfClass("other"); }

// ---------------------------------------------------------------------------------------------------------------
// Header facts
// ---------------------------------------------------------------------------------------------------------------

std::string FormatNumber(double v) { return js::NumberToString(Round6(v)); }

HeaderFacts BuildHeaderFacts(const Data& data, Engine& engine, const Context& ctx) {
    HeaderFacts facts;
    facts.skill = ctx.skill;
    if (ctx.skill == nullptr || ctx.skill->record == nullptr) return facts;
    const Skill& skill = *ctx.skill;
    const Record& rec = *skill.record;
    if (const Class* c = data.FindClass(skill.cls)) facts.classTitle = c->title;
    if (const Tree* t = data.FindTree(skill.cls, skill.tree)) facts.treeTitle = t->title;
    // Row 1 skills carry no required level in the data: they are available from level 1.
    facts.requiredLevelGiven = skill.requiredLevel > 0;
    facts.requiredLevel = skill.requiredLevel > 0 ? skill.requiredLevel : skill.tree > 0 ? 1 : 0;
    facts.requiredLevelWithPrerequisites = std::max(facts.requiredLevel, data.RequiredLevelWithPrerequisites(skill));
    facts.maxLevel = static_cast<int>(rec.mx);
    facts.passive = skill.passive;
    facts.aura = skill.aura;
    facts.hardPointsOnly = skill.hardPoints;

    Calculator& calc = engine.CalculatorFor(ctx);
    // Mana: the line that reads usmc (Mana Cost), else the record's mana columns.
    const Line* manaLine = nullptr;
    for (const std::vector<Line>* group : {&skill.general, &skill.levelLines})
        for (const Line& line : *group)
            if (manaLine == nullptr && line.x.kind == Formula::Kind::Text && line.x.text.find("usmc") != std::string::npos)
                manaLine = &line;
    if (manaLine != nullptr) {
        const auto manaAt = [&calc, &rec, manaLine](int lvl) {
            const double a = calc.Run(rec, manaLine->x, lvl);
            const double b = manaLine->y.Empty() ? 0 : calc.Run(rec, manaLine->y, lvl);
            return Round6(b != 0 ? a / b : a);
        };
        const double m1 = manaAt(1);
        const double m2 = manaAt(2);
        const double m3 = manaAt(3);
        const double m20 = manaAt(20);
        facts.manaAtChosen = manaAt(ClampLevel(data, ctx.skillLevel));
        facts.hasMana = m1 != 0 || m2 != 0 || m3 != 0 || m20 != 0 || facts.manaAtChosen != 0;
        if (facts.hasMana) {
            const double d = Round6(m2 - m1);
            facts.manaLinear = std::fabs((m3 - m2) - d) < 1e-6 && std::fabs(m1 + 19 * d - m20) < 1e-6;
            facts.manaAtLevel1 = m1;
            facts.manaPerLevel = facts.manaLinear ? d : 0;
        }
    }
    // The paired 20-level bonus.
    for (const Line& line : skill.synergyLines) {
        if (line.t != 18) continue;
        std::optional<std::string> text = calc.FormatLine(rec, line, ClampLevel(data, ctx.skillLevel));
        if (!text) continue;
        const std::size_t nl = text->find('\n');
        if (nl == std::string::npos) continue;
        PairedBonus bonus;
        bonus.header = text->substr(0, nl);
        bonus.text = text->substr(nl + 1);
        // The longest other skill name in the header: same class first, then any class (item skills).
        for (int pass = 0; pass < 2 && bonus.partner == nullptr; ++pass) {
            std::size_t best = 0;
            for (const Skill& other : data.skills) {
                if (&other == &skill || other.name.empty() || other.name.size() <= best) continue;
                if (pass == 0 && other.cls != skill.cls) continue;
                if (other.name == skill.name) continue;
                if (bonus.header.find(other.name) == std::string::npos) continue;
                bonus.partner = &other;
                best = other.name.size();
            }
        }
        facts.pairedBonuses.push_back(std::move(bonus));
    }
    return facts;
}

// ---------------------------------------------------------------------------------------------------------------
// Values table
// ---------------------------------------------------------------------------------------------------------------

std::vector<int> DefaultSampleLevels(const Data& data) {
    std::vector<int> levels = {1, 20, 30, 50, data.levels.max};
    std::vector<int> out;
    for (const int l : levels) {
        const int c = ClampLevel(data, l);
        if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    }
    return out;
}

std::vector<SynergyInput> SynergySkills(const Data& data, const Skill& skill) {
    std::vector<SynergyInput> out;
    for (const std::string& id : skill.synergies)
        if (const Skill* s = data.FindById(id)) out.push_back(SynergyInput{s, data.levels.synergy, false});
    for (const std::string& id : skill.levelLinks)
        if (const Skill* s = data.FindById(id)) out.push_back(SynergyInput{s, data.levels.max, true});
    return out;
}

std::map<std::string, int> MaxAll(const Data& data, const Skill& skill) {
    std::map<std::string, int> out;
    for (const SynergyInput& in : SynergySkills(data, skill)) out[in.skill->id] = in.maxPoints;
    return out;
}

ValueTable BuildValueTable(const Data& data, Engine& engine, const Context& ctx, const std::vector<int>& levels) {
    ValueTable table;
    table.skill = ctx.skill;
    if (ctx.skill == nullptr || ctx.skill->record == nullptr) return table;
    const Skill& skill = *ctx.skill;
    const Record& rec = *skill.record;
    table.chosenLevel = ClampLevel(data, ctx.skillLevel);
    for (const int l : levels) table.levels.push_back(ClampLevel(data, l));
    Calculator& calc = engine.CalculatorFor(ctx);

    const auto makeCell = [&calc, &rec](const Line& line, int lvl) {
        LineText t = TextAt(calc, rec, line, lvl);
        ValueCell cell;
        cell.level = lvl;
        cell.present = t.present;
        cell.shownInCard = t.shown;
        cell.text = std::move(t.text);
        return cell;
    };

    const auto addGroup = [&](const std::vector<Line>& lines, LineGroup group) {
        for (std::size_t index = 0; index < lines.size(); ++index) {
            const Line& line = lines[index];
            if (!UsesCalc(line)) {
                ValueCell c = makeCell(line, table.chosenLevel);
                if (!c.present) continue;
                table.constants.push_back(ConstantLine{&line, group, index, c.text, c.shownInCard});
                continue;
            }
            ValueRow row;
            row.line = &line;
            row.group = group;
            row.index = index;
            row.chosen = makeCell(line, table.chosenLevel);
            bool allPresent = row.chosen.present;
            bool anyPresent = row.chosen.present;
            bool same = true;
            for (const int l : table.levels) {
                row.samples.push_back(makeCell(line, l));
                const ValueCell& c = row.samples.back();
                allPresent = allPresent && c.present;
                anyPresent = anyPresent || c.present;
                if (c.text != row.chosen.text) same = false;
            }
            if (!anyPresent) continue;
            if (allPresent && same) {
                table.constants.push_back(ConstantLine{&line, group, index, row.chosen.text, row.chosen.shownInCard});
                continue;
            }
            std::vector<const std::string*> texts;
            const auto addText = [&texts](const ValueCell& c) {
                if (!c.present) return;
                for (const std::string* t : texts)
                    if (*t == c.text) return;
                texts.push_back(&c.text);
            };
            addText(row.chosen);
            for (const ValueCell& c : row.samples) addText(c);
            if (texts.size() >= 2) {
                Parts parts = CommonParts(texts);
                const std::string* model = texts[0];
                // One text in another form (a singular "1 second" among "4.8 seconds", a differently worded level
                // 1 text): leave it out when that gives a longer label. That cell then shows its full text.
                if (texts.size() >= 3) {
                    for (std::size_t drop = 0; drop < texts.size(); ++drop) {
                        std::vector<const std::string*> rest;
                        for (std::size_t k = 0; k < texts.size(); ++k)
                            if (k != drop) rest.push_back(texts[k]);
                        const Parts p2 = CommonParts(rest);
                        if (p2.p + p2.q > parts.p + parts.q) {
                            parts = p2;
                            model = rest[0];
                        }
                    }
                }
                row.prefix = model->substr(0, parts.p);
                row.suffix = model->substr(model->size() - parts.q);
            }
            // A label in front ("Cold Length: 4.8 seconds"): what follows the number is its unit, part of the value.
            row.suffixInValue = !StripColon(row.prefix).empty() && row.suffix.find('\n') == std::string::npos;
            row.label = row.suffixInValue ? StripColon(row.prefix) : MakeLabel(row.prefix, row.suffix);
            for (char& ch : row.label)
                if (ch == '\n') ch = ' ';
            bool valuesOk = true;
            bool numeric = true;
            const auto split = [&row, &valuesOk, &numeric](ValueCell& c) {
                if (!c.present) return;
                if (!SplitCell(row, c)) numeric = false;
                if (c.value.empty()) valuesOk = false;
            };
            split(row.chosen);
            for (ValueCell& c : row.samples) split(c);
            row.splitOk = valuesOk && !row.label.empty();
            row.numericValue = row.splitOk && numeric;
            if (!valuesOk) {
                // Show the full texts rather than an empty value.
                row.prefix.clear();
                row.suffix.clear();
                row.suffixInValue = false;
                row.chosen.value = row.chosen.exactValue = row.chosen.text;
                row.chosen.split = false;
                for (ValueCell& c : row.samples) {
                    c.value = c.exactValue = c.text;
                    c.split = false;
                }
            }
            table.rows.push_back(std::move(row));
        }
    };
    addGroup(skill.general, LineGroup::General);
    addGroup(skill.levelLines, LineGroup::Current);

    // Synergies.
    std::set<const Skill*> listed;
    const std::vector<SynergyInput> inputs = SynergySkills(data, skill);
    const auto inputFor = [&inputs](const Skill* s) -> const SynergyInput* {
        for (const SynergyInput& in : inputs)
            if (in.skill == s) return &in;
        return nullptr;
    };
    const auto pointsOf = [&ctx](const Skill* s) {
        const auto it = ctx.synergyPoints.find(s->id);
        return it == ctx.synergyPoints.end() ? 0 : std::max(0, it->second);
    };
    for (const Line& line : skill.synergyLines) {
        std::optional<std::string> text = calc.FormatLine(rec, line, table.chosenLevel);
        if (line.t == 40) {
            if (text && table.synergyHeader.empty()) table.synergyHeader = *text;
            continue;
        }
        if (line.t == 76 || line.t == 79) {
            SynergyEntry e;
            e.skill = FindByName(data, line.b, skill.cls);
            if (text) e.text = *text;
            if (const SynergyInput* in = e.skill != nullptr ? inputFor(e.skill) : nullptr) {
                e.adjustable = true;
                e.levelLink = in->levelLink;
                e.maxPoints = in->maxPoints;
                e.points = std::min(pointsOf(e.skill), e.maxPoints);
            } else {
                e.maxPoints = data.levels.synergy;
            }
            if (e.skill != nullptr) listed.insert(e.skill);
            if (e.text.empty() && e.skill == nullptr) continue;
            table.synergies.push_back(std::move(e));
            continue;
        }
        if (!text || text->empty()) continue;
        if (line.t == 18 && text->find('\n') != std::string::npos) continue;  // the paired bonus (HeaderFacts)
        table.synergyNotes.push_back(*text);
    }
    for (const SynergyInput& in : inputs) {
        if (listed.count(in.skill) != 0) continue;
        SynergyEntry e;
        e.skill = in.skill;
        e.adjustable = true;
        e.levelLink = in.levelLink;
        e.maxPoints = in.maxPoints;
        e.points = std::min(pointsOf(in.skill), in.maxPoints);
        table.synergies.push_back(std::move(e));
    }
    return table;
}

std::vector<std::vector<ValueCell>> AllLevels(const Data& data, Engine& engine, const Context& ctx,
                                              const ValueTable& table, int from, int to) {
    std::vector<std::vector<ValueCell>> out;
    if (ctx.skill == nullptr || ctx.skill->record == nullptr) return out;
    const Record& rec = *ctx.skill->record;
    from = ClampLevel(data, from);
    to = ClampLevel(data, to);
    Calculator& calc = engine.CalculatorFor(ctx);
    out.reserve(table.rows.size());
    for (const ValueRow& row : table.rows) {
        std::vector<ValueCell> cells;
        if (to >= from) cells.reserve(static_cast<std::size_t>(to - from + 1));
        for (int l = from; l <= to; ++l) {
            LineText t = TextAt(calc, rec, *row.line, l);
            ValueCell c;
            c.level = l;
            c.present = t.present;
            c.shownInCard = t.shown;
            c.text = std::move(t.text);
            if (c.present) {
                SplitCell(row, c);
            }
            cells.push_back(std::move(c));
        }
        out.push_back(std::move(cells));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Item sources
// ---------------------------------------------------------------------------------------------------------------

SourceKind KindOf(const std::string& k) {
    static const std::pair<const char*, SourceKind> kKinds[] = {
        {"Unique", SourceKind::Unique},
        {"Set item", SourceKind::SetItem},
        {"Set bonus", SourceKind::SetBonus},
        {"Runeword", SourceKind::Runeword},
        {"Magic prefix", SourceKind::MagicPrefix},
        {"Magic suffix", SourceKind::MagicSuffix},
        {"Anointment", SourceKind::Anointment},
        {"Vessel of Souls", SourceKind::VesselOfSouls},
        {"Cube recipe", SourceKind::CubeRecipe},
    };
    for (const auto& [text, kind] : kKinds)
        if (k == text) return kind;
    return SourceKind::Other;
}

std::vector<SourceGroup> GroupSources(const Skill& skill) {
    std::vector<SourceGroup> known(static_cast<std::size_t>(SourceKind::Other));
    std::vector<SourceGroup> other;
    for (const Source& src : skill.sources) {
        const SourceKind kind = KindOf(src.k);
        SourceGroup* group = nullptr;
        if (kind != SourceKind::Other) {
            group = &known[static_cast<std::size_t>(kind)];
        } else {
            for (SourceGroup& g : other)
                if (g.kindText == src.k) group = &g;
            if (group == nullptr) {
                other.emplace_back();
                group = &other.back();
            }
        }
        group->kind = kind;
        group->kindText = src.k;
        group->items.push_back(&src);
    }
    std::vector<SourceGroup> out;
    for (std::vector<SourceGroup>* list : {&known, &other})
        for (SourceGroup& g : *list) {
            if (g.items.empty()) continue;
            g.count = g.items.size();
            out.push_back(std::move(g));
        }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------------------------------------------

void IconMap::Parse(const std::string& text) {
    map_.clear();
    badLines_ = 0;
    std::size_t start = 0;
    bool first = true;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // BOM
        first = false;
        const std::string trimmed = TrimSpaces(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            if (end == text.size()) break;
            continue;
        }
        std::string key;
        std::string image;
        const std::size_t tab = line.find('\t');
        const std::size_t arrow = line.find("->");
        if (tab != std::string::npos) {
            key = line.substr(0, tab);
            image = line.substr(tab + 1);
        } else if (arrow != std::string::npos) {
            key = line.substr(0, arrow);
            image = line.substr(arrow + 2);
        }
        key = LowerAscii(TrimSpaces(key));
        image = TrimSpaces(image);
        if (key.empty() || image.empty()) {
            ++badLines_;
        } else {
            map_[key] = image;
        }
        if (end == text.size()) break;
    }
}

bool IconMap::Load(const std::string& path) {
    map_.clear();
    badLines_ = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    Parse(buffer.str());
    return true;
}

const std::string* IconMap::Find(const std::string& skillKey) const {
    const auto it = map_.find(LowerAscii(skillKey));
    return it == map_.end() ? nullptr : &it->second;
}

}  // namespace esrwiki::skills
