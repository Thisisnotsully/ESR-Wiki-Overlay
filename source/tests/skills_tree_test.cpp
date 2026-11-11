// Checks the Skills tab's view model (skills_tree.hpp): the tree grids and links, header facts, the Values table
// (against the reference numbers and against Engine::Card), item source groups and the icon map.
//   skills_tree_test <data folder>   (the folder with skills-data.json, locales/ and skill-icons.txt)
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "skills_tree.hpp"

namespace {

using namespace esrwiki::skills;

int failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        if (failures <= 60) std::printf("FAIL: %s\n", what.c_str());
    }
}

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

bool Load(const std::string& dir, const std::string& lang, Data& data) {
    const std::string path = lang == "enUS" ? dir + "/skills-data.json" : dir + "/locales/" + lang + "/skills-data.json";
    std::string text;
    if (!ReadFile(path, text)) {
        Check(false, "read " + path);
        return false;
    }
    std::string error;
    if (!Parse(text, data, error)) {
        Check(false, "parse " + path + ": " + error);
        return false;
    }
    return true;
}

const ValueRow* RowByLabel(const ValueTable& t, const std::string& label) {
    for (const ValueRow& r : t.rows)
        if (r.label == label) return &r;
    return nullptr;
}

bool HasConstant(const ValueTable& t, const std::string& text) {
    for (const ConstantLine& c : t.constants)
        if (c.text == text) return true;
    return false;
}

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : " / ") + x;
    return s;
}

void CheckSamples(const ValueRow* row, const std::string& label, const std::vector<std::string>& expected) {
    Check(row != nullptr, "Multiple Shot row " + label);
    if (row == nullptr) return;
    std::vector<std::string> got;
    for (const ValueCell& c : row->samples) got.push_back(c.value);
    Check(got == expected, "Multiple Shot " + label + ": " + Join(got) + " want " + Join(expected));
}

// The table's texts at sample k (or the chosen cell when k < 0) that the card shows, in data order.
std::vector<std::string> TableTexts(const ValueTable& t, LineGroup group, int k) {
    std::map<std::size_t, std::string> byIndex;
    for (const ConstantLine& c : t.constants)
        if (c.group == group && c.shownInCard) byIndex[c.index] = c.text;
    for (const ValueRow& r : t.rows) {
        if (r.group != group) continue;
        const ValueCell& c = k < 0 ? r.chosen : r.samples[static_cast<std::size_t>(k)];
        if (c.present && c.shownInCard) byIndex[r.index] = c.text;
    }
    std::vector<std::string> out;
    for (const auto& [i, text] : byIndex) out.push_back(text);
    return out;
}

std::vector<std::string> CardTexts(const std::vector<CardLine>& lines) {
    std::vector<std::string> out;
    for (const CardLine& l : lines) out.push_back(l.text);
    return out;
}

struct SplitStats {
    int rows = 0;
    int emptyValue = 0;
    int notSplit = 0;  // splitOk false (full texts shown)
    int nonNumeric = 0;
};

void CheckTrees(const Data& data, const std::string& lang, bool print) {
    for (const Class& c : data.classes) {
        if (c.id == "other") {
            const std::vector<const Skill*> items = ItemSkillList(data);
            Check(!items.empty(), lang + " other: item skills");
            if (print) std::printf("  other: %zu item skills\n", items.size());
            continue;
        }
        std::map<const Skill*, int> placed;
        int maxRows = 0;
        int maxCols = 0;
        int maxDataCol = 0;
        std::size_t issues = 0;
        std::size_t outside = 0;
        std::size_t links = 0;
        for (const Tree& tree : c.trees) {
            const TreeLayout layout = BuildTree(data, c.id, tree.n);
            const TreeLayout compact = BuildTree(data, c.id, tree.n, true);
            Check(compact.cells.size() == layout.cells.size(), lang + " compact cells " + tree.id);
            Check(compact.rows <= layout.rows, lang + " compact rows " + tree.id);
            std::set<std::pair<int, int>> cells;
            for (const TreeCell& cell : layout.cells) {
                ++placed[cell.skill];
                Check(cell.row >= 0 && cell.row < layout.rows && cell.col >= 0 && cell.col < kTreeColumns,
                      lang + " cell in grid: " + cell.skill->key);
                Check(cells.insert({cell.row, cell.col}).second, lang + " two skills in one cell: " + cell.skill->key);
                Check(layout.CellAt(cell.row, cell.col) == layout.IndexOf(cell.skill), lang + " CellAt " + cell.skill->key);
            }
            for (const TreeLink& link : layout.links)
                Check(link.from < layout.cells.size() && link.to < layout.cells.size() && link.from != link.to,
                      lang + " link in tree " + tree.id);
            for (const auto& issue : layout.issues) {
                if (print) std::printf("  issue: %s\n", issue.c_str());
            }
            maxRows = std::max(maxRows, layout.rows);
            maxCols = std::max(maxCols, layout.cols);
            maxDataCol = std::max(maxDataCol, layout.maxDataCol);
            Check(layout.cols <= kTreeColumns, lang + " columns " + tree.id);
            issues += layout.issues.size();
            outside += layout.outsidePrerequisites.size();
            links += layout.links.size();
        }
        int unplaced = 0;
        for (const Skill* s : data.SkillsOfClass(c.id)) {
            const auto it = placed.find(s);
            if (it == placed.end()) {
                ++unplaced;
                Check(s->tree == 0, lang + " class skill not placed: " + s->key);
            } else {
                Check(it->second == 1, lang + " placed more than once: " + s->key);
            }
        }
        if (print)
            std::printf("  %s: %zu trees, max rows %d, max cols %d (data max col %d), links %zu, outside prereqs %zu, "
                        "collisions %zu, tree-0 skills %d\n",
                        c.id.c_str(), c.trees.size(), maxRows, maxCols, maxDataCol, links, outside, issues, unplaced);
    }
}

void CrossCheck(const Data& data, const std::string& lang, SplitStats& stats) {
    Engine engine(data);
    for (const Skill& skill : data.skills) {
        for (int pass = 0; pass < 2; ++pass) {
            Context ctx;
            ctx.skill = &skill;
            ctx.skillLevel = data.levels.skill;
            ctx.characterLevel = data.levels.character;
            if (pass == 1) {
                ctx.synergyPoints = MaxAll(data, skill);
                if (ctx.synergyPoints.empty()) continue;
            }
            const ValueTable t = BuildValueTable(data, engine, ctx);
            for (int k = -1; k < static_cast<int>(t.levels.size()); ++k) {
                Context at = ctx;
                at.skillLevel = k < 0 ? t.chosenLevel : t.levels[static_cast<std::size_t>(k)];
                const CardLines card = engine.Card(at);
                Check(TableTexts(t, LineGroup::General, k) == CardTexts(card.general),
                      lang + " general lines " + skill.key + " at " + std::to_string(at.skillLevel));
                Check(TableTexts(t, LineGroup::Current, k) == CardTexts(card.current),
                      lang + " current lines " + skill.key + " at " + std::to_string(at.skillLevel));
            }
            if (pass == 1) continue;
            for (const ValueRow& r : t.rows) {
                ++stats.rows;
                if (!r.splitOk) ++stats.notSplit;
                if (r.splitOk && !r.numericValue) ++stats.nonNumeric;
                bool empty = false;
                for (const ValueCell& c : r.samples)
                    if (c.present && c.value.empty()) empty = true;
                if (r.chosen.present && r.chosen.value.empty()) empty = true;
                if (empty) ++stats.emptyValue;
                Check(!empty, lang + " empty value: " + skill.key + " " + r.label);
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: skills_tree_test <data folder>\n");
        return 2;
    }
    const std::string dir = argv[1];

    // --- Trees in enUS and three other languages.
    for (const char* lang : {"enUS", "deDE", "jaJP", "frFR"}) {
        Data data;
        if (!Load(dir, lang, data)) continue;
        const bool print = std::string(lang) == "enUS";
        if (print) std::printf("trees (%s):\n", lang);
        CheckTrees(data, lang, print);
    }

    Data en;
    if (!Load(dir, "enUS", en)) return 1;
    Engine engine(en);
    const Skill* ms = en.FindByKey("multiple shot");
    const Skill* ma = en.FindByKey("magic arrow");
    Check(ms != nullptr && ma != nullptr, "Multiple Shot / Magic Arrow");
    if (ms == nullptr || ma == nullptr) return 1;

    // --- Header facts.
    {
        Context ctx;
        ctx.skill = ms;
        ctx.skillLevel = 20;
        const HeaderFacts h = BuildHeaderFacts(en, engine, ctx);
        Check(h.requiredLevel == 1, "MS req " + std::to_string(h.requiredLevel));
        Check(h.maxLevel == 20, "MS max " + std::to_string(h.maxLevel));
        Check(h.hasMana && h.manaLinear, "MS mana linear");
        Check(FormatNumber(h.manaAtLevel1) == "1" && FormatNumber(h.manaPerLevel) == "0.25",
              "MS mana " + FormatNumber(h.manaAtLevel1) + " +" + FormatNumber(h.manaPerLevel));
        Check(FormatNumber(h.manaAtChosen) == "5.75", "MS mana at 20 " + FormatNumber(h.manaAtChosen));
        Check(!h.passive && !h.aura && !h.hardPointsOnly, "MS flags");
        Check(h.classTitle == "Amazon", "MS class " + h.classTitle);
        Check(!h.treeTitle.empty(), "MS tree title");
        Check(h.pairedBonuses.size() == 1, "MS paired bonus");
        if (!h.pairedBonuses.empty()) {
            Check(h.pairedBonuses[0].header == "If both Multiple Shot and Strafe have 20 levels:",
                  "MS bonus header " + h.pairedBonuses[0].header);
            Check(h.pairedBonuses[0].text == "Every 3 Attacks, Multiple Shot deals 75% extra weapon damage",
                  "MS bonus text " + h.pairedBonuses[0].text);
            Check(h.pairedBonuses[0].partner == en.FindByKey("strafe"), "MS bonus partner");
        }
        int passives = 0;
        int hardOnly = 0;
        int paired = 0;
        int linear = 0;
        int mana = 0;
        for (const Skill& s : en.skills) {
            Context c2;
            c2.skill = &s;
            const HeaderFacts f = BuildHeaderFacts(en, engine, c2);
            if (s.passive) {
                ++passives;
                Check(!f.hasMana, "passive with mana: " + s.key);
            }
            if (f.hardPointsOnly) ++hardOnly;
            if (!f.pairedBonuses.empty()) {
                ++paired;
                Check(f.pairedBonuses[0].partner != nullptr, "paired bonus partner: " + s.key);
            }
            if (f.hasMana) ++mana;
            if (f.manaLinear) ++linear;
        }
        const Skill* dodge = en.FindByKey("dodge");
        if (dodge != nullptr) {
            Context c3;
            c3.skill = dodge;
            Check(BuildHeaderFacts(en, engine, c3).hardPointsOnly, "Dodge hard points only");
        }
        std::printf("header: %d passives (none with mana), %d with mana (%d linear), %d hard points only, %d paired bonuses\n",
                    passives, mana, linear, hardOnly, paired);
    }

    // --- Values table: the reference numbers.
    {
        Context ctx;
        ctx.skill = ms;
        ctx.skillLevel = 20;
        ctx.characterLevel = 110;
        const ValueTable t = BuildValueTable(en, engine, ctx);
        Check(t.levels == std::vector<int>({1, 20, 30, 50, 110}), "sample levels");
        Check(BeyondReach(110) && !BeyondReach(50), "beyond reach");
        // The reference page shows Attack Rating +10/+200/+300/+500/+1100% and Missiles 16 at level 20. This engine
        // (byte-identical to the esr-docs skill-calc.js) gives toht = 10 at every level (db th [10, 0]) and 17
        // missiles at level 20, so those two follow the engine; Damage and Mana Cost match the reference.
        CheckSamples(RowByLabel(t, "Damage"), "Damage", {"+10%", "+200%", "+300%", "+500%", "+1100%"});
        CheckSamples(RowByLabel(t, "Missiles"), "Missiles", {"3", "17", "18", "19", "20"});
        CheckSamples(RowByLabel(t, "Mana Cost"), "Mana Cost", {"1", "5.8", "8.3", "13.3", "28.3"});
        const ValueRow* mana = RowByLabel(t, "Mana Cost");
        Check(mana != nullptr && mana->chosen.exactValue == "5.75" && mana->chosen.text == "Mana Cost: 5.75", "mana exact");
        const ValueRow* ar = RowByLabel(t, "Damage");
        Check(ar != nullptr && ar->chosen.value == "+200%" && ar->chosen.text == "Damage: +200%", "Damage chosen");
        Check(HasConstant(t, "125% Weapon Damage"), "constant 125% Weapon Damage");
        Check(HasConstant(t, "0% Physical Damage Converted to Magic Damage"), "constant 0% conversion");
        Check(HasConstant(t, "Attack Rating: +10%"), "constant Attack Rating (engine)");
        Check(t.constants.size() == 3 && t.rows.size() == 3,
              "MS table shape " + std::to_string(t.constants.size()) + "/" + std::to_string(t.rows.size()));
        Check(t.synergyHeader == "Multiple Shot Receives Bonuses From:", "synergy header " + t.synergyHeader);
        Check(t.synergies.size() == 3, "MS synergies " + std::to_string(t.synergies.size()));
        int adjustable = 0;
        for (const SynergyEntry& e : t.synergies) adjustable += e.adjustable ? 1 : 0;
        Check(adjustable == 2, "MS adjustable synergies");
        const std::vector<SynergyInput> inputs = SynergySkills(en, *ms);
        Check(inputs.size() == 2 && inputs[0].maxPoints == 20, "MS synergy inputs");
        Check(MaxAll(en, *ms).size() == 2, "MaxAll");

        Context syn = ctx;
        syn.synergyPoints[ma->id] = 20;
        const ValueTable t2 = BuildValueTable(en, engine, syn);
        Check(HasConstant(t2, "100% Physical Damage Converted to Magic Damage"), "Magic Arrow 20: 100% conversion");
        bool shownPts = false;
        for (const SynergyEntry& e : t2.synergies)
            if (e.skill == ma && e.points == 20 && e.text.find("Magic Arrow") != std::string::npos) shownPts = true;
        Check(shownPts, "Magic Arrow synergy entry with 20 points");

        const auto all = AllLevels(en, engine, ctx, t, 1, en.levels.max);
        Check(all.size() == t.rows.size() && !all.empty() && all[0].size() == 110, "AllLevels shape");
        if (ar != nullptr && !all.empty()) {
            const auto& arAll = all[static_cast<std::size_t>(ar - t.rows.data())];
            Check(arAll[0].value == "+10%" && arAll[19].value == "+200%" && arAll[109].value == "+1100%", "AllLevels Damage");
        }
    }

    // --- Values table equals Engine::Card at every sampled level, all skills, three languages.
    for (const char* lang : {"enUS", "deDE", "jaJP"}) {
        Data data;
        if (!Load(dir, lang, data)) continue;
        SplitStats stats;
        CrossCheck(data, lang, stats);
        std::printf("split %s: %d varying rows, %d empty values, %d not split (full text), %d with words in the value\n",
                    lang, stats.rows, stats.emptyValue, stats.notSplit, stats.nonNumeric);
    }

    // --- Timing: table + all levels, worst skill.
    {
        double worst = 0;
        std::string worstKey;
        for (const Skill& s : en.skills) {
            Engine fresh(en);
            Context ctx;
            ctx.skill = &s;
            ctx.synergyPoints = MaxAll(en, s);
            const auto t0 = std::chrono::steady_clock::now();
            const ValueTable t = BuildValueTable(en, fresh, ctx);
            const auto all = AllLevels(en, fresh, ctx, t, 1, en.levels.max);
            const double ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            Check(all.size() == t.rows.size(), "AllLevels rows: " + s.key);
            if (ms_ > worst) {
                worst = ms_;
                worstKey = s.key;
            }
        }
        std::printf("timing: worst %.2f ms (%s)\n", worst, worstKey.c_str());
        Check(worst < 30, "table + all levels under 30 ms");
    }

    // --- Item sources.
    {
        const std::vector<SourceGroup> g = GroupSources(*ms);
        std::string got;
        for (const SourceGroup& x : g) got += x.kindText + ":" + std::to_string(x.count) + " ";
        Check(got == "Unique:6 Magic suffix:1 Anointment:3 Vessel of Souls:1 ", "MS sources " + got);
        std::size_t unknown = 0;
        for (const Skill& s : en.skills) {
            std::size_t total = 0;
            int last = -1;
            for (const SourceGroup& x : GroupSources(s)) {
                total += x.count;
                if (x.kind == SourceKind::Other) ++unknown;
                Check(static_cast<int>(x.kind) >= last, "source group order: " + s.key);
                last = static_cast<int>(x.kind);
            }
            Check(total == s.sources.size(), "source count: " + s.key);
        }
        Check(unknown == 0, "unknown source kinds: " + std::to_string(unknown));
    }

    // --- Icons.
    {
        IconMap icons;
        icons.Parse("# comment\r\nMultiple Shot\tskill_ama_1\r\n\nmagic arrow -> skill_ama_2\nbroken line\n");
        Check(icons.Size() == 2 && icons.BadLines() == 1, "icon parse");
        Check(icons.Find("multiple shot") != nullptr && *icons.Find("multiple shot") == "skill_ama_1", "icon find");
        Check(icons.Find("Magic Arrow") != nullptr && *icons.Find("Magic Arrow") == "skill_ama_2", "icon arrow");
        IconMap none;
        Check(!none.Load(dir + "/no-such-file.txt") && none.Size() == 0, "icon file absent");
        IconMap real;
        if (real.Load(dir + "/skill-icons.txt")) {
            int found = 0;
            for (const Skill& s : en.skills) found += real.Find(s.key) != nullptr ? 1 : 0;
            std::printf("icons: %zu entries, %d bad lines, %d of %zu skills have one\n", real.Size(), real.BadLines(),
                        found, en.skills.size());
        } else {
            std::printf("icons: skill-icons.txt not present\n");
        }
    }

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("skills_tree: all checks passed\n");
    return 0;
}
