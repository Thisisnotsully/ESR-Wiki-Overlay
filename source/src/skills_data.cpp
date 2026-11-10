#include "skills_data.hpp"

#include <algorithm>
#include <utility>

#include "json_lite.hpp"

namespace esrwiki::skills {

namespace {

using json::Value;

std::string Str(const Value& v) { return v.kind == Value::Kind::String ? v.text : std::string(); }

double Num(const Value& v, double fallback = 0) {
    if (v.kind == Value::Kind::Number) return v.number;
    if (v.kind == Value::Kind::Bool) return v.boolean ? 1 : 0;
    return fallback;
}

int Int(const Value& v, int fallback = 0) { return static_cast<int>(Num(v, fallback)); }

bool Truthy(const Value& v) {
    switch (v.kind) {
        case Value::Kind::Bool: return v.boolean;
        case Value::Kind::Number: return v.number != 0;
        case Value::Kind::String: return !v.text.empty();
        case Value::Kind::Array:
        case Value::Kind::Object: return true;
        default: return false;
    }
}

std::vector<double> Numbers(const Value& v) {
    std::vector<double> out;
    if (v.kind != Value::Kind::Array) return out;
    out.reserve(v.items.size());
    for (const Value& item : v.items) out.push_back(Num(item));
    return out;
}

std::vector<std::string> Strings(const Value& v) {
    std::vector<std::string> out;
    if (v.kind != Value::Kind::Array) return out;
    out.reserve(v.items.size());
    for (const Value& item : v.items) out.push_back(Str(item));
    return out;
}

Formula ToFormula(const Value& v) {
    Formula f;
    if (v.kind == Value::Kind::Number) {
        f.kind = Formula::Kind::Number;
        f.number = v.number;
    } else if (v.kind == Value::Kind::String) {
        f.kind = Formula::Kind::Text;
        f.text = v.text;
    }
    return f;
}

std::vector<Formula> Formulas(const Value& v) {
    std::vector<Formula> out;
    if (v.kind != Value::Kind::Array) return out;
    out.reserve(v.items.size());
    for (const Value& item : v.items) out.push_back(ToFormula(item));
    return out;
}

// Calls fn(key, value) for every member of an object.
template <typename Fn>
void Members(const Value& object, Fn&& fn) {
    if (object.kind != Value::Kind::Object) return;
    for (std::size_t i = 0; i < object.keys.size(); ++i) fn(object.keys[i], object.values[i]);
}

Record ToRecord(const Value& v) {
    Record r;
    Members(v, [&r](const std::string& key, const Value& value) {
        if (key == "k") r.k = Str(value);
        else if (key == "mx") r.mx = Num(value);
        else if (key == "p") r.p = Numbers(value);
        else if (key == "c") r.c = Formulas(value);
        else if (key == "mn") r.mn = Numbers(value);
        else if (key == "th") r.th = Numbers(value);
        else if (key == "hs") {
            if (value.kind == Value::Kind::Number) {
                r.hasHs = true;
                r.hs = value.number;
            }
        } else if (key == "ed") r.ed = Numbers(value);
        else if (key == "el") r.el = Numbers(value);
        else if (key == "pd") r.pd = Numbers(value);
        else if (key == "esym") r.esym = ToFormula(value);
        else if (key == "lsym") r.lsym = ToFormula(value);
        else if (key == "psym") r.psym = ToFormula(value);
        else if (key == "as") r.as = Formulas(value);
        else if (key == "ps") r.ps = Formulas(value);
        else if (key == "rng") r.rng = ToFormula(value);
        else if (key == "len") r.len = ToFormula(value);
        else if (key == "pet") r.pet = ToFormula(value);
        else if (key == "ms") r.ms = Strings(value);
        else if (key == "r") r.r = Num(value);
        else if (key == "rad") r.rad = Num(value);
    });
    return r;
}

std::vector<Line> ToLines(const Value& v) {
    std::vector<Line> out;
    if (v.kind != Value::Kind::Array) return out;
    out.reserve(v.items.size());
    for (const Value& item : v.items) {
        Line line;
        Members(item, [&line](const std::string& key, const Value& value) {
            if (key == "t") line.t = Int(value);
            else if (key == "a") line.a = Str(value);
            else if (key == "b") line.b = Str(value);
            else if (key == "x") line.x = ToFormula(value);
            else if (key == "y") line.y = ToFormula(value);
            else if (key == "z") line.z = ToFormula(value);
        });
        out.push_back(std::move(line));
    }
    return out;
}

void AppendWord(std::string& out, const std::string& text) {
    if (text.empty()) return;
    if (!out.empty()) out += ' ';
    out += text;
}

}  // namespace

std::string TemplateText(const Line& line) {
    std::string joined;
    if (!line.a.empty()) joined = line.a;
    if (!line.b.empty()) {
        if (!joined.empty()) joined += ' ';
        joined += line.b;
    }
    // .replace(/%\+?\d*(?:\.\d+)?[dsif]/g, ' ').replace(/%%/g, '%')
    std::string step;
    step.reserve(joined.size());
    for (std::size_t i = 0; i < joined.size();) {
        if (joined[i] == '%') {
            std::size_t j = i + 1;
            if (j < joined.size() && joined[j] == '+') ++j;
            while (j < joined.size() && joined[j] >= '0' && joined[j] <= '9') ++j;
            if (j + 1 < joined.size() && joined[j] == '.' && joined[j + 1] >= '0' && joined[j + 1] <= '9') {
                ++j;
                while (j < joined.size() && joined[j] >= '0' && joined[j] <= '9') ++j;
            }
            if (j < joined.size() && (joined[j] == 'd' || joined[j] == 's' || joined[j] == 'i' || joined[j] == 'f')) {
                step += ' ';
                i = j + 1;
                continue;
            }
        }
        step += joined[i++];
    }
    std::string out;
    out.reserve(step.size());
    for (std::size_t i = 0; i < step.size(); ++i) {
        out += step[i];
        if (step[i] == '%' && i + 1 < step.size() && step[i + 1] == '%') ++i;
    }
    return out;
}

const Record* Db::Skill(const std::string& key) const {
    const auto it = s.find(key);
    return it == s.end() ? nullptr : &it->second;
}

const Record* Db::Missile(const std::string& key) const {
    const auto it = m.find(key);
    return it == m.end() ? nullptr : &it->second;
}

const Skill* Data::FindByKey(const std::string& key) const {
    const auto it = byKey.find(key);
    return it == byKey.end() ? nullptr : &skills[it->second];
}

const Skill* Data::FindById(const std::string& id) const {
    const auto it = byId.find(id);
    return it == byId.end() ? nullptr : &skills[it->second];
}

const Class* Data::FindClass(const std::string& classId) const {
    for (const Class& c : classes) {
        if (c.id == classId) return &c;
    }
    return nullptr;
}

const Tree* Data::FindTree(const std::string& classId, int treeN) const {
    const Class* c = FindClass(classId);
    if (c == nullptr) return nullptr;
    for (const Tree& t : c->trees) {
        if (t.n == treeN) return &t;
    }
    return nullptr;
}

std::vector<const Skill*> Data::SkillsOfClass(const std::string& classId) const {
    std::vector<const Skill*> out;
    for (const Skill& s : skills) {
        if (s.cls == classId) out.push_back(&s);
    }
    return out;
}

std::vector<const Skill*> Data::SkillsOfTree(const std::string& classId, int treeN) const {
    std::vector<const Skill*> out;
    for (const Skill& s : skills) {
        if (s.cls == classId && s.tree == treeN) out.push_back(&s);
    }
    return out;
}

std::vector<const Skill*> Data::Prerequisites(const Skill& skill) const {
    std::vector<const Skill*> out;
    for (const std::string& name : skill.prerequisites) {
        const Skill* found = nullptr;
        for (const Skill& s : skills) {
            if (s.name == name && s.cls == skill.cls) {
                found = &s;
                break;
            }
        }
        if (found == nullptr) {
            for (const Skill& s : skills) {
                if (s.name == name) {
                    found = &s;
                    break;
                }
            }
        }
        if (found != nullptr) out.push_back(found);
    }
    return out;
}

std::vector<const Skill*> Data::AllPrerequisites(const Skill& skill) const {
    std::vector<const Skill*> out;
    std::vector<const Skill*> queue = Prerequisites(skill);
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const Skill* s = queue[i];
        if (s == &skill || std::find(out.begin(), out.end(), s) != out.end()) continue;
        out.push_back(s);
        for (const Skill* next : Prerequisites(*s)) queue.push_back(next);
    }
    return out;
}

int Data::RequiredLevelWithPrerequisites(const Skill& skill) const {
    int level = skill.requiredLevel;
    for (const Skill* s : AllPrerequisites(skill)) level = std::max(level, s->requiredLevel);
    return level;
}

bool Parse(const std::string& jsonText, Data& out, std::string& error) {
    out = Data();
    Data data;
    {
        Value root;
        if (!json::Parse(jsonText, root, error)) {
            error = "skills data: " + error;
            return false;
        }
        if (root.kind != Value::Kind::Object) {
            error = "skills data: not an object";
            return false;
        }
        data.source = Str(root.Field("source"));
        data.lang = Str(root.Field("lang"));
        data.notes = Strings(root.Field("notes"));
        const Value& levels = root.Field("levels");
        data.levels.max = Int(levels.Field("max"), 110);
        data.levels.skill = Int(levels.Field("skill"), 20);
        data.levels.character = Int(levels.Field("character"), data.levels.max);
        data.levels.synergy = Int(levels.Field("synergy"), 20);
        if (data.levels.synergy <= 0) data.levels.synergy = 20;
        const Value& points = root.Field("points");
        data.questPoints = Int(points.Field("quest"));
        data.difficulties = Int(points.Field("difficulties"));

        for (const Value& cv : root.Field("classes").items) {
            Class c;
            c.id = Str(cv.Field("id"));
            c.title = Str(cv.Field("title"));
            c.perLevel = Int(cv.Field("perLevel"));
            c.intro = Str(cv.Field("intro"));
            for (const Value& tv : cv.Field("trees").items) {
                Tree t;
                t.id = Str(tv.Field("id"));
                t.n = Int(tv.Field("n"));
                t.title = Str(tv.Field("title"));
                c.trees.push_back(std::move(t));
            }
            data.classes.push_back(std::move(c));
        }

        const Value& db = root.Field("db");
        Members(db.Field("s"), [&data](const std::string& key, const Value& value) { data.db.s.emplace(key, ToRecord(value)); });
        Members(db.Field("m"), [&data](const std::string& key, const Value& value) { data.db.m.emplace(key, ToRecord(value)); });
        for (const std::string& b : Strings(db.Field("base"))) data.db.base.insert(b);
        Members(db.Field("stats"), [&data](const std::string& key, const Value& value) { data.db.stats[key] = Num(value); });
        Members(db.Field("aliases"), [&data](const std::string& key, const Value& value) {
            Alias a;
            if (value.kind == Value::Kind::Number) {
                a.isNumber = true;
                a.number = value.number;
            } else {
                a.name = Str(value);
            }
            data.db.aliases[key] = a;
        });

        const Value& skills = root.Field("skills");
        data.skills.reserve(skills.items.size());
        for (const Value& sv : skills.items) {
            Skill s;
            Members(sv, [&s](const std::string& key, const Value& value) {
                if (key == "id") s.id = Str(value);
                else if (key == "n") s.name = Str(value);
                else if (key == "c") s.cls = Str(value);
                else if (key == "k") s.key = Str(value);
                else if (key == "tr") s.tree = Int(value);
                else if (key == "row") s.row = Int(value);
                else if (key == "col") s.col = Int(value);
                else if (key == "r") s.requiredLevel = Int(value);
                else if (key == "pre") s.prerequisites = Strings(value);
                else if (key == "os") s.onStriking = value.kind == Value::Kind::Null ? -1 : (Truthy(value) ? 1 : 0);
                else if (key == "osr") s.onStrikingReason = Str(value);
                else if (key == "oc") s.onCast = value.kind == Value::Kind::Null ? -1 : (Truthy(value) ? 1 : 0);
                else if (key == "ocr") s.onCastReason = Str(value);
                else if (key == "d") s.description = Str(value);
                else if (key == "g") s.general = ToLines(value);
                else if (key == "l") s.levelLines = ToLines(value);
                else if (key == "s") s.synergyLines = ToLines(value);
                else if (key == "src") {
                    for (const Value& x : value.items) {
                        Source src;
                        src.k = Str(x.Field("k"));
                        src.n = Str(x.Field("n"));
                        src.v = Str(x.Field("v"));
                        src.c = Int(x.Field("c"));
                        s.sources.push_back(std::move(src));
                    }
                } else if (key == "sy") s.synergies = Strings(value);
                else if (key == "sl") s.levelLinks = Strings(value);
                else if (key == "pas") s.passive = Truthy(value);
                else if (key == "aur") s.aura = Truthy(value);
                else if (key == "hp") s.hardPoints = Truthy(value);
            });
            data.skills.push_back(std::move(s));
        }
    }  // the JSON document is freed here

    for (std::size_t i = 0; i < data.skills.size(); ++i) {
        Skill& s = data.skills[i];
        if (s.key.empty() || s.id.empty()) {
            error = "skills data: skill " + std::to_string(i) + " has no id or key";
            return false;
        }
        if (!data.byKey.emplace(s.key, i).second) {
            error = "skills data: duplicate skill key '" + s.key + "'";
            return false;
        }
        if (!data.byId.emplace(s.id, i).second) {
            error = "skills data: duplicate skill id '" + s.id + "'";
            return false;
        }
        s.record = data.db.Skill(s.key);
        if (s.record == nullptr) {
            error = "skills data: no db.s record for '" + s.key + "'";
            return false;
        }
    }

    for (Skill& s : data.skills) {
        std::string text;
        AppendWord(text, s.name);
        const Class* c = data.FindClass(s.cls);
        if (c != nullptr) AppendWord(text, c->title);
        const Tree* t = data.FindTree(s.cls, s.tree);
        if (t != nullptr) AppendWord(text, t->title);
        AppendWord(text, s.description);
        for (const std::string& p : s.prerequisites) AppendWord(text, p);
        for (const std::vector<Line>* group : {&s.general, &s.levelLines, &s.synergyLines}) {
            for (const Line& line : *group) AppendWord(text, TemplateText(line));
        }
        for (const Source& src : s.sources) {
            AppendWord(text, src.k);
            AppendWord(text, src.n);
            AppendWord(text, src.v);
        }
        s.searchText = std::move(text);
    }

    out = std::move(data);
    return true;
}

}  // namespace esrwiki::skills
