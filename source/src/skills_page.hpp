// The Skills tab of the wiki, made from the skills data (skills_data.hpp) as ordinary wiki entries and blocks, so the
// overlay's search, in-page filter, tooltips, highlighting, sorting and controller navigation work on it as on the other
// tabs. One entry per class in the data's order (the item skills, class "other", are one more entry, after an
// introduction block), one item block (card) per skill, grouped by tree (Block::section is the tree's title).
//
// The numbers on a card depend on the picker (skill level, character level, hard points). RefreshSkillsPage writes them
// again without changing the entries or the blocks' places, so pointers to the blocks stay valid.
//
// Platform-neutral (no Win32), so it can be tested on any host. Strings are UTF-8.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "skill_calc.hpp"
#include "skills_data.hpp"
#include "wiki_model.hpp"

namespace esrwiki::skills {

// The wiki page id of the tab. The page is not in wiki-data.txt: InsertSkillsPage adds it.
inline constexpr const char* kSkillsPageId = "skills";
// The page the tab goes right after (and whose tab group it joins).
inline constexpr const char* kSkillsPageAfter = "kill-ledger";

// The words the page uses. Each lookup gets the English text and gives the text to show; a missing lookup, or one that
// gives an empty string, leaves the English as it is.
struct PageText {
    // The overlay's own labels ("Required Level", "Synergies", "At skill level {0}"): the UI strings.
    std::function<std::string(const std::string& english)> ui;
    // The phrases the generator leaves in English in every language: class titles, the item skills' introduction, the
    // notes, the on-striking and on-casting reasons, and the kind, name and value of each source. All of them go through
    // this one lookup (the overlay's SkillsDataText), so their translations can be added in one place.
    std::function<std::string(const std::string& english)> data;

    std::string Ui(const std::string& english) const;
    std::string Phrase(const std::string& english) const;
    // Ui(english) with {0}, {1}, ... replaced by the arguments.
    std::string Format(const std::string& english, const std::vector<std::string>& args) const;
};

// What the cards are worked out for: the picker strip above them.
struct Picker {
    int skillLevel = 20;                // 1..levels.max
    int characterLevel = 110;           // 1..levels.max
    std::optional<int> hardPoints;      // 0..levels.synergy; nullopt = the page's default (hard points follow the level)
    bool operator==(const Picker& other) const {
        return skillLevel == other.skillLevel && characterLevel == other.characterLevel && hardPoints == other.hardPoints;
    }
};

// The data's default levels (levels.skill, levels.character, default hard points).
Picker DefaultPicker(const Data& data);
// `picker` with every value inside its range.
Picker ClampPicker(const Data& data, Picker picker);
// One picker field (0 skill level, 1 character level, 2 hard points) moved by `delta` steps, clamped. Hard points step
// through Default, 0, 1, ... levels.synergy (Default is below 0).
Picker StepPicker(const Data& data, Picker picker, int field, int delta);

// What a card shows on its face besides the skill's name.
struct CardFace {
    std::string tags;           // "Passive · Aura" (empty when none)
    std::string requirement;    // "Required Level: 18" (empty when the data gives none)
    std::string prerequisites;  // "Requires: Fire Bolt, Inferno" (empty when none)
    std::vector<std::string> lines;  // the first lines at the chosen level (each a single text line), at most kFaceLines
};
inline constexpr std::size_t kFaceLines = 3;

// Where each block of the page came from, and its face; both indexed like the page's entries and their blocks. A block
// that is not a skill (the item skills' introduction) has a null skill and an empty face.
struct PageLayout {
    std::vector<std::vector<const Skill*>> skills;
    std::vector<std::vector<CardFace>> faces;
    Picker picker;  // what the lines were made for
};

// The page's entries for `picker`. `layout` is filled to match.
std::vector<Entry> BuildSkillsEntries(const Data& data, Engine& engine, const Picker& picker, const PageText& text,
                                      PageLayout& layout);
// Writes the blocks of `entries` (made by BuildSkillsEntries from the same data) and the faces again for `picker`. The
// entries and blocks stay where they are; each block's search cache is reset. Returns false (and changes nothing) when
// `entries` does not have the shape `layout` describes.
bool RefreshSkillsEntries(const Data& data, Engine& engine, const Picker& picker, const PageText& text,
                          std::vector<Entry>& entries, PageLayout& layout);

// Puts the Skills page into `db`: a page with id kSkillsPageId, titled text.Ui("Skills"), right after the
// kSkillsPageAfter page and in its group (at the end, group "Endgame", when that page is missing). An existing Skills page
// is replaced in place. Returns the page's index.
std::size_t InsertSkillsPage(Database& db, std::vector<Entry> entries, const PageText& text);
// Removes the Skills page from `db`; true when there was one.
bool RemoveSkillsPage(Database& db);
// The index of the Skills page in `db`, or -1.
int FindSkillsPage(const Database& db);

}  // namespace esrwiki::skills
