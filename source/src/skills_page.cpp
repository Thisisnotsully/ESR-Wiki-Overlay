// The Skills tab as wiki entries and blocks (see skills_page.hpp).
#include "skills_page.hpp"

#include <algorithm>
#include <utility>

namespace esrwiki::skills {

namespace {

const char kDot[] = " \xC2\xB7 ";     // " · " between the parts of a line
const char kTimes[] = " \xC3\x97";    // " ×" before a source's count

std::string Lookup(const std::function<std::string(const std::string&)>& lookup, const std::string& english) {
    if (!lookup || english.empty()) return english;
    std::string text = lookup(english);
    return text.empty() ? english : text;
}

// The non-empty lines of a text that may hold '\n'.
std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string piece = text.substr(start, end - start);
        if (!piece.empty() && piece.back() == '\r') piece.pop_back();
        if (!piece.empty()) out.push_back(std::move(piece));
        start = end + 1;
    }
    return out;
}

std::string Join(const std::vector<std::string>& parts, const std::string& separator) {
    std::string out;
    for (const std::string& part : parts) {
        if (part.empty()) continue;
        if (!out.empty()) out += separator;
        out += part;
    }
    return out;
}

// A list ("Fire Bolt, Inferno") joined through the "{0}, {1}" phrase, so each language keeps its own separator.
std::string JoinList(const std::vector<std::string>& parts, const PageText& text) {
    std::string out;
    for (const std::string& part : parts) {
        if (part.empty()) continue;
        out = out.empty() ? part : text.Format("{0}, {1}", {out, part});
    }
    return out;
}

std::string ClassTitle(const Data& data, const std::string& classId, const PageText& text) {
    const Class* found = data.FindClass(classId);
    return found == nullptr ? classId : text.Phrase(found->title);
}

std::string TreeTitle(const Data& data, const Skill& skill) {
    if (skill.tree <= 0) return std::string();
    const Tree* tree = data.FindTree(skill.cls, skill.tree);
    return tree == nullptr ? std::string() : tree->title;
}

// "Yes. It hits with your weapon." for the on-striking and on-casting fields.
std::string ProcText(int flag, const std::string& reason, const PageText& text) {
    const std::string answer = text.Ui(flag == 1 ? "Yes" : "No");
    return reason.empty() ? answer : text.Format("{0}. {1}", {answer, text.Phrase(reason)});
}

void AddLines(std::vector<esrwiki::Line>& lines, const std::string& text, LineKind firstKind) {
    bool first = true;
    for (std::string& piece : SplitLines(text)) {
        lines.push_back(esrwiki::Line{first ? firstKind : LineKind::Text, std::move(piece)});
        first = false;
    }
}

Block MakeBlock(const Data& data, Engine& engine, const Skill& skill, const Picker& picker, const PageText& text,
                CardFace& face) {
    Block block;
    block.item = true;
    block.title = skill.name;
    block.level = skill.requiredLevel;
    block.section = TreeTitle(data, skill);

    std::vector<std::string> tags;
    if (skill.passive) tags.push_back(text.Ui("Passive"));
    if (skill.aura) tags.push_back(text.Ui("Aura"));
    block.subtitle = Join({ClassTitle(data, skill.cls, text), block.section, Join(tags, kDot)}, kDot);

    std::vector<std::string> prerequisites;
    for (const std::string& name : skill.prerequisites) prerequisites.push_back(name);
    const std::string required = skill.requiredLevel > 0 ? std::to_string(skill.requiredLevel) : std::string();
    if (!required.empty()) block.fields.push_back(Field{text.Ui("Required Level"), required});
    const std::string prerequisiteText = JoinList(prerequisites, text);
    if (!prerequisites.empty()) block.fields.push_back(Field{text.Ui("Requires"), prerequisiteText});
    if (skill.onStriking >= 0)
        block.fields.push_back(Field{text.Ui("Procs on striking"), ProcText(skill.onStriking, skill.onStrikingReason, text)});
    if (skill.onCast >= 0)
        block.fields.push_back(Field{text.Ui("Procs on casting"), ProcText(skill.onCast, skill.onCastReason, text)});

    Context ctx;
    ctx.skill = &skill;
    ctx.skillLevel = picker.skillLevel;
    ctx.characterLevel = picker.characterLevel;
    ctx.hardPoints = picker.hardPoints;
    const CardLines card = engine.Card(ctx);

    AddLines(block.lines, skill.description, LineKind::Text);
    for (const CardLine& line : card.general) AddLines(block.lines, line.text, line.header ? LineKind::Heading : LineKind::Text);
    if (!card.current.empty()) {
        block.lines.push_back(esrwiki::Line{LineKind::Heading, text.Format("At skill level {0}", {std::to_string(picker.skillLevel)})});
        for (const CardLine& line : card.current) AddLines(block.lines, line.text, LineKind::Text);
    }
    if (!card.synergies.empty()) {
        block.lines.push_back(esrwiki::Line{LineKind::Heading, text.Ui("Synergies")});
        for (const CardLine& line : card.synergies) AddLines(block.lines, line.text, LineKind::Text);
    }
    if (!skill.sources.empty()) {
        block.lines.push_back(esrwiki::Line{LineKind::Heading,
                                   text.Format("Granted by {0} items and recipes", {std::to_string(skill.sources.size())})});
        for (const Source& source : skill.sources) {
            std::string row = Join({text.Phrase(source.k), text.Phrase(source.n), text.Phrase(source.v)}, kDot);
            if (source.c > 1) row += kTimes + std::to_string(source.c);
            if (!row.empty()) block.lines.push_back(esrwiki::Line{LineKind::Text, std::move(row)});
        }
    }

    face = CardFace{};
    face.tags = Join(tags, kDot);
    if (!required.empty()) face.requirement = text.Format("{0}: {1}", {text.Ui("Required Level"), required});
    if (!prerequisites.empty()) face.prerequisites = text.Format("{0}: {1}", {text.Ui("Requires"), prerequisiteText});
    for (const std::vector<CardLine>* group : {&card.current, &card.general}) {
        for (const CardLine& line : *group) {
            if (face.lines.size() >= kFaceLines) break;
            if (line.header) continue;
            const std::vector<std::string> pieces = SplitLines(line.text);
            if (!pieces.empty()) face.lines.push_back(pieces.front());
        }
        if (!face.lines.empty()) break;  // the general lines only stand in when there are no lines at the level
    }
    return block;
}

// The entries' shape: per entry, the skills of its blocks (null for a block that is not a skill).
std::vector<std::vector<const Skill*>> Shape(const Data& data) {
    std::vector<std::vector<const Skill*>> shape;
    for (const Class& cls : data.classes) {
        std::vector<const Skill*> skills = data.SkillsOfClass(cls.id);
        std::vector<const Skill*> blocks;
        if (!cls.intro.empty()) blocks.push_back(nullptr);  // the class introduction (the item skills have one)
        for (const Skill* skill : skills) blocks.push_back(skill);
        if (skills.empty()) continue;
        shape.push_back(std::move(blocks));
    }
    return shape;
}

}  // namespace

std::string PageText::Ui(const std::string& english) const { return Lookup(ui, english); }

std::string PageText::Phrase(const std::string& english) const { return Lookup(data, english); }

std::string PageText::Format(const std::string& english, const std::vector<std::string>& args) const {
    std::string out = Ui(english);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string token = "{" + std::to_string(i) + "}";
        for (std::size_t at = out.find(token); at != std::string::npos; at = out.find(token, at + args[i].size()))
            out.replace(at, token.size(), args[i]);
    }
    return out;
}

Picker DefaultPicker(const Data& data) {
    Picker picker;
    picker.skillLevel = data.levels.skill;
    picker.characterLevel = data.levels.character;
    picker.hardPoints.reset();
    return ClampPicker(data, picker);
}

Picker ClampPicker(const Data& data, Picker picker) {
    const int top = std::max(1, data.levels.max);
    picker.skillLevel = std::clamp(picker.skillLevel, 1, top);
    picker.characterLevel = std::clamp(picker.characterLevel, 1, top);
    if (picker.hardPoints) picker.hardPoints = std::clamp(*picker.hardPoints, 0, std::max(0, data.levels.synergy));
    return picker;
}

Picker StepPicker(const Data& data, Picker picker, int field, int delta) {
    if (field == 0) {
        picker.skillLevel += delta;
    } else if (field == 1) {
        picker.characterLevel += delta;
    } else if (field == 2) {
        // Default is the step below 0.
        const int now = picker.hardPoints ? *picker.hardPoints : -1;
        const int next = std::clamp(now + delta, -1, std::max(0, data.levels.synergy));
        if (next < 0) picker.hardPoints.reset();
        else picker.hardPoints = next;
    }
    return ClampPicker(data, picker);
}

std::vector<Entry> BuildSkillsEntries(const Data& data, Engine& engine, const Picker& picker, const PageText& text,
                                      PageLayout& layout) {
    layout = PageLayout{};
    layout.picker = ClampPicker(data, picker);
    std::vector<Entry> entries;
    for (const Class& cls : data.classes) {
        const std::vector<const Skill*> skills = data.SkillsOfClass(cls.id);
        if (skills.empty()) continue;
        Entry entry;
        entry.title = ClassTitle(data, cls.id, text);
        std::vector<const Skill*> blockSkills;
        std::vector<CardFace> faces;
        if (!cls.intro.empty()) {
            Block intro;
            AddLines(intro.lines, text.Phrase(cls.intro), LineKind::Text);
            entry.blocks.push_back(std::move(intro));
            blockSkills.push_back(nullptr);
            faces.emplace_back();
        }
        for (const Skill* skill : skills) {
            CardFace face;
            entry.blocks.push_back(MakeBlock(data, engine, *skill, layout.picker, text, face));
            blockSkills.push_back(skill);
            faces.push_back(std::move(face));
        }
        entries.push_back(std::move(entry));
        layout.skills.push_back(std::move(blockSkills));
        layout.faces.push_back(std::move(faces));
    }
    return entries;
}

bool RefreshSkillsEntries(const Data& data, Engine& engine, const Picker& picker, const PageText& text,
                          std::vector<Entry>& entries, PageLayout& layout) {
    if (entries.size() != layout.skills.size() || layout.faces.size() != layout.skills.size()) return false;
    if (layout.skills != Shape(data)) return false;
    for (std::size_t e = 0; e < entries.size(); ++e)
        if (entries[e].blocks.size() != layout.skills[e].size()) return false;
    layout.picker = ClampPicker(data, picker);
    for (std::size_t e = 0; e < entries.size(); ++e) {
        for (std::size_t b = 0; b < entries[e].blocks.size(); ++b) {
            const Skill* skill = layout.skills[e][b];
            if (skill == nullptr) continue;
            // Assigned in place: the block keeps its address, and the new one's search cache is empty.
            entries[e].blocks[b] = MakeBlock(data, engine, *skill, layout.picker, text, layout.faces[e][b]);
        }
    }
    return true;
}

int FindSkillsPage(const Database& db) {
    for (std::size_t i = 0; i < db.pages.size(); ++i)
        if (db.pages[i].id == kSkillsPageId) return static_cast<int>(i);
    return -1;
}

std::size_t InsertSkillsPage(Database& db, std::vector<Entry> entries, const PageText& text) {
    const int existing = FindSkillsPage(db);
    if (existing >= 0) {
        Page& page = db.pages[static_cast<std::size_t>(existing)];
        page.title = text.Ui("Skills");
        page.entries = std::move(entries);
        return static_cast<std::size_t>(existing);
    }
    Page page;
    page.id = kSkillsPageId;
    page.title = text.Ui("Skills");
    page.group = "Endgame";
    page.entries = std::move(entries);
    std::size_t at = db.pages.size();
    for (std::size_t i = 0; i < db.pages.size(); ++i) {
        if (db.pages[i].id == kSkillsPageAfter) {
            at = i + 1;
            page.group = db.pages[i].group;
            break;
        }
    }
    db.pages.insert(db.pages.begin() + static_cast<std::ptrdiff_t>(at), std::move(page));
    return at;
}

bool RemoveSkillsPage(Database& db) {
    const int at = FindSkillsPage(db);
    if (at < 0) return false;
    db.pages.erase(db.pages.begin() + at);
    return true;
}

}  // namespace esrwiki::skills
