// Skill formula evaluator and description-line formatter: a port of the vendored esr-docs
// tools/esr-docs/site/assets/skill-calc.js (window.ESRSkillCalc), which the website's Skills page uses. The results
// are byte-identical to the JavaScript (tests/skill_calc_parity.mjs + tests/skill_calc_test.cpp check every line of
// every skill in all 13 languages).
//
// Semantics kept from the JavaScript, on purpose:
//   - Values are doubles. '/' truncates toward zero and x/0 is 0; '%' is fmod (sign of the dividend) and x%0 is 0;
//     comparisons, '!', '&&' and '||' give 0 or 1. '&&' and '||' evaluate both sides (no short-circuit), '?:' only
//     the branch taken.
//   - A sign in front of a ternary's condition applies to the result: "-(c)?a:b" is -(c?a:b).
//   - A formula that does not parse evaluates to 0 (the page then shows the line with 0, or hides it when it reads
//     "0%"): there is no error text. Compile() keeps the parser's message for diagnostics (Expr::message).
//   - Other skills are at level 0 unless set by synergy points (Context::synergyPoints), except the basic skills
//     every character has (db.base), which are at level 1. stat('x'.accr) is 0 unless db.stats sets it.
//   - A Calculator memoises clc / ast / pst / len / rng / pets columns for the whole session, as the page's Calc does.
//
// Platform-neutral, no Windows headers. Strings are UTF-8.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "skills_data.hpp"

namespace esrwiki::skills {

namespace calc {

enum class Op {
    Error,    // did not parse; message says why
    Number,   // value
    Ident,    // name (lower case): lvl, blvl, ulvl, par1, clc2, ln12, edmn, m1en, ...
    Ref,      // func('name'.path...): skill / stat / miss / sklvl / sksrc
    Call,     // func(args...): min / max
    Ternary,  // a ? b : c  -> cond, a, b
    Neg, Pos, Not,
    Add, Sub, Mul, Div, Mod,
    Eq, Ne, Lt, Gt, Le, Ge,
    And, Or,
};

// One element of a lookup path ('Fire Ball'.lvl.edmn). name is the element as the JavaScript would turn it into a
// property name (numbers in their JavaScript String() form).
struct PathPart {
    bool isNumber = false;
    double number = 0;
    std::string name;
};

struct Expr {
    Op op = Op::Error;
    double value = 0;           // Number
    std::string name;           // Ident: token; Ref: looked-up name (lower case); Call / Ref: see func
    std::string func;           // Ref / Call: function name (lower case)
    std::vector<PathPart> path; // Ref
    std::string message;        // Error
    std::unique_ptr<Expr> a, b, c;              // operands; Ternary: c = condition, a = then, b = else
    std::vector<std::unique_ptr<Expr>> args;    // Call
};

}  // namespace calc

// Parses a formula (game calc string). Never returns null: a formula that does not parse gives an Op::Error node.
std::shared_ptr<const calc::Expr> Compile(const std::string& formula);
// The formula's compiled expression, compiled once and kept in the Formula. Null for an empty / numeric formula.
const calc::Expr* Compiled(const Formula& formula);

// Another skill's levels: blvl = hard points (what synergies use), lvl = level with +skills.
struct SkillLevel {
    double blvl = 0;
    double lvl = 0;
};

struct CalcOptions {
    int characterLevel = 1;               // ulvl
    std::optional<int> hardPoints;        // caps the described skill's blvl; nullopt = equal to the level (page default)
    std::unordered_map<std::string, SkillLevel> levels;  // other skills by key (synergy points)
    // Called for things the JavaScript warns about (parse, token, missile, skill, function, recursion, operator).
    std::function<void(const std::string& kind, const std::string& detail)> warn;
};

// The JavaScript Calc: evaluation state for one set of options. Reuse it while the options stay the same (it keeps a
// memo), as the page does; make a new one when the character level, hard points or synergy points change.
// Not thread-safe.
class Calculator {
public:
    Calculator(const Data& data, CalcOptions options);

    const CalcOptions& Options() const { return options_; }

    // Evaluate a formula for skill record `rec` at skill level `lvl` (Calc.run).
    double Run(const Record& rec, const Formula& formula, double lvl);
    double Run(const Record& rec, const calc::Expr& expr, double lvl);
    // A description line's display text (Calc.line): may contain '\n' for multi-line strings; nullopt for a hidden
    // line (no line type, or a percentage that comes out as 0).
    std::optional<std::string> FormatLine(const Record& rec, const Line& line, double lvl);

private:
    struct MemoKey {
        const Record* rec;
        std::string key;
        double lvl;
        const Record* self;  // null when no self or self is rec
        double selfLvl;
        bool operator==(const MemoKey& o) const {
            return rec == o.rec && key == o.key && lvl == o.lvl && self == o.self && selfLvl == o.selfLvl;
        }
    };
    struct MemoHash {
        std::size_t operator()(const MemoKey& k) const;
    };

    double RunExpr(const Record& rec, const calc::Expr* expr, const std::string& source, double lvl);
    double Cached(const Record& rec, const std::string& key, const Formula* formula, double lvl);
    double Node(const calc::Expr& n, const Record& rec, double lvl);
    double Fn(const Record& rec, const calc::Expr& n, double lvl);
    double Token(const Record& rec, std::string_view name, double lvl);
    double ManaCost(const Record& rec, double lvl) const;
    double Damage(const Record& src, const Record& rec, std::string_view which, double lvl);
    double Ref(const Record& rec, const calc::Expr& n, double lvl);
    void Warn(const char* kind, const std::string& detail) const;

    const Data& data_;
    CalcOptions options_;
    std::unordered_map<MemoKey, double, MemoHash> memo_;
    int depth_ = 0;
    const Record* self_ = nullptr;  // the skill being described
    double selfLvl_ = 0;
};

// What the Skills tab shows a skill at.
struct Context {
    const Skill* skill = nullptr;
    int skillLevel = 20;       // 1..levels.max
    int characterLevel = 110;  // 1..levels.max
    std::optional<int> hardPoints;  // 0..levels.synergy; nullopt = hard points equal the level up to the cap (default)
    // Synergy points by skill id (Skill::synergies: hard points, capped at levels.synergy; Skill::levelLinks:
    // levels with +skills, up to levels.max). Ids that are not the skill's synergies are ignored, as on skill.html.
    std::map<std::string, int> synergyPoints;
};

// The options a Context evaluates with (skill.js / skill.html getCalc).
CalcOptions OptionsFor(const Data& data, const Context& ctx);

// One-off helpers: a fresh Calculator each call (use Engine to evaluate many lines).
double Evaluate(const Data& data, const calc::Expr& expr, const Context& ctx);
std::optional<std::string> FormatLine(const Data& data, const Line& line, const Context& ctx);

struct CardLine {
    std::string text;     // may contain '\n' (draw as separate lines)
    bool header = false;  // line type 40 ("X Receives Bonuses From:")
};

struct CardLines {
    std::vector<CardLine> general;    // Skill::general
    std::vector<CardLine> current;    // Skill::levelLines
    std::vector<CardLine> synergies;  // Skill::synergyLines
};

// Keeps one Calculator per (character level, hard points, synergy points), like the page's getCalc().
class Engine {
public:
    explicit Engine(const Data& data) : data_(data) {}

    Calculator& CalculatorFor(const Context& ctx);
    std::optional<std::string> FormatLine(const Context& ctx, const Line& line);
    // A card's lines at ctx (hidden and empty lines left out), as skills.js fillLines.
    CardLines Card(const Context& ctx);

private:
    const Data& data_;
    std::unique_ptr<Calculator> calc_;
    int clvl_ = -1;
    std::optional<int> hp_;
    std::unordered_map<std::string, SkillLevel> levels_;
};

// Whether a line's text can change with the levels (skill-calc.js usesCalc).
bool UsesCalc(const Line& line);
// Whether any of the skill's lines differ between character level 1 and levels.max (skills.js s._ulvl).
bool ScalesWithCharacter(const Data& data, const Skill& skill);
// Hard points at a skill level: hp (or the level when nullopt) capped by the level and levels.synergy.
int HardPointsAt(const Data& data, int skillLevel, std::optional<int> hardPoints);

// JavaScript number formatting, exposed for tests.
namespace js {
std::string NumberToString(double v);                  // String(v)
std::string ToFixed(double v, int digits);             // v.toFixed(digits), digits 0..100
double StringToNumber(std::string_view s);             // +s
std::string ToLower(std::string_view utf8);            // s.toLowerCase() (Latin, Greek, Cyrillic, Armenian, full-width)
struct Arg {
    enum class Kind { Undefined, Number, String };
    Kind kind = Kind::Undefined;
    double number = 0;
    std::string text;
};
std::string Sprintf(std::string_view tmpl, const std::vector<Arg>& vals);  // skill-calc.js sprintf
}  // namespace js

}  // namespace esrwiki::skills
