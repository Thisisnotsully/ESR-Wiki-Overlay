// The Skills tab's portable view model (no UI): the class trees as grids with prerequisite links, a skill's header
// facts (required / max level, mana, passive / aura, hard points only, the paired 20-level bonus), the Values table
// (constant lines, rows that change with the level, every level on request, the synergy inputs), the item sources
// grouped by kind, and the skill icon map (data/skill-icons.txt).
//
// Everything works on any language of skills-data.json: nothing here looks for English words. Labels and values are
// split by comparing the same line's text at several levels (see ValueRow).
//
// Platform-neutral: no Windows headers. Strings are UTF-8.
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "skill_calc.hpp"
#include "skills_data.hpp"

namespace esrwiki::skills {

// ---------------------------------------------------------------------------------------------------------------
// Trees
// ---------------------------------------------------------------------------------------------------------------

constexpr int kTreeColumns = 3;

struct TreeCell {
    const Skill* skill = nullptr;
    int row = 0;      // 0-based grid row
    int col = 0;      // 0-based grid column, < kTreeColumns
    int dataRow = 0;  // Skill::row as given (1-based, 0 = none)
    int dataCol = 0;  // Skill::col as given
    bool moved = false;  // not at its data position (collision or no / bad position)
};

// A prerequisite connector: cells[from] (the prerequisite) -> cells[to] (the skill that needs it).
struct TreeLink {
    std::size_t from = 0;
    std::size_t to = 0;
};

struct TreeLayout {
    std::string classId;
    int tree = 0;
    int rows = 0;  // grid rows (cells use 0..rows-1)
    int cols = 0;  // grid columns used (<= kTreeColumns)
    int maxDataRow = 0;
    int maxDataCol = 0;
    std::vector<TreeCell> cells;  // in data order
    std::vector<TreeLink> links;  // sorted by (to, from)
    // Prerequisites that are not in this tree (another tree / class, or a name that matches no skill): not drawn.
    std::vector<std::pair<const Skill*, std::string>> outsidePrerequisites;
    // Diagnostics (English, not for the UI): collisions and bad positions and how they were resolved.
    std::vector<std::string> issues;

    int IndexOf(const Skill* skill) const;  // -1 when not in the tree
    int CellAt(int row, int col) const;     // index into cells, -1 when empty
};

// The tree's skills on a grid. rows / cols come from the data: by default the in-game rows are kept (row = data row
// - 1, so an empty row stays empty, as in game); dropEmptyRows closes the gaps. A skill with no / a bad position, or
// one that lands on an occupied cell, goes to the first free cell of its row (then of the rows below, adding rows at
// the end when needed), in data order: deterministic, and reported in issues.
TreeLayout BuildTree(const Data& data, const std::string& classId, int treeN, bool dropEmptyRows = false);

// The 'other' class (item-only skills), in data order.
std::vector<const Skill*> ItemSkillList(const Data& data);

// ---------------------------------------------------------------------------------------------------------------
// Header facts
// ---------------------------------------------------------------------------------------------------------------

// The two-skill bonus ("If both X and Y have 20 levels:" + what it does): a multi-line type 18 synergy line.
struct PairedBonus {
    std::string header;  // first line as shown ("If both Multiple Shot and Strafe have 20 levels:")
    std::string text;    // the rest (may contain '\n')
    const Skill* partner = nullptr;  // the other skill named in the header (null when not found)
};

struct HeaderFacts {
    const Skill* skill = nullptr;
    std::string classTitle;
    std::string treeTitle;     // empty for tree 0
    int requiredLevel = 0;     // Skill::requiredLevel; 1 for a tree skill without one (row 1); 0 = none (item skills)
    bool requiredLevelGiven = false;  // the data gives it
    int requiredLevelWithPrerequisites = 0;
    int maxLevel = 0;          // db record mx (0 = none)
    bool hasMana = false;
    bool manaLinear = false;   // mana = manaAtLevel1 + manaPerLevel * (level - 1)
    double manaAtLevel1 = 0;
    double manaPerLevel = 0;
    double manaAtChosen = 0;   // at the context's skill level
    // Mana comes from the skill's mana line (the one whose calc reads usmc, "Mana Cost: %d"); no such line: no mana.
    bool passive = false;
    bool aura = false;
    bool hardPointsOnly = false;  // Skill::hardPoints: "Hard points only: +skills from items do not count."
    std::vector<PairedBonus> pairedBonuses;
};

HeaderFacts BuildHeaderFacts(const Data& data, Engine& engine, const Context& ctx);

// A number as the page writes it: JavaScript String() after rounding to 6 decimals (0.25, 1, 5.75).
std::string FormatNumber(double v);

// ---------------------------------------------------------------------------------------------------------------
// Values table
// ---------------------------------------------------------------------------------------------------------------

// Skill levels above this are marked as beyond reach (the '*' on 110). Assumption: 99 is the highest skill level
// a character can reach with hard points and +skills; the page still evaluates up to Levels::max.
constexpr int kReachableSkillLevel = 99;

// {1, 20, 30, 50, levels.max}
std::vector<int> DefaultSampleLevels(const Data& data);
inline bool BeyondReach(int level) { return level > kReachableSkillLevel; }

enum class LineGroup { General, Current };

struct ValueCell {
    int level = 0;
    bool present = false;      // false: the line is hidden at this level and its text could not be made
    bool shownInCard = false;  // Engine::Card shows this text at this level (false for a "0%" line it hides)
    std::string text;          // the full formatted line (fall back to this)
    std::string value;         // text without the row's label parts, decimals rounded to one place ("5.8")
    std::string exactValue;    // the same, unrounded ("5.75")
    bool split = false;        // value is text minus the label; false: value is the full text (another form)
};

// A line whose text changes between the sampled levels. The label is what all its texts share: the longest common
// prefix and suffix, moved outwards to UTF-8 character boundaries and so that a number (with its sign, decimals,
// range and '%') is never cut; value = the differing middle. label is prefix + suffix trimmed, without a trailing
// ':' ("Attack Rating: +10%" -> label "Attack Rating", value "+10%"). When leaving one text out gives a longer
// label (a singular "1 second" among "4.8 seconds"), that is used, and the left-out cells show their full text.
// With a label in front, the common suffix is the number's unit and stays in the value ("Cold Length: 4.8
// seconds" -> "Cold Length" / "4.8 seconds"); with none, the suffix is the label ("125% Weapon Damage").
struct ValueRow {
    const Line* line = nullptr;
    LineGroup group = LineGroup::Current;
    std::size_t index = 0;  // index in Skill::general / Skill::levelLines
    std::string label;
    std::string prefix;
    std::string suffix;
    bool suffixInValue = false;  // label is the prefix; the suffix (a unit: " seconds") stays in the value
    bool splitOk = false;      // label and every present value non-empty; otherwise show the full texts
    bool numericValue = false; // every value is only a number / range / percentage (no words)
    ValueCell chosen;
    std::vector<ValueCell> samples;  // one per ValueTable::levels
};

// A line with the same text at every sampled level (and at the chosen one): the "AT LEVEL N" list.
struct ConstantLine {
    const Line* line = nullptr;
    LineGroup group = LineGroup::General;
    std::size_t index = 0;
    std::string text;
    bool shownInCard = false;
};

// One "RECEIVES BONUSES FROM" row.
struct SynergyEntry {
    const Skill* skill = nullptr;  // null when the line names no known skill
    std::string text;              // the synergy line at the context (empty for an input with no line)
    int points = 0;                // current points from Context::synergyPoints
    int maxPoints = 0;             // levels.synergy (hard points) or levels.max (level link)
    bool adjustable = false;       // the skill is in Skill::synergies / levelLinks (an input changes the numbers)
    bool levelLink = false;        // counts levels with +skills, not hard points
};

struct ValueTable {
    const Skill* skill = nullptr;
    int chosenLevel = 0;
    std::vector<int> levels;        // the sampled levels
    std::vector<ConstantLine> constants;  // data order: general, then current
    std::vector<ValueRow> rows;           // data order: general, then current
    std::string synergyHeader;      // the type 40 line ("Multiple Shot Receives Bonuses From:"), may be empty
    std::vector<SynergyEntry> synergies;
    std::vector<std::string> synergyNotes;  // other synergy lines (not the paired bonus)
};

ValueTable BuildValueTable(const Data& data, Engine& engine, const Context& ctx, const std::vector<int>& levels);
inline ValueTable BuildValueTable(const Data& data, Engine& engine, const Context& ctx) {
    return BuildValueTable(data, engine, ctx, DefaultSampleLevels(data));
}

// "All levels": every row of the table at levels from..to (clamped to 1..levels.max), values split with the row's
// label parts. result[i] belongs to table.rows[i], with one cell per level.
std::vector<std::vector<ValueCell>> AllLevels(const Data& data, Engine& engine, const Context& ctx,
                                              const ValueTable& table, int from, int to);

// The synergy inputs: Skill::synergies (hard points, 0..levels.synergy) then Skill::levelLinks (0..levels.max).
struct SynergyInput {
    const Skill* skill = nullptr;
    int maxPoints = 0;
    bool levelLink = false;
};
std::vector<SynergyInput> SynergySkills(const Data& data, const Skill& skill);
// Every synergy input at its maximum, as Context::synergyPoints.
std::map<std::string, int> MaxAll(const Data& data, const Skill& skill);

// ---------------------------------------------------------------------------------------------------------------
// Item sources
// ---------------------------------------------------------------------------------------------------------------

enum class SourceKind {
    Unique,
    SetItem,
    SetBonus,
    Runeword,
    MagicPrefix,
    MagicSuffix,
    Anointment,
    VesselOfSouls,
    CubeRecipe,
    Other,
};

// Source::k (the same English word in every language file) -> kind.
SourceKind KindOf(const std::string& k);

struct SourceGroup {
    SourceKind kind = SourceKind::Other;
    std::string kindText;  // Source::k as in the data
    std::vector<const Source*> items;  // data order
    std::size_t count = 0;             // items.size()
};

// Groups in the order of SourceKind (Other kinds after, in order of first appearance); empty groups left out.
std::vector<SourceGroup> GroupSources(const Skill& skill);

// ---------------------------------------------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------------------------------------------

// data/skill-icons.txt: one "skill key<TAB>image name" per line ("key -> image" also accepted); blank lines and
// lines starting with '#' skipped. Keys are matched ignoring ASCII case.
class IconMap {
public:
    void Parse(const std::string& text);
    bool Load(const std::string& path);  // false when the file cannot be read (the map is then left empty)
    const std::string* Find(const std::string& skillKey) const;
    std::size_t Size() const { return map_.size(); }
    int BadLines() const { return badLines_; }

private:
    std::unordered_map<std::string, std::string> map_;
    int badLines_ = 0;
};

}  // namespace esrwiki::skills
