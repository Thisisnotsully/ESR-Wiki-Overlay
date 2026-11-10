// Checks the Skills tab's entries (skills_page.hpp): their shape, a card's fields and lines, the refresh for the picker,
// where the page goes in the wiki data, and that the normal search and filter find the skills.
//   skills_page_test <data folder>   (the folder with skills-data.json, wiki-data.txt and locales/)
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "search_index.hpp"
#include "skills_page.hpp"
#include "wiki_model.hpp"

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
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

bool HasLine(const esrwiki::Block& block, const std::string& text) {
    for (const esrwiki::Line& line : block.lines)
        if (line.text == text) return true;
    return false;
}

const esrwiki::Field* FindField(const esrwiki::Block& block, const std::string& label) {
    for (const esrwiki::Field& field : block.fields)
        if (field.label == label) return &field;
    return nullptr;
}

const esrwiki::Block* FindBlock(const std::vector<esrwiki::Entry>& entries, const std::string& title) {
    for (const esrwiki::Entry& entry : entries)
        for (const esrwiki::Block& block : entry.blocks)
            if (block.title == title) return &block;
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace esrwiki;
    if (argc < 2) {
        std::printf("usage: skills_page_test <data folder>\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::string json, error;
    if (!ReadFile(dir + "/skills-data.json", json)) {
        std::printf("cannot read skills-data.json\n");
        return 2;
    }
    skills::Data data;
    if (!skills::Parse(json, data, error)) {
        std::printf("skills-data.json: %s\n", error.c_str());
        return 2;
    }

    // The labels: a fake translation marks every UI label and every data phrase, so the test sees which lookup each went
    // through.
    skills::PageText text;
    text.ui = [](const std::string& english) { return english == "Skills" ? std::string("Skills!") : std::string(); };
    text.data = [](const std::string& english) { return english == "Item Skills" ? std::string("[Item Skills]") : std::string(); };
    skills::PageText marked;
    marked.ui = [](const std::string& english) { return "<" + english + ">"; };
    marked.data = [](const std::string& english) { return "[" + english + "]"; };

    skills::Engine engine(data);
    skills::PageLayout layout;
    const skills::Picker picker = skills::DefaultPicker(data);
    Check(picker.skillLevel == 20 && picker.characterLevel == 110 && !picker.hardPoints, "default picker");

    const auto started = std::chrono::steady_clock::now();
    std::vector<Entry> entries = skills::BuildSkillsEntries(data, engine, picker, text, layout);
    const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    // One entry per class, in the data's order; the item skills last, with their introduction first.
    Check(entries.size() == data.classes.size(), "one entry per class");
    std::size_t cards = 0, guides = 0;
    for (const Entry& entry : entries)
        for (const Block& block : entry.blocks) (block.item ? cards : guides) += 1;
    Check(cards == data.skills.size(), "one card per skill (" + std::to_string(cards) + ")");
    Check(guides == 1, "one introduction block");
    Check(entries.front().title == "Amazon", "first entry Amazon: " + entries.front().title);
    Check(entries.back().title == "[Item Skills]", "the item skills' title goes through the data lookup: " + entries.back().title);
    Check(!entries.back().blocks.empty() && !entries.back().blocks.front().item, "the item skills start with the introduction");
    Check(layout.skills.size() == entries.size() && layout.faces.size() == entries.size(), "layout per entry");
    for (std::size_t e = 0; e < entries.size() && e < layout.skills.size(); ++e)
        Check(layout.skills[e].size() == entries[e].blocks.size() && layout.faces[e].size() == entries[e].blocks.size(),
              "layout per block of " + entries[e].title);

    // Fire Ball: tree, fields, lines at the chosen level, synergies and sources.
    const Block* fireBall = FindBlock(entries, "Fire Ball");
    Check(fireBall != nullptr, "Fire Ball card");
    if (fireBall != nullptr) {
        Check(fireBall->section == "Fire Skills", "Fire Ball section: " + fireBall->section);
        Check(fireBall->subtitle == "Sorceress \xC2\xB7 Fire Skills", "Fire Ball subtitle: " + fireBall->subtitle);
        const Field* required = FindField(*fireBall, "Required Level");
        Check(required != nullptr && required->value == "12", "Fire Ball required level");
        const Field* requires_ = FindField(*fireBall, "Requires");
        Check(requires_ != nullptr && requires_->value == "Fire Bolt", "Fire Ball prerequisites");
        const Field* striking = FindField(*fireBall, "Procs on striking");
        Check(striking != nullptr && striking->value.rfind("No. It deals no weapon damage", 0) == 0, "Fire Ball on striking");
        Check(HasLine(*fireBall, "At skill level 20"), "Fire Ball level heading");
        Check(HasLine(*fireBall, "Synergies"), "Fire Ball synergies heading");
        Check(HasLine(*fireBall, "Granted by 9 items and recipes"), "Fire Ball sources heading");
        Check(HasLine(*fireBall, "Unique \xC2\xB7 Ember of Creation \xC2\xB7 +1"), "Fire Ball source row");
        bool vessel = false;
        for (const Line& line : fireBall->lines)
            if (line.text.rfind("Vessel of Souls \xC2\xB7 2% Chance", 0) == 0 && line.text.find(" \xC3\x97" "2") != std::string::npos) vessel = true;
        Check(vessel, "Fire Ball Vessel of Souls row with its count");
        Check(BlockMatches(*fireBall, "ember of creation"), "a source's item name finds the card");
        Check(BlockMatches(*fireBall, "sorceress"), "the class finds the card");
        Check(BlockMatches(*fireBall, "explosive sphere"), "the description finds the card");
    }
    // The face of Fire Ball: its requirement, prerequisites and the first lines at the level.
    for (std::size_t e = 0; e < entries.size(); ++e)
        for (std::size_t b = 0; b < entries[e].blocks.size(); ++b)
            if (entries[e].blocks[b].title == "Fire Ball") {
                const skills::CardFace& face = layout.faces[e][b];
                Check(face.requirement == "Required Level: 12", "Fire Ball face requirement: " + face.requirement);
                Check(face.prerequisites == "Requires: Fire Bolt", "Fire Ball face prerequisites: " + face.prerequisites);
                Check(!face.lines.empty() && face.lines.size() <= skills::kFaceLines, "Fire Ball face lines");
                Check(!face.lines.empty() && face.lines.front().find("Radius") == 0, "Fire Ball face starts with its radius");
            }

    // Every label goes through one of the two lookups.
    {
        skills::PageLayout markedLayout;
        skills::Engine markedEngine(data);
        const std::vector<Entry> markedEntries = skills::BuildSkillsEntries(data, markedEngine, picker, marked, markedLayout);
        const Block* block = FindBlock(markedEntries, "Fire Ball");
        Check(block != nullptr && FindField(*block, "<Required Level>") != nullptr, "the required level label is a UI string");
        Check(block != nullptr && HasLine(*block, "<At skill level 20>"), "the level heading is a UI string");
        bool sourceRow = false, reason = false;
        if (block != nullptr) {
            for (const Line& line : block->lines)
                if (line.text == "[Unique] \xC2\xB7 [Ember of Creation] \xC2\xB7 [+1]") sourceRow = true;
            const Field* striking = FindField(*block, "<Procs on striking>");
            reason = striking != nullptr && striking->value == "<<No>. [It deals no weapon damage, and spells don\xE2\x80\x99t count as strikes.]>";  // joined through the marked "{0}. {1}"
        }
        Check(sourceRow, "the source's kind, name and value go through the data lookup");
        Check(reason, "the on-striking reason goes through the data lookup");
        const Block* aura = FindBlock(markedEntries, "Might");
        Check(aura != nullptr && aura->subtitle.find("<Aura>") != std::string::npos, "an aura is tagged: " + (aura ? aura->subtitle : std::string()));
        Check(markedEntries.front().title == "[Amazon]", "class titles go through the data lookup");
    }

    // The picker: the numbers change, the blocks stay where they are.
    {
        const Block* before = fireBall;
        std::vector<std::string> oldLines;
        if (fireBall != nullptr)
            for (const Line& line : fireBall->lines) oldLines.push_back(line.text);
        if (fireBall != nullptr) (void)BlockSearchText(*fireBall);  // fills the search cache
        skills::Picker next = picker;
        next.skillLevel = 1;
        next.characterLevel = 1;
        const auto refreshStart = std::chrono::steady_clock::now();
        Check(skills::RefreshSkillsEntries(data, engine, next, text, entries, layout), "refresh");
        const double refreshMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - refreshStart).count();
        std::printf("build %.1f ms, refresh %.1f ms\n", buildMs, refreshMs);
        const Block* after = FindBlock(entries, "Fire Ball");
        Check(after == before, "the block keeps its place");
        Check(after != nullptr && HasLine(*after, "At skill level 1"), "the heading follows the level");
        std::vector<std::string> newLines;
        if (after != nullptr)
            for (const Line& line : after->lines) newLines.push_back(line.text);
        Check(newLines != oldLines, "the lines follow the level");
        Check(after != nullptr && !after->foldedReady, "the search cache is reset");
        Check(layout.picker.skillLevel == 1, "the layout remembers the picker");
        std::vector<Entry> wrongShape(entries.begin(), entries.end() - 1);
        skills::PageLayout copy = layout;
        Check(!skills::RefreshSkillsEntries(data, engine, picker, text, wrongShape, copy), "a refresh of another shape is refused");
    }

    // The picker's steps and limits.
    {
        skills::Picker p = picker;
        p = skills::StepPicker(data, p, 0, 500);
        Check(p.skillLevel == data.levels.max, "skill level stops at the top");
        p = skills::StepPicker(data, p, 1, -500);
        Check(p.characterLevel == 1, "character level stops at 1");
        p = skills::StepPicker(data, p, 2, 1);
        Check(p.hardPoints && *p.hardPoints == 0, "hard points: Default, then 0");
        p = skills::StepPicker(data, p, 2, 100);
        Check(p.hardPoints && *p.hardPoints == data.levels.synergy, "hard points stop at the synergy cap");
        p = skills::StepPicker(data, p, 2, -100);
        Check(!p.hardPoints, "hard points: back down to Default");
    }

    // The page in the wiki data: right after the Kill Ledger, in its group; replaced in place; removed.
    std::string wiki;
    Database db;
    if (ReadFile(dir + "/wiki-data.txt", wiki) && ParseDatabase(wiki, db, error)) {
        const std::size_t pagesBefore = db.pages.size();
        const std::size_t at = skills::InsertSkillsPage(db, entries, text);
        Check(db.pages.size() == pagesBefore + 1, "the page is added");
        Check(at > 0 && db.pages[at - 1].id == "kill-ledger", "the page follows the Kill Ledger");
        Check(db.pages[at].title == "Skills!" && db.pages[at].group == db.pages[at - 1].group, "the page's title and group");
        Check(skills::FindSkillsPage(db) == static_cast<int>(at), "FindSkillsPage");
        Check(skills::InsertSkillsPage(db, entries, text) == at && db.pages.size() == pagesBefore + 1, "a second insert replaces it");

        // The normal filter and search find the skills.
        const Page& page = db.pages[at];
        const std::vector<std::size_t> found = FilterEntries(page, "fire ball");
        bool sorceress = false;
        for (const std::size_t e : found) sorceress = sorceress || page.entries[e].title == "Sorceress";
        Check(sorceress, "the in-page filter finds Fire Ball under Sorceress");
        Check(found.size() < page.entries.size(), "the in-page filter leaves out the classes without it");
        Check(!FilterEntries(page, "ember of creation").empty(), "the in-page filter finds a source item");
        search::Index index;
        index.Build(db);
        const std::vector<search::Hit> hits = index.Query(db, "Fire Ball", at, 8);
        bool card = false;
        for (const search::Hit& hit : hits)
            if (hit.kind == search::HitKind::Card && hit.page == static_cast<int>(at) && hit.label == "Fire Ball" && hit.entry >= 0 &&
                hit.block >= 0 && page.entries[static_cast<std::size_t>(hit.entry)].blocks[static_cast<std::size_t>(hit.block)].title == "Fire Ball")
                card = true;
        Check(card, "the search suggestions list Fire Ball as a card of the Skills page");

        Check(skills::RemoveSkillsPage(db) && db.pages.size() == pagesBefore && skills::FindSkillsPage(db) < 0, "the page is removed");
        Check(!skills::RemoveSkillsPage(db), "nothing to remove the second time");
    } else {
        Check(false, "wiki-data.txt: " + error);
    }

    // A translated file: the same shape (the keys are the same in every language).
    std::string localized;
    if (ReadFile(dir + "/locales/deDE/skills-data.json", localized)) {
        skills::Data de;
        if (skills::Parse(localized, de, error)) {
            skills::Engine deEngine(de);
            skills::PageLayout deLayout;
            const std::vector<Entry> deEntries = skills::BuildSkillsEntries(de, deEngine, skills::DefaultPicker(de), text, deLayout);
            Check(deEntries.size() == entries.size(), "deDE: same entries");
            const skills::Skill* deFireBall = de.FindByKey("fire ball");
            Check(deFireBall != nullptr && FindBlock(deEntries, deFireBall->name) != nullptr, "deDE: Fire Ball by its key");
        } else {
            Check(false, "deDE skills-data.json: " + error);
        }
    }

    if (failures == 0) std::printf("skills_page_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
