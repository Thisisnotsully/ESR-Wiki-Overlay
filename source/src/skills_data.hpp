// The Skills tab's data: data/skills-data.json (enUS) or data/locales/<code>/skills-data.json, as written by
// tools/build_skills.mjs from the vendored esr-docs generator. Parse() turns the JSON text into typed structs and keeps
// nothing of the JSON document. Every language file is self-contained: the formulas inside it name skills by their
// (lower-case) keys, which are the same in every language.
//
// Platform-neutral: no Windows headers. Strings are UTF-8.
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace esrwiki::skills {

namespace calc {
struct Expr;  // skill_calc.hpp
}

// A calc column: a formula string, a plain number, or nothing (missing / null / ""), as the generator stores it.
// The compiled expression is filled in on first use by skill_calc.cpp and cached here (one compile per formula).
struct Formula {
    enum class Kind { None, Number, Text };
    Kind kind = Kind::None;
    double number = 0;
    std::string text;
    mutable std::shared_ptr<const calc::Expr> compiled;

    bool Empty() const { return kind == Kind::None || (kind == Kind::Text && text.empty()); }
};

// One description line (skilldesc descline / dsc2line / dsc3line): t is the line type (descfunc), a and b the texts
// (format strings), x, y, z the calcs.
struct Line {
    int t = 0;
    std::string a;
    std::string b;
    Formula x;
    Formula y;
    Formula z;
};

// Where a skill comes from on items: k is the kind ("Unique", "Runeword", ...), n the name, v the levels / text,
// c an optional count (0 when absent).
struct Source {
    std::string k;
    std::string n;
    std::string v;
    int c = 0;
};

// A db.s (skill) or db.m (missile) record: the calc columns the formulas read.
struct Record {
    std::string k;  // lower-case key
    double mx = 0;  // max level (0 = none)
    std::vector<double> p;  // par1..par8
    std::vector<Formula> c;  // clc1..
    std::vector<double> mn;  // mana: [base, per level, shift, minimum]
    std::vector<double> th;  // to-hit: [base, per level]
    bool hasHs = false;
    double hs = 8;  // hit shift (damage is in 2^hs / 256 points)
    std::vector<double> ed;  // elemental damage [min x6, max x6]
    std::vector<double> el;  // elemental length [base, steps x3]
    std::vector<double> pd;  // physical damage [min x6, max x6]
    Formula esym;
    Formula lsym;
    Formula psym;
    std::vector<Formula> as;  // ast1..
    std::vector<Formula> ps;  // pst1..pst9, ps10..
    Formula rng;
    Formula len;
    Formula pet;
    std::vector<std::string> ms;  // description missiles (db.m keys)
    double r = 0;    // missile range
    double rad = 0;  // missile radius
};

// db.aliases: a token renamed to another token, or pinned to a number.
struct Alias {
    bool isNumber = false;
    double number = 0;
    std::string name;
};

struct Db {
    std::unordered_map<std::string, Record> s;  // skills by key
    std::unordered_map<std::string, Record> m;  // missiles by key
    std::unordered_set<std::string> base;       // basic skills every character has (at level 1)
    std::unordered_map<std::string, double> stats;  // stat defaults (stat('x'.accr))
    std::unordered_map<std::string, Alias> aliases;

    const Record* Skill(const std::string& key) const;
    const Record* Missile(const std::string& key) const;
};

struct Tree {
    std::string id;     // "ama-tree-1"
    int n = 0;          // 1..3, matches Skill::tree
    std::string title;
};

struct Class {
    std::string id;     // "ama", ..., "other"
    std::string title;
    int perLevel = 0;   // skill points per character level (0 when absent)
    std::vector<Tree> trees;
    std::string intro;  // optional class introduction
};

struct Skill {
    std::string id;     // "ama-cold-arrow": unique, stable
    std::string name;   // n: display name (localized)
    std::string cls;    // c: class id
    std::string key;    // k: stable English key (db.s key), unique
    int tree = 0;       // tr (0 = no tree, e.g. item-only skills)
    int row = 0;
    int col = 0;
    int requiredLevel = 0;  // r (0 = none given)
    std::vector<std::string> prerequisites;  // pre: display names of required skills (same class)
    int onStriking = -1;  // os: 1 / 0, -1 when unknown
    std::string onStrikingReason;  // osr
    int onCast = -1;      // oc
    std::string onCastReason;      // ocr
    std::string description;       // d (may contain '\n')
    std::vector<Line> general;     // g
    std::vector<Line> levelLines;  // l (values at the chosen level)
    std::vector<Line> synergyLines;  // s
    std::vector<Source> sources;   // src
    std::vector<std::string> synergies;   // sy: skill ids whose hard points give synergy bonuses
    std::vector<std::string> levelLinks;  // sl: skill ids whose level (with +skills) the numbers use
    bool passive = false;    // pas
    bool aura = false;       // aur
    bool hardPoints = false; // hp: gives more per hard point
    const Record* record = nullptr;  // db.s[key]; never null after a successful Parse
    std::string searchText;  // name, class, tree, description, prerequisites, line texts, sources (not folded)
};

struct Levels {
    int max = 110;
    int skill = 20;      // default skill level
    int character = 110; // default character level
    int synergy = 20;    // hard point / synergy cap
};

// Move-only: skills point at their db.s records.
struct Data {
    Data() = default;
    Data(const Data&) = delete;
    Data& operator=(const Data&) = delete;
    Data(Data&&) = default;
    Data& operator=(Data&&) = default;

    std::string source;
    std::string lang;
    std::vector<std::string> notes;
    Levels levels;
    int questPoints = 0;
    int difficulties = 0;
    std::vector<Class> classes;
    std::vector<Skill> skills;
    Db db;

    const Skill* FindByKey(const std::string& key) const;
    const Skill* FindById(const std::string& id) const;
    const Class* FindClass(const std::string& classId) const;
    const Tree* FindTree(const std::string& classId, int treeN) const;
    // Skills of a class (all trees) / one tree, in data order.
    std::vector<const Skill*> SkillsOfClass(const std::string& classId) const;
    std::vector<const Skill*> SkillsOfTree(const std::string& classId, int treeN) const;
    // The skills named in skill.prerequisites (matched by display name within the class, then anywhere).
    std::vector<const Skill*> Prerequisites(const Skill& skill) const;
    // Every prerequisite, transitively, nearest first, without repeats.
    std::vector<const Skill*> AllPrerequisites(const Skill& skill) const;
    // The highest required level of the skill and all its prerequisites (0 when none is given).
    int RequiredLevelWithPrerequisites(const Skill& skill) const;

    std::unordered_map<std::string, std::size_t> byKey;
    std::unordered_map<std::string, std::size_t> byId;
};

// Reads the whole JSON text. On failure returns false and `error` says why; `out` is then left empty.
bool Parse(const std::string& jsonText, Data& out, std::string& error);

// The text of a line's templates without placeholders ("%d%% Weapon Damage" -> " % Weapon Damage"), as the search
// index uses it (skills.js templateText).
std::string TemplateText(const Line& line);

}  // namespace esrwiki::skills
