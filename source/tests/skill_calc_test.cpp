// Tests for the Skills tab model: src/skills_data.cpp (JSON -> typed data) and src/skill_calc.cpp (the port of the
// website's skill-calc.js).
//
//   skill_calc_test <data dir> <fixture dir>
//
// 1. Unit tests: expression parser / evaluator edge cases, sprintf and number formatting, line formatting and the
//    data helpers. The expected values were produced by the JavaScript (tools/esr-docs/site/assets/skill-calc.js).
// 2. Parity: for every language file (<data dir>/skills-data.json and <data dir>/locales/<code>/skills-data.json),
//    every skill and every line, the text must be byte-identical to the fixture written by
//    tests/skill_calc_parity.mjs (<fixture dir>/<lang>.txt). The first 20 mismatches are printed.
// 3. Timing: loading a language file, and formatting every line of all skills for one context (printed only).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "skill_calc.hpp"
#include "skills_data.hpp"

using namespace esrwiki::skills;

namespace {

int g_failures = 0;

void Fail(const std::string& what) {
    ++g_failures;
    if (g_failures <= 60) std::printf("FAIL: %s\n", what.c_str());
}

std::string Show(const std::optional<std::string>& s) {
    if (!s) return "(hidden)";
    std::string out = "\"";
    for (const char c : *s) {
        if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
}

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

double Ms(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// ---------------------------------------------------------------------------------------------------------------
// Unit tests (expected values from the JavaScript)
// ---------------------------------------------------------------------------------------------------------------

const char* const kUnitJson = R"JSON(
{"source":"test","lang":"enUS","levels":{"max":110,"skill":20,"character":110,"synergy":20},"classes":[{"id":"tst","title":"Tester","trees":[{"id":"tst-tree-1","n":1,"title":"Tree"}]}],"skills":[{"id":"tst-test","n":"Test","c":"tst","k":"test","tr":1,"r":6,"pre":["Helper"],"l":[{"t":36,"a":"Mana Cost: %d","x":"usmc","y":"256"},{"t":36,"a":"Length: %d second","b":"Length: %d seconds","x":"len","y":"25"},{"t":63,"a":"Resist","b":"x","x":"0"},{"t":63,"a":"Resist","b":"x","x":"lvl"},{"t":3,"a":"Attack Rating:","b":" percent","x":"lvl"},{"t":5,"a":"Bonus:","x":"-lvl"},{"t":18,"a":"Line one\nLine two"},{"t":74,"a":"%d-%d damage\n%d%% chance","x":"edmn","y":"edmx","z":"lvl"},{"t":0,"a":"hidden"},{"t":99,"a":"%d|%d|%d","x":"1","y":"2","z":"3"},{"t":40,"a":"%s Receives Bonuses From:","b":"Test"},{"t":76,"a":"%s: %+d%% Damage per Level","b":"Helper","x":"par8"},{"t":79,"a":"%s: %d per second","b":"Helper","x":"lvl","y":"3"},{"t":6,"a":" to Attack","b":"%","x":"lvl"},{"t":4,"a":"Speed ","b":"%","x":"0-lvl"},{"t":67,"a":"Life","b":"points","x":"lvl"},{"t":25,"a":"A ","b":" B"},{"t":13,"a":"  %d  \t  spaced  ","x":"lvl"},{"t":3,"a":"x","b":"0%","x":"10"},{"t":3,"a":"x:","b":"%","x":"0"},{"t":36,"a":"%d percent","x":"lvl","y":"2"}]},{"id":"tst-helper","n":"Helper","c":"tst","k":"helper","tr":1,"r":1},{"id":"tst-attack","n":"Attack","c":"tst","k":"attack"}],"db":{"s":{"test":{"k":"test","mx":20,"p":[3,7,0,0,0,0,0,5],"c":["par1*2","clc3+1","clc2+1","skill('helper'.clc1)"],"mn":[2,1,8,3],"th":[10,5],"hs":8,"ed":[1,2,3,4,5,6,10,20,30,40,50,60],"esym":"skill('helper'.blvl)*10","el":[25,5,3,1],"lsym":"50","as":["lvl*3"],"ps":["7","8","9","10","11","12","13","14","15","16"],"len":"lvl*25","ms":["arrow"]},"helper":{"k":"helper","mx":20,"p":[4],"c":["skill('test'.lvl)*100"],"hs":8,"mn":[1]},"attack":{"k":"attack","mx":1,"p":[9],"hs":8,"mn":[0]}},"m":{"arrow":{"k":"arrow","r":12,"rad":3,"hs":8,"ed":[2,1,0,0,0,0,4,1,0,0,0,0],"esym":"10","p":[6]}},"base":["attack"],"stats":{"item_x":5},"aliases":{"me3o":"m3eo","pinned":42}}})JSON";

struct ExprCase {
    const char* formula;
    int hardPoints;  // -1 = default
    int lvl;
    double expected;
    bool parseError;
};

// Evaluated in this order with one Calculator per (hard points, level), character level 60, like the JavaScript.
const ExprCase kExprCases[] = {
    {"7/2", -1, 1, 3, false},
    {"-7/2", -1, 1, -3, false},
    {"7/0", -1, 1, 0, false},
    {"-7%3", -1, 1, -1, false},
    {"7%-3", -1, 1, 1, false},
    {"5%0", -1, 1, 0, false},
    {"1+2*3", -1, 1, 7, false},
    {"(1+2)*3", -1, 1, 9, false},
    {"2-3-4", -1, 1, -5, false},
    {"8/2/2", -1, 1, 2, false},
    {"3>2", -1, 1, 1, false},
    {"2>=3", -1, 1, 0, false},
    {"1==1", -1, 1, 1, false},
    {"1!=1", -1, 1, 0, false},
    {"1<2==1", -1, 1, 1, false},
    {"!0", -1, 1, 1, false},
    {"!5", -1, 1, 0, false},
    {"--3", -1, 1, 3, false},
    {"-+-3", -1, 1, 3, false},
    {"0&&1", -1, 1, 0, false},
    {"2||0", -1, 1, 1, false},
    {"1?2:3", -1, 1, 2, false},
    {"0?2:3", -1, 1, 3, false},
    {"1?0?5:6:7", -1, 1, 6, false},
    {"0?1:0?2:3", -1, 1, 3, false},
    {"-(1>0)?0:(10+lvl/2)", -1, 1, -0.0, false},
    {"-(0)?0:(10+lvl/2)", -1, 1, -10, false},
    {"+(0)?1:lvl", -1, 1, 1, false},
    {"min(3,1,2)", -1, 1, 1, false},
    {"max(1,4)", -1, 1, 4, false},
    {"min()", -1, 1, 0, false},
    {"foo(7,2)", -1, 1, 7, false},
    {"max(-1)", -1, 1, -1, false},
    {"\"min(ln12,30)\"", -1, 1, 3, false},
    {"  \"lvl\"  ", -1, 1, 1, false},
    {"ln12", -1, 1, 3, false},
    {"dm12", -1, 1, 3, false},
    {"par1", -1, 1, 3, false},
    {"par8", -1, 1, 5, false},
    {"par9", -1, 1, 0, false},
    {"blvl", -1, 1, 1, false},
    {"ulvl", -1, 1, 60, false},
    {"toht", -1, 1, 10, false},
    {"usmc", -1, 1, 768, false},
    {"skill('attack'.par1)", -1, 1, 9, false},
    {"skill('attack'.blvl)", -1, 1, 1, false},
    {"skill('other'.blvl)", -1, 1, 0, false},
    {"stat('item_x'.accr)", -1, 1, 5, false},
    {"stat('nope'.accr)", -1, 1, 0, false},
    {"sksrc('x'.y)", -1, 1, 0, false},
    {"clc1", -1, 1, 6, false},
    {"clc2", -1, 1, 2, false},
    {"clc3", -1, 1, 1, false},
    {"clc4", -1, 1, 0, false},
    {"ast1", -1, 1, 3, false},
    {"pst1", -1, 1, 7, false},
    {"ps10", -1, 1, 16, false},
    {"ps19", -1, 1, 0, false},
    {"len", -1, 1, 25, false},
    {"edmn", -1, 1, 1, false},
    {"edmx", -1, 1, 10, false},
    {"enma", -1, 1, 1, false},
    {"exma", -1, 1, 10, false},
    {"edns", -1, 1, 256, false},
    {"edln", -1, 1, 37, false},
    {"m1en", -1, 1, 2, false},
    {"m1ex", -1, 1, 4, false},
    {"m2en", -1, 1, 0, false},
    {"miss('arrow'.rang)", -1, 1, 12, false},
    {"miss('arrow'.rad)", -1, 1, 3, false},
    {"miss('arrow'.par1)", -1, 1, 6, false},
    {"miss('arrow'.enma)", -1, 1, 2, false},
    {"miss('nope'.rang)", -1, 1, 0, false},
    {"sklvl('helper'.5.clc1)", -1, 1, 100, false},
    {"sklvl('helper'.lvl.par1)", -1, 1, 4, false},
    {"sklvl('Helper'.blvl)", -1, 1, 1, false},
    {"SKILL('TEST'.LVL)", -1, 1, 1, false},
    {"pinned", -1, 1, 42, false},
    {"bogus", -1, 1, 0, false},
    {"1+", -1, 1, 0, true},
    {"(1", -1, 1, 0, true},
    {"1 2", -1, 1, 0, true},
    {"a.b", -1, 1, 0, true},
    {"skill(", -1, 1, 0, true},
    {"'abc'", -1, 1, 0, true},
    {"1 = 2", -1, 1, 0, true},
    {"#", -1, 1, 0, true},
    {"", -1, 1, 0, false},
    {"   ", -1, 1, 0, false},
    {"lvl*1000000000000*1000000000", -1, 1, 1e+21, false},
    {" 3 *\t4 ", -1, 1, 12, false},
    {"10-lvl*2", -1, 1, 8, false},
    {"(lvl+1)%4", -1, 1, 2, false},
    {"lvl>3&&lvl<10", -1, 1, 0, false},
    {"!lvl||0", -1, 1, 0, false},
    {"7/2", -1, 5, 3, false},
    {"-7/2", -1, 5, -3, false},
    {"7/0", -1, 5, 0, false},
    {"-7%3", -1, 5, -1, false},
    {"7%-3", -1, 5, 1, false},
    {"5%0", -1, 5, 0, false},
    {"1+2*3", -1, 5, 7, false},
    {"(1+2)*3", -1, 5, 9, false},
    {"2-3-4", -1, 5, -5, false},
    {"8/2/2", -1, 5, 2, false},
    {"3>2", -1, 5, 1, false},
    {"2>=3", -1, 5, 0, false},
    {"1==1", -1, 5, 1, false},
    {"1!=1", -1, 5, 0, false},
    {"1<2==1", -1, 5, 1, false},
    {"!0", -1, 5, 1, false},
    {"!5", -1, 5, 0, false},
    {"--3", -1, 5, 3, false},
    {"-+-3", -1, 5, 3, false},
    {"0&&1", -1, 5, 0, false},
    {"2||0", -1, 5, 1, false},
    {"1?2:3", -1, 5, 2, false},
    {"0?2:3", -1, 5, 3, false},
    {"1?0?5:6:7", -1, 5, 6, false},
    {"0?1:0?2:3", -1, 5, 3, false},
    {"-(1>0)?0:(10+lvl/2)", -1, 5, -0.0, false},
    {"-(0)?0:(10+lvl/2)", -1, 5, -12, false},
    {"+(0)?1:lvl", -1, 5, 5, false},
    {"min(3,1,2)", -1, 5, 1, false},
    {"max(1,4)", -1, 5, 4, false},
    {"min()", -1, 5, 0, false},
    {"foo(7,2)", -1, 5, 7, false},
    {"max(-1)", -1, 5, -1, false},
    {"\"min(ln12,30)\"", -1, 5, 30, false},
    {"  \"lvl\"  ", -1, 5, 5, false},
    {"ln12", -1, 5, 31, false},
    {"dm12", -1, 5, 5, false},
    {"par1", -1, 5, 3, false},
    {"par8", -1, 5, 5, false},
    {"par9", -1, 5, 0, false},
    {"blvl", -1, 5, 5, false},
    {"ulvl", -1, 5, 60, false},
    {"toht", -1, 5, 30, false},
    {"usmc", -1, 5, 1536, false},
    {"skill('attack'.par1)", -1, 5, 9, false},
    {"skill('attack'.blvl)", -1, 5, 1, false},
    {"skill('other'.blvl)", -1, 5, 0, false},
    {"stat('item_x'.accr)", -1, 5, 5, false},
    {"stat('nope'.accr)", -1, 5, 0, false},
    {"sksrc('x'.y)", -1, 5, 0, false},
    {"clc1", -1, 5, 6, false},
    {"clc2", -1, 5, 2, false},
    {"clc3", -1, 5, 1, false},
    {"clc4", -1, 5, 0, false},
    {"ast1", -1, 5, 15, false},
    {"pst1", -1, 5, 7, false},
    {"ps10", -1, 5, 16, false},
    {"ps19", -1, 5, 0, false},
    {"len", -1, 5, 125, false},
    {"edmn", -1, 5, 9, false},
    {"edmx", -1, 5, 90, false},
    {"enma", -1, 5, 9, false},
    {"exma", -1, 5, 90, false},
    {"edns", -1, 5, 2304, false},
    {"edln", -1, 5, 67, false},
    {"m1en", -1, 5, 6, false},
    {"m1ex", -1, 5, 8, false},
    {"m2en", -1, 5, 0, false},
    {"miss('arrow'.rang)", -1, 5, 12, false},
    {"miss('arrow'.rad)", -1, 5, 3, false},
    {"miss('arrow'.par1)", -1, 5, 6, false},
    {"miss('arrow'.enma)", -1, 5, 6, false},
    {"miss('nope'.rang)", -1, 5, 0, false},
    {"sklvl('helper'.5.clc1)", -1, 5, 500, false},
    {"sklvl('helper'.lvl.par1)", -1, 5, 4, false},
    {"sklvl('Helper'.blvl)", -1, 5, 5, false},
    {"SKILL('TEST'.LVL)", -1, 5, 5, false},
    {"pinned", -1, 5, 42, false},
    {"bogus", -1, 5, 0, false},
    {"1+", -1, 5, 0, true},
    {"(1", -1, 5, 0, true},
    {"1 2", -1, 5, 0, true},
    {"a.b", -1, 5, 0, true},
    {"skill(", -1, 5, 0, true},
    {"'abc'", -1, 5, 0, true},
    {"1 = 2", -1, 5, 0, true},
    {"#", -1, 5, 0, true},
    {"", -1, 5, 0, false},
    {"   ", -1, 5, 0, false},
    {"lvl*1000000000000*1000000000", -1, 5, 5e+21, false},
    {" 3 *\t4 ", -1, 5, 12, false},
    {"10-lvl*2", -1, 5, 0, false},
    {"(lvl+1)%4", -1, 5, 2, false},
    {"lvl>3&&lvl<10", -1, 5, 1, false},
    {"!lvl||0", -1, 5, 0, false},
    {"7/2", -1, 30, 3, false},
    {"-7/2", -1, 30, -3, false},
    {"7/0", -1, 30, 0, false},
    {"-7%3", -1, 30, -1, false},
    {"7%-3", -1, 30, 1, false},
    {"5%0", -1, 30, 0, false},
    {"1+2*3", -1, 30, 7, false},
    {"(1+2)*3", -1, 30, 9, false},
    {"2-3-4", -1, 30, -5, false},
    {"8/2/2", -1, 30, 2, false},
    {"3>2", -1, 30, 1, false},
    {"2>=3", -1, 30, 0, false},
    {"1==1", -1, 30, 1, false},
    {"1!=1", -1, 30, 0, false},
    {"1<2==1", -1, 30, 1, false},
    {"!0", -1, 30, 1, false},
    {"!5", -1, 30, 0, false},
    {"--3", -1, 30, 3, false},
    {"-+-3", -1, 30, 3, false},
    {"0&&1", -1, 30, 0, false},
    {"2||0", -1, 30, 1, false},
    {"1?2:3", -1, 30, 2, false},
    {"0?2:3", -1, 30, 3, false},
    {"1?0?5:6:7", -1, 30, 6, false},
    {"0?1:0?2:3", -1, 30, 3, false},
    {"-(1>0)?0:(10+lvl/2)", -1, 30, -0.0, false},
    {"-(0)?0:(10+lvl/2)", -1, 30, -25, false},
    {"+(0)?1:lvl", -1, 30, 30, false},
    {"min(3,1,2)", -1, 30, 1, false},
    {"max(1,4)", -1, 30, 4, false},
    {"min()", -1, 30, 0, false},
    {"foo(7,2)", -1, 30, 7, false},
    {"max(-1)", -1, 30, -1, false},
    {"\"min(ln12,30)\"", -1, 30, 30, false},
    {"  \"lvl\"  ", -1, 30, 30, false},
    {"ln12", -1, 30, 206, false},
    {"dm12", -1, 30, 6, false},
    {"par1", -1, 30, 3, false},
    {"par8", -1, 30, 5, false},
    {"par9", -1, 30, 0, false},
    {"blvl", -1, 30, 20, false},
    {"ulvl", -1, 30, 60, false},
    {"toht", -1, 30, 155, false},
    {"usmc", -1, 30, 7936, false},
    {"skill('attack'.par1)", -1, 30, 9, false},
    {"skill('attack'.blvl)", -1, 30, 1, false},
    {"skill('other'.blvl)", -1, 30, 0, false},
    {"stat('item_x'.accr)", -1, 30, 5, false},
    {"stat('nope'.accr)", -1, 30, 0, false},
    {"sksrc('x'.y)", -1, 30, 0, false},
    {"clc1", -1, 30, 6, false},
    {"clc2", -1, 30, 2, false},
    {"clc3", -1, 30, 1, false},
    {"clc4", -1, 30, 0, false},
    {"ast1", -1, 30, 90, false},
    {"pst1", -1, 30, 7, false},
    {"ps10", -1, 30, 16, false},
    {"ps19", -1, 30, 0, false},
    {"len", -1, 30, 750, false},
    {"edmn", -1, 30, 105, false},
    {"edmx", -1, 30, 1050, false},
    {"enma", -1, 30, 105, false},
    {"exma", -1, 30, 1050, false},
    {"edns", -1, 30, 26880, false},
    {"edln", -1, 30, 147, false},
    {"m1en", -1, 30, 9, false},
    {"m1ex", -1, 30, 12, false},
    {"m2en", -1, 30, 0, false},
    {"miss('arrow'.rang)", -1, 30, 12, false},
    {"miss('arrow'.rad)", -1, 30, 3, false},
    {"miss('arrow'.par1)", -1, 30, 6, false},
    {"miss('arrow'.enma)", -1, 30, 9, false},
    {"miss('nope'.rang)", -1, 30, 0, false},
    {"sklvl('helper'.5.clc1)", -1, 30, 3000, false},
    {"sklvl('helper'.lvl.par1)", -1, 30, 4, false},
    {"sklvl('Helper'.blvl)", -1, 30, 20, false},
    {"SKILL('TEST'.LVL)", -1, 30, 30, false},
    {"pinned", -1, 30, 42, false},
    {"bogus", -1, 30, 0, false},
    {"1+", -1, 30, 0, true},
    {"(1", -1, 30, 0, true},
    {"1 2", -1, 30, 0, true},
    {"a.b", -1, 30, 0, true},
    {"skill(", -1, 30, 0, true},
    {"'abc'", -1, 30, 0, true},
    {"1 = 2", -1, 30, 0, true},
    {"#", -1, 30, 0, true},
    {"", -1, 30, 0, false},
    {"   ", -1, 30, 0, false},
    {"lvl*1000000000000*1000000000", -1, 30, 3e+22, false},
    {" 3 *\t4 ", -1, 30, 12, false},
    {"10-lvl*2", -1, 30, -50, false},
    {"(lvl+1)%4", -1, 30, 3, false},
    {"lvl>3&&lvl<10", -1, 30, 0, false},
    {"!lvl||0", -1, 30, 0, false},
    {"7/2", 5, 1, 3, false},
    {"-7/2", 5, 1, -3, false},
    {"7/0", 5, 1, 0, false},
    {"-7%3", 5, 1, -1, false},
    {"7%-3", 5, 1, 1, false},
    {"5%0", 5, 1, 0, false},
    {"1+2*3", 5, 1, 7, false},
    {"(1+2)*3", 5, 1, 9, false},
    {"2-3-4", 5, 1, -5, false},
    {"8/2/2", 5, 1, 2, false},
    {"3>2", 5, 1, 1, false},
    {"2>=3", 5, 1, 0, false},
    {"1==1", 5, 1, 1, false},
    {"1!=1", 5, 1, 0, false},
    {"1<2==1", 5, 1, 1, false},
    {"!0", 5, 1, 1, false},
    {"!5", 5, 1, 0, false},
    {"--3", 5, 1, 3, false},
    {"-+-3", 5, 1, 3, false},
    {"0&&1", 5, 1, 0, false},
    {"2||0", 5, 1, 1, false},
    {"1?2:3", 5, 1, 2, false},
    {"0?2:3", 5, 1, 3, false},
    {"1?0?5:6:7", 5, 1, 6, false},
    {"0?1:0?2:3", 5, 1, 3, false},
    {"-(1>0)?0:(10+lvl/2)", 5, 1, -0.0, false},
    {"-(0)?0:(10+lvl/2)", 5, 1, -10, false},
    {"+(0)?1:lvl", 5, 1, 1, false},
    {"min(3,1,2)", 5, 1, 1, false},
    {"max(1,4)", 5, 1, 4, false},
    {"min()", 5, 1, 0, false},
    {"foo(7,2)", 5, 1, 7, false},
    {"max(-1)", 5, 1, -1, false},
    {"\"min(ln12,30)\"", 5, 1, 3, false},
    {"  \"lvl\"  ", 5, 1, 1, false},
    {"ln12", 5, 1, 3, false},
    {"dm12", 5, 1, 3, false},
    {"par1", 5, 1, 3, false},
    {"par8", 5, 1, 5, false},
    {"par9", 5, 1, 0, false},
    {"blvl", 5, 1, 1, false},
    {"ulvl", 5, 1, 60, false},
    {"toht", 5, 1, 10, false},
    {"usmc", 5, 1, 768, false},
    {"skill('attack'.par1)", 5, 1, 9, false},
    {"skill('attack'.blvl)", 5, 1, 1, false},
    {"skill('other'.blvl)", 5, 1, 0, false},
    {"stat('item_x'.accr)", 5, 1, 5, false},
    {"stat('nope'.accr)", 5, 1, 0, false},
    {"sksrc('x'.y)", 5, 1, 0, false},
    {"clc1", 5, 1, 6, false},
    {"clc2", 5, 1, 2, false},
    {"clc3", 5, 1, 1, false},
    {"clc4", 5, 1, 0, false},
    {"ast1", 5, 1, 3, false},
    {"pst1", 5, 1, 7, false},
    {"ps10", 5, 1, 16, false},
    {"ps19", 5, 1, 0, false},
    {"len", 5, 1, 25, false},
    {"edmn", 5, 1, 1, false},
    {"edmx", 5, 1, 10, false},
    {"enma", 5, 1, 1, false},
    {"exma", 5, 1, 10, false},
    {"edns", 5, 1, 256, false},
    {"edln", 5, 1, 37, false},
    {"m1en", 5, 1, 2, false},
    {"m1ex", 5, 1, 4, false},
    {"m2en", 5, 1, 0, false},
    {"miss('arrow'.rang)", 5, 1, 12, false},
    {"miss('arrow'.rad)", 5, 1, 3, false},
    {"miss('arrow'.par1)", 5, 1, 6, false},
    {"miss('arrow'.enma)", 5, 1, 2, false},
    {"miss('nope'.rang)", 5, 1, 0, false},
    {"sklvl('helper'.5.clc1)", 5, 1, 100, false},
    {"sklvl('helper'.lvl.par1)", 5, 1, 4, false},
    {"sklvl('Helper'.blvl)", 5, 1, 1, false},
    {"SKILL('TEST'.LVL)", 5, 1, 1, false},
    {"pinned", 5, 1, 42, false},
    {"bogus", 5, 1, 0, false},
    {"1+", 5, 1, 0, true},
    {"(1", 5, 1, 0, true},
    {"1 2", 5, 1, 0, true},
    {"a.b", 5, 1, 0, true},
    {"skill(", 5, 1, 0, true},
    {"'abc'", 5, 1, 0, true},
    {"1 = 2", 5, 1, 0, true},
    {"#", 5, 1, 0, true},
    {"", 5, 1, 0, false},
    {"   ", 5, 1, 0, false},
    {"lvl*1000000000000*1000000000", 5, 1, 1e+21, false},
    {" 3 *\t4 ", 5, 1, 12, false},
    {"10-lvl*2", 5, 1, 8, false},
    {"(lvl+1)%4", 5, 1, 2, false},
    {"lvl>3&&lvl<10", 5, 1, 0, false},
    {"!lvl||0", 5, 1, 0, false},
    {"7/2", 5, 5, 3, false},
    {"-7/2", 5, 5, -3, false},
    {"7/0", 5, 5, 0, false},
    {"-7%3", 5, 5, -1, false},
    {"7%-3", 5, 5, 1, false},
    {"5%0", 5, 5, 0, false},
    {"1+2*3", 5, 5, 7, false},
    {"(1+2)*3", 5, 5, 9, false},
    {"2-3-4", 5, 5, -5, false},
    {"8/2/2", 5, 5, 2, false},
    {"3>2", 5, 5, 1, false},
    {"2>=3", 5, 5, 0, false},
    {"1==1", 5, 5, 1, false},
    {"1!=1", 5, 5, 0, false},
    {"1<2==1", 5, 5, 1, false},
    {"!0", 5, 5, 1, false},
    {"!5", 5, 5, 0, false},
    {"--3", 5, 5, 3, false},
    {"-+-3", 5, 5, 3, false},
    {"0&&1", 5, 5, 0, false},
    {"2||0", 5, 5, 1, false},
    {"1?2:3", 5, 5, 2, false},
    {"0?2:3", 5, 5, 3, false},
    {"1?0?5:6:7", 5, 5, 6, false},
    {"0?1:0?2:3", 5, 5, 3, false},
    {"-(1>0)?0:(10+lvl/2)", 5, 5, -0.0, false},
    {"-(0)?0:(10+lvl/2)", 5, 5, -12, false},
    {"+(0)?1:lvl", 5, 5, 5, false},
    {"min(3,1,2)", 5, 5, 1, false},
    {"max(1,4)", 5, 5, 4, false},
    {"min()", 5, 5, 0, false},
    {"foo(7,2)", 5, 5, 7, false},
    {"max(-1)", 5, 5, -1, false},
    {"\"min(ln12,30)\"", 5, 5, 30, false},
    {"  \"lvl\"  ", 5, 5, 5, false},
    {"ln12", 5, 5, 31, false},
    {"dm12", 5, 5, 5, false},
    {"par1", 5, 5, 3, false},
    {"par8", 5, 5, 5, false},
    {"par9", 5, 5, 0, false},
    {"blvl", 5, 5, 5, false},
    {"ulvl", 5, 5, 60, false},
    {"toht", 5, 5, 30, false},
    {"usmc", 5, 5, 1536, false},
    {"skill('attack'.par1)", 5, 5, 9, false},
    {"skill('attack'.blvl)", 5, 5, 1, false},
    {"skill('other'.blvl)", 5, 5, 0, false},
    {"stat('item_x'.accr)", 5, 5, 5, false},
    {"stat('nope'.accr)", 5, 5, 0, false},
    {"sksrc('x'.y)", 5, 5, 0, false},
    {"clc1", 5, 5, 6, false},
    {"clc2", 5, 5, 2, false},
    {"clc3", 5, 5, 1, false},
    {"clc4", 5, 5, 0, false},
    {"ast1", 5, 5, 15, false},
    {"pst1", 5, 5, 7, false},
    {"ps10", 5, 5, 16, false},
    {"ps19", 5, 5, 0, false},
    {"len", 5, 5, 125, false},
    {"edmn", 5, 5, 9, false},
    {"edmx", 5, 5, 90, false},
    {"enma", 5, 5, 9, false},
    {"exma", 5, 5, 90, false},
    {"edns", 5, 5, 2304, false},
    {"edln", 5, 5, 67, false},
    {"m1en", 5, 5, 6, false},
    {"m1ex", 5, 5, 8, false},
    {"m2en", 5, 5, 0, false},
    {"miss('arrow'.rang)", 5, 5, 12, false},
    {"miss('arrow'.rad)", 5, 5, 3, false},
    {"miss('arrow'.par1)", 5, 5, 6, false},
    {"miss('arrow'.enma)", 5, 5, 6, false},
    {"miss('nope'.rang)", 5, 5, 0, false},
    {"sklvl('helper'.5.clc1)", 5, 5, 500, false},
    {"sklvl('helper'.lvl.par1)", 5, 5, 4, false},
    {"sklvl('Helper'.blvl)", 5, 5, 5, false},
    {"SKILL('TEST'.LVL)", 5, 5, 5, false},
    {"pinned", 5, 5, 42, false},
    {"bogus", 5, 5, 0, false},
    {"1+", 5, 5, 0, true},
    {"(1", 5, 5, 0, true},
    {"1 2", 5, 5, 0, true},
    {"a.b", 5, 5, 0, true},
    {"skill(", 5, 5, 0, true},
    {"'abc'", 5, 5, 0, true},
    {"1 = 2", 5, 5, 0, true},
    {"#", 5, 5, 0, true},
    {"", 5, 5, 0, false},
    {"   ", 5, 5, 0, false},
    {"lvl*1000000000000*1000000000", 5, 5, 5e+21, false},
    {" 3 *\t4 ", 5, 5, 12, false},
    {"10-lvl*2", 5, 5, 0, false},
    {"(lvl+1)%4", 5, 5, 2, false},
    {"lvl>3&&lvl<10", 5, 5, 1, false},
    {"!lvl||0", 5, 5, 0, false},
    {"7/2", 5, 30, 3, false},
    {"-7/2", 5, 30, -3, false},
    {"7/0", 5, 30, 0, false},
    {"-7%3", 5, 30, -1, false},
    {"7%-3", 5, 30, 1, false},
    {"5%0", 5, 30, 0, false},
    {"1+2*3", 5, 30, 7, false},
    {"(1+2)*3", 5, 30, 9, false},
    {"2-3-4", 5, 30, -5, false},
    {"8/2/2", 5, 30, 2, false},
    {"3>2", 5, 30, 1, false},
    {"2>=3", 5, 30, 0, false},
    {"1==1", 5, 30, 1, false},
    {"1!=1", 5, 30, 0, false},
    {"1<2==1", 5, 30, 1, false},
    {"!0", 5, 30, 1, false},
    {"!5", 5, 30, 0, false},
    {"--3", 5, 30, 3, false},
    {"-+-3", 5, 30, 3, false},
    {"0&&1", 5, 30, 0, false},
    {"2||0", 5, 30, 1, false},
    {"1?2:3", 5, 30, 2, false},
    {"0?2:3", 5, 30, 3, false},
    {"1?0?5:6:7", 5, 30, 6, false},
    {"0?1:0?2:3", 5, 30, 3, false},
    {"-(1>0)?0:(10+lvl/2)", 5, 30, -0.0, false},
    {"-(0)?0:(10+lvl/2)", 5, 30, -25, false},
    {"+(0)?1:lvl", 5, 30, 30, false},
    {"min(3,1,2)", 5, 30, 1, false},
    {"max(1,4)", 5, 30, 4, false},
    {"min()", 5, 30, 0, false},
    {"foo(7,2)", 5, 30, 7, false},
    {"max(-1)", 5, 30, -1, false},
    {"\"min(ln12,30)\"", 5, 30, 30, false},
    {"  \"lvl\"  ", 5, 30, 30, false},
    {"ln12", 5, 30, 206, false},
    {"dm12", 5, 30, 6, false},
    {"par1", 5, 30, 3, false},
    {"par8", 5, 30, 5, false},
    {"par9", 5, 30, 0, false},
    {"blvl", 5, 30, 5, false},
    {"ulvl", 5, 30, 60, false},
    {"toht", 5, 30, 155, false},
    {"usmc", 5, 30, 7936, false},
    {"skill('attack'.par1)", 5, 30, 9, false},
    {"skill('attack'.blvl)", 5, 30, 1, false},
    {"skill('other'.blvl)", 5, 30, 0, false},
    {"stat('item_x'.accr)", 5, 30, 5, false},
    {"stat('nope'.accr)", 5, 30, 0, false},
    {"sksrc('x'.y)", 5, 30, 0, false},
    {"clc1", 5, 30, 6, false},
    {"clc2", 5, 30, 2, false},
    {"clc3", 5, 30, 1, false},
    {"clc4", 5, 30, 0, false},
    {"ast1", 5, 30, 90, false},
    {"pst1", 5, 30, 7, false},
    {"ps10", 5, 30, 16, false},
    {"ps19", 5, 30, 0, false},
    {"len", 5, 30, 750, false},
    {"edmn", 5, 30, 105, false},
    {"edmx", 5, 30, 1050, false},
    {"enma", 5, 30, 105, false},
    {"exma", 5, 30, 1050, false},
    {"edns", 5, 30, 26880, false},
    {"edln", 5, 30, 147, false},
    {"m1en", 5, 30, 9, false},
    {"m1ex", 5, 30, 12, false},
    {"m2en", 5, 30, 0, false},
    {"miss('arrow'.rang)", 5, 30, 12, false},
    {"miss('arrow'.rad)", 5, 30, 3, false},
    {"miss('arrow'.par1)", 5, 30, 6, false},
    {"miss('arrow'.enma)", 5, 30, 9, false},
    {"miss('nope'.rang)", 5, 30, 0, false},
    {"sklvl('helper'.5.clc1)", 5, 30, 3000, false},
    {"sklvl('helper'.lvl.par1)", 5, 30, 4, false},
    {"sklvl('Helper'.blvl)", 5, 30, 20, false},
    {"SKILL('TEST'.LVL)", 5, 30, 30, false},
    {"pinned", 5, 30, 42, false},
    {"bogus", 5, 30, 0, false},
    {"1+", 5, 30, 0, true},
    {"(1", 5, 30, 0, true},
    {"1 2", 5, 30, 0, true},
    {"a.b", 5, 30, 0, true},
    {"skill(", 5, 30, 0, true},
    {"'abc'", 5, 30, 0, true},
    {"1 = 2", 5, 30, 0, true},
    {"#", 5, 30, 0, true},
    {"", 5, 30, 0, false},
    {"   ", 5, 30, 0, false},
    {"lvl*1000000000000*1000000000", 5, 30, 3e+22, false},
    {" 3 *\t4 ", 5, 30, 12, false},
    {"10-lvl*2", 5, 30, -50, false},
    {"(lvl+1)%4", 5, 30, 3, false},
    {"lvl>3&&lvl<10", 5, 30, 0, false},
    {"!lvl||0", 5, 30, 0, false},};

js::Arg N(double v) {
    js::Arg a;
    a.kind = js::Arg::Kind::Number;
    a.number = v;
    return a;
}

js::Arg S(const char* s) {
    js::Arg a;
    a.kind = js::Arg::Kind::String;
    a.text = s;
    return a;
}

struct SprintfCase {
    const char* tmpl;
    std::vector<js::Arg> vals;
    const char* expected;
};

const SprintfCase kSprintfCases[] = {
    {"%d%% x", {N(5)}, "5% x"},
    {"%+d", {N(5)}, "+5"},
    {"%+d", {N(-3)}, "-3"},
    {"%.1f", {N(2.25)}, "2.3"},
    {"%.2f", {N(1.005)}, "1.00"},
    {"%.0f", {N(0.5)}, "1"},
    {"%.0f", {N(-2.5)}, "-3"},
    {"%.3f", {N(-0.0001)}, "-0.000"},
    {"%s", {N(0.3333333333333333)}, "0.3333333333333333"},
    {"%d", {N(0.3333333333333333)}, "0.33"},
    {"%5d", {N(7)}, "7"},
    {"%q %", {N(1)}, "%q %"},
    {"%s %s", {S("a")}, "a "},
    {"%d", {S("12")}, "12"},
    {"%d", {S("abc")}, "0"},
    {"%d", {S(" 0x1F ")}, "31"},
    {"%+d", {N(0)}, "+0"},
    {"%i %f", {N(2.5), N(1e+21)}, "2.5 999999999999999900000"},
    {"%.2f", {N(1e+21)}, "1e+21"},
    {"%s", {N(1e+21)}, "1e+21"},
    {"%s", {N(1e-7)}, "1e-7"},
    {"%s", {N(0.000001)}, "0.000001"},
    {"%s", {N(123456789012345680000.0)}, "123456789012345680000"},
    {"%s", {N(-1.5e+300)}, "-1.5e+300"},
    {"%d", {N(1.005)}, "1"},
    {"%d", {N(-0.125)}, "-0.12"},
    {"%+.1d", {N(3)}, "+3.0"},
    {"%.d", {N(3)}, "%.d"},
    {"%+%", {N(3)}, "%"},};

struct LineCase {
    int line;  // index into the test skill's level lines
    int lvl;
    const char* expected;  // nullptr = hidden
};

// One Calculator (character level 60) for all of them, in this order.
const LineCase kLineCases[] = {
    {0, 1, "Mana Cost: 3"},
    {0, 7, "Mana Cost: 8"},
    {1, 1, "Length: 1 second"},
    {1, 7, "Length: 7 seconds"},
    {2, 1, nullptr},
    {2, 7, nullptr},
    {3, 1, "Resist: +1% x"},
    {3, 7, "Resist: +7% x"},
    {4, 1, "Attack Rating: 1%"},
    {4, 7, "Attack Rating: 7%"},
    {5, 1, "Bonus: -1"},
    {5, 7, "Bonus: -7"},
    {6, 1, "Line two\nLine one"},
    {6, 7, "Line two\nLine one"},
    {7, 1, "1% chance\n1-10 damage"},
    {7, 7, "7% chance\n13-130 damage"},
    {8, 1, nullptr},
    {8, 7, nullptr},
    {9, 1, "1|2|3"},
    {9, 7, "1|2|3"},
    {10, 1, "Test Receives Bonuses From:"},
    {10, 7, "Test Receives Bonuses From:"},
    {11, 1, "Helper: +5% Damage per Level"},
    {11, 7, "Helper: +5% Damage per Level"},
    {12, 1, "Helper: 0.33 per second"},
    {12, 7, "Helper: 2.33 per second"},
    {13, 1, "+1% to Attack"},
    {13, 7, "+7% to Attack"},
    {14, 1, "Speed -1%"},
    {14, 7, "Speed -7%"},
    {15, 1, "Life: +1 points"},
    {15, 7, "Life: +7 points"},
    {16, 1, "A B"},
    {16, 7, "A B"},
    {17, 1, "1 spaced"},
    {17, 7, "7 spaced"},
    {18, 1, "x100%"},
    {18, 7, "x100%"},
    {19, 1, nullptr},
    {19, 7, nullptr},
    {20, 1, "0.5%"},
    {20, 7, "3.5%"},};

void UnitTests() {
    Data data;
    std::string error;
    if (!Parse(kUnitJson, data, error)) {
        Fail("unit data: " + error);
        return;
    }
    const Skill* test = data.FindByKey("test");
    const Skill* helper = data.FindById("tst-helper");
    if (test == nullptr || helper == nullptr || test->record == nullptr) {
        Fail("unit data: lookups");
        return;
    }

    // Expressions
    std::size_t i = 0;
    const std::size_t count = sizeof(kExprCases) / sizeof(kExprCases[0]);
    while (i < count) {
        CalcOptions options;
        options.characterLevel = 60;
        if (kExprCases[i].hardPoints >= 0) options.hardPoints = kExprCases[i].hardPoints;
        Calculator calc(data, options);
        const int hp = kExprCases[i].hardPoints;
        const int lvl = kExprCases[i].lvl;
        for (; i < count && kExprCases[i].hardPoints == hp && kExprCases[i].lvl == lvl; ++i) {
            const ExprCase& c = kExprCases[i];
            Formula f;
            f.kind = Formula::Kind::Text;
            f.text = c.formula;
            const double got = calc.Run(*test->record, f, lvl);
            if (!(got == c.expected) || std::signbit(got) != std::signbit(c.expected)) {
                Fail(std::string("expr '") + c.formula + "' hp " + std::to_string(hp) + " lvl " + std::to_string(lvl) +
                     ": expected " + js::NumberToString(c.expected) + ", got " + js::NumberToString(got));
            }
            const bool isError = !f.text.empty() && Compiled(f) != nullptr && Compiled(f)->op == calc::Op::Error;
            const bool blank = f.text.find_first_not_of(" \t") == std::string::npos;
            if (!blank && isError != c.parseError) Fail(std::string("expr '") + c.formula + "': parse error flag");
        }
    }

    for (const SprintfCase& c : kSprintfCases) {
        const std::string got = js::Sprintf(c.tmpl, c.vals);
        if (got != c.expected) Fail(std::string("sprintf '") + c.tmpl + "': expected '" + c.expected + "', got '" + got + "'");
    }

    {
        CalcOptions options;
        options.characterLevel = 60;
        Calculator calc(data, options);
        for (const LineCase& c : kLineCases) {
            const Line& line = test->levelLines[static_cast<std::size_t>(c.line)];
            const std::optional<std::string> got = calc.FormatLine(*test->record, line, c.lvl);
            const std::optional<std::string> want =
                c.expected == nullptr ? std::nullopt : std::optional<std::string>(c.expected);
            if (got != want) {
                Fail("line " + std::to_string(c.line) + " lvl " + std::to_string(c.lvl) + ": expected " + Show(want) +
                     ", got " + Show(got));
            }
        }
    }

    // Number formatting
    const struct {
        double v;
        const char* s;
    } numbers[] = {{0, "0"},           {-0.0, "0"},          {1, "1"},           {-1.5, "-1.5"},
                   {0.1 + 0.2, "0.30000000000000004"},     {1e21, "1e+21"},    {1e20, "100000000000000000000"},
                   {1.5e-7, "1.5e-7"}, {0.000001, "0.000001"}, {123.456, "123.456"}, {5e-324, "5e-324"}};
    for (const auto& n : numbers) {
        if (js::NumberToString(n.v) != n.s) Fail(std::string("NumberToString ") + n.s + ": got " + js::NumberToString(n.v));
    }
    if (js::ToFixed(2.5, 0) != "3" || js::ToFixed(-2.5, 0) != "-3" || js::ToFixed(1.005, 2) != "1.00" ||
        js::ToFixed(0.000001, 7) != "0.0000010" || js::ToFixed(123.456, 1) != "123.5") {
        Fail("ToFixed");
    }
    if (js::ToLower("Fire BALL") != "fire ball" || js::ToLower("\xC3\x89T\xC3\x89") != "\xC3\xA9t\xC3\xA9" ||
        js::ToLower("\xD0\x9C\xD0\x95\xD0\xA7") != "\xD0\xBC\xD0\xB5\xD1\x87") {
        Fail("ToLower");
    }

    // Compile
    if (Compile("1+")->op != calc::Op::Error || Compile("min(1,2)")->op != calc::Op::Call ||
        Compile("-(a)?1:2")->op != calc::Op::Neg || Compile("skill('X Y'.blvl)")->name != "x y") {
        Fail("Compile shapes");
    }

    // Data helpers
    if (test->requiredLevel != 6 || data.RequiredLevelWithPrerequisites(*test) != 6) Fail("required level");
    const std::vector<const Skill*> pre = data.Prerequisites(*test);
    if (pre.size() != 1 || pre[0] != helper) Fail("prerequisites");
    if (data.SkillsOfTree("tst", 1).size() != 2 || data.SkillsOfClass("tst").size() != 3) Fail("class / tree lists");
    if (test->searchText.find("Tester") == std::string::npos || test->searchText.find("Mana Cost: ") == std::string::npos) {
        Fail("search text: " + test->searchText);
    }
    if (TemplateText(test->levelLines[0]) != "Mana Cost:  ") Fail("TemplateText: '" + TemplateText(test->levelLines[0]) + "'");

    // Synergy points reach other skills' levels; ids that are not the skill's synergies are ignored.
    Context ctx;
    ctx.skill = test;
    ctx.skillLevel = 5;
    ctx.characterLevel = 60;
    Formula esym;
    esym.kind = Formula::Kind::Text;
    esym.text = "skill('helper'.blvl)*10";
    if (Evaluate(data, *Compile(esym.text), ctx) != 0) Fail("synergy default");
    ctx.synergyPoints["tst-helper"] = 7;
    if (Evaluate(data, *Compile(esym.text), ctx) != 0) Fail("synergy ignored when not listed");
    data.skills[0].synergies.push_back("tst-helper");
    if (Evaluate(data, *Compile(esym.text), ctx) != 70) Fail("synergy points");
    ctx.synergyPoints["tst-helper"] = 99;
    if (Evaluate(data, *Compile(esym.text), ctx) != 200) Fail("synergy cap");

    // Malformed JSON is reported, not guessed at.
    Data bad;
    if (Parse("{\"skills\": [", bad, error) || error.empty()) Fail("bad json accepted");
}

// ---------------------------------------------------------------------------------------------------------------
// Parity with the JavaScript
// ---------------------------------------------------------------------------------------------------------------

const char* const kLangs[] = {"enUS", "deDE", "esES", "esMX", "frFR", "itIT", "jaJP",
                              "koKR", "plPL", "ptBR", "ruRU", "zhCN", "zhTW"};
const int kSlvl[] = {1, 5, 10, 20, 21, 30, 50, 110};
const int kClvl[] = {1, 30, 60, 99, 110};
const int kHp[] = {-1, 0, 1, 20};
constexpr std::size_t kContexts = 4 * 5 * 8;

struct Expected {
    std::vector<std::optional<std::string>> distinct;
    std::vector<int> index;  // per context
};

std::string Unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i] == 'n' ? '\n' : s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

bool ReadFixture(const std::string& path, std::map<std::string, Expected>& out) {
    std::string text;
    if (!ReadFile(path, text)) return false;
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.empty() || l[0] != '@') continue;
        const std::size_t t1 = l.find('\t');
        const std::size_t t2 = l.find('\t', t1 + 1);
        const std::string key = l.substr(1, t2 - 1);
        const int n = std::stoi(l.substr(t2 + 1));
        Expected e;
        for (int k = 0; k < n; ++k) {
            const std::string& v = lines[++i];
            if (v == "~") e.distinct.emplace_back(std::nullopt);
            else e.distinct.emplace_back(Unescape(v.substr(1)));
        }
        std::istringstream idx(lines[++i]);
        std::string tok;
        while (idx >> tok) {
            const std::size_t star = tok.find('*');
            const int value = std::stoi(tok.substr(0, star));
            const int times = star == std::string::npos ? 1 : std::stoi(tok.substr(star + 1));
            for (int k = 0; k < times; ++k) e.index.push_back(value);
        }
        out[key] = std::move(e);
    }
    return true;
}

const char* const kGroups[] = {"g", "l", "s"};

const std::vector<Line>& Group(const Skill& s, int g) {
    return g == 0 ? s.general : g == 1 ? s.levelLines : s.synergyLines;
}

int g_reported = 0;

void Report(const std::string& lang, const std::string& where, const std::optional<std::string>& want,
            const std::optional<std::string>& got) {
    ++g_reported;
    if (g_reported <= 20) {
        std::printf("MISMATCH %s %s\n  expected %s\n  got      %s\n", lang.c_str(), where.c_str(), Show(want).c_str(),
                    Show(got).c_str());
    }
}

void Parity(const std::string& dataDir, const std::string& fixtureDir) {
    std::size_t allCompared = 0;
    std::size_t allMismatches = 0;
    for (const char* lang : kLangs) {
        const std::string file = std::string(lang) == "enUS" ? dataDir + "/skills-data.json"
                                                             : dataDir + "/locales/" + lang + "/skills-data.json";
        std::string json;
        if (!ReadFile(file, json)) {
            Fail(std::string("cannot read ") + file);
            continue;
        }
        Data data;
        std::string error;
        const auto t0 = std::chrono::steady_clock::now();
        if (!Parse(json, data, error)) {
            Fail(std::string(lang) + ": " + error);
            continue;
        }
        const double loadMs = Ms(std::chrono::steady_clock::now() - t0);
        std::map<std::string, Expected> fixture;
        if (!ReadFixture(fixtureDir + "/" + lang + ".txt", fixture)) {
            Fail(std::string("cannot read fixture for ") + lang);
            continue;
        }

        // results[skill][group][line][context], computed in the fixture's order
        std::vector<std::vector<std::vector<std::vector<std::optional<std::string>>>>> results(data.skills.size());
        for (std::size_t si = 0; si < data.skills.size(); ++si) {
            results[si].resize(3);
            for (int g = 0; g < 3; ++g) results[si][static_cast<std::size_t>(g)].resize(Group(data.skills[si], g).size());
        }
        const auto t1 = std::chrono::steady_clock::now();
        for (const int hp : kHp) {
            for (const int clvl : kClvl) {
                CalcOptions options;
                options.characterLevel = clvl;
                if (hp >= 0) options.hardPoints = hp;
                Calculator calc(data, options);
                for (std::size_t si = 0; si < data.skills.size(); ++si) {
                    const Skill& s = data.skills[si];
                    for (const int slvl : kSlvl) {
                        for (int g = 0; g < 3; ++g) {
                            const std::vector<Line>& lines = Group(s, g);
                            for (std::size_t li = 0; li < lines.size(); ++li) {
                                results[si][static_cast<std::size_t>(g)][li].push_back(calc.FormatLine(*s.record, lines[li], slvl));
                            }
                        }
                    }
                }
            }
        }
        const double gridMs = Ms(std::chrono::steady_clock::now() - t1);

        std::size_t compared = 0;
        std::size_t mismatches = 0;
        std::size_t lineCount = 0;
        for (std::size_t si = 0; si < data.skills.size(); ++si) {
            const Skill& s = data.skills[si];
            for (int g = 0; g < 3; ++g) {
                const std::vector<Line>& lines = Group(s, g);
                for (std::size_t li = 0; li < lines.size(); ++li) {
                    ++lineCount;
                    const std::string name = std::string(kGroups[g]) + std::to_string(li);
                    const auto it = fixture.find(s.id + "\t" + name);
                    if (it == fixture.end() || it->second.index.size() != kContexts) {
                        ++mismatches;
                        Report(lang, s.id + " " + name + ": not in the fixture", std::nullopt, std::nullopt);
                        continue;
                    }
                    const auto& got = results[si][static_cast<std::size_t>(g)][li];
                    for (std::size_t c = 0; c < kContexts; ++c) {
                        ++compared;
                        const std::optional<std::string>& want = it->second.distinct[static_cast<std::size_t>(it->second.index[c])];
                        if (got[c] != want) {
                            ++mismatches;
                            const std::size_t hpi = c / 40, ci = (c / 8) % 5, sli = c % 8;
                            Report(lang, s.id + " " + name + " (hp " + (kHp[hpi] < 0 ? std::string("default") : std::to_string(kHp[hpi])) +
                                             ", clvl " + std::to_string(kClvl[ci]) + ", slvl " + std::to_string(kSlvl[sli]) + ")",
                                   want, got[c]);
                        }
                    }
                }
            }
            // The synergy context: every synergy and level link at 20, skill level 20, character level 110.
            if (s.synergies.empty() && s.levelLinks.empty()) continue;
            Context ctx;
            ctx.skill = &s;
            ctx.skillLevel = 20;
            ctx.characterLevel = 110;
            for (const std::string& id : s.synergies) ctx.synergyPoints[id] = 20;
            for (const std::string& id : s.levelLinks) ctx.synergyPoints[id] = 20;
            Calculator calc(data, OptionsFor(data, ctx));
            for (int g = 0; g < 3; ++g) {
                const std::vector<Line>& lines = Group(s, g);
                for (std::size_t li = 0; li < lines.size(); ++li) {
                    const std::string name = std::string("syn:") + kGroups[g] + std::to_string(li);
                    const auto it = fixture.find(s.id + "\t" + name);
                    const std::optional<std::string> got = calc.FormatLine(*s.record, lines[li], 20);
                    ++compared;
                    if (it == fixture.end() || it->second.distinct.size() != 1 || got != it->second.distinct[0]) {
                        ++mismatches;
                        Report(lang, s.id + " " + name, it == fixture.end() ? std::nullopt : it->second.distinct[0], got);
                    }
                }
            }
        }
        std::printf("%s: %zu skills, %zu lines, %zu results compared, %zu mismatches (load %.1f ms, grid %.0f ms)\n", lang,
                    data.skills.size(), lineCount, compared, mismatches, loadMs, gridMs);
        allCompared += compared;
        allMismatches += mismatches;
    }
    std::printf("parity: %zu results compared, %zu mismatches\n", allCompared, allMismatches);
    if (allMismatches != 0) Fail("parity mismatches: " + std::to_string(allMismatches));
}

// Every displayed line of every skill for one context.
void Timing(const std::string& dataDir) {
    std::string json;
    if (!ReadFile(dataDir + "/skills-data.json", json)) return;
    Data data;
    std::string error;
    if (!Parse(json, data, error)) return;
    Context ctx;
    ctx.skillLevel = 20;
    ctx.characterLevel = 110;
    std::size_t shown = 0;
    double cold = 0;
    double warm = 0;
    for (int pass = 0; pass < 6; ++pass) {
        Engine engine(data);  // a fresh Calculator (empty memo) each pass; formulas stay compiled after pass 0
        ctx.characterLevel = pass % 2 == 0 ? 110 : 99;
        const auto t0 = std::chrono::steady_clock::now();
        shown = 0;
        for (const Skill& s : data.skills) {
            ctx.skill = &s;
            const CardLines card = engine.Card(ctx);
            shown += card.general.size() + card.current.size() + card.synergies.size();
        }
        const double ms = Ms(std::chrono::steady_clock::now() - t0);
        if (pass == 0) cold = ms;
        else if (pass == 1 || ms < warm) warm = ms;
    }
    std::printf("timing: all %zu skills, %zu shown lines, one context: %.2f ms first time (compiles), %.2f ms after\n",
                data.skills.size(), shown, cold, warm);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: skill_calc_test <data dir> <fixture dir>\n");
        return 2;
    }
    UnitTests();
    std::printf("unit tests: %d failures\n", g_failures);
    Parity(argv[1], argv[2]);
    Timing(argv[1]);
    if (g_failures != 0) {
        std::printf("%d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("all skill calc tests passed\n");
    return 0;
}
