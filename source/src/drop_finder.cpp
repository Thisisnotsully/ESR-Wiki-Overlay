#include "drop_finder.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>

#include "json_lite.hpp"

namespace esrwiki::drop {

namespace {

using json::Value;

const Value& Missing() {
    static const Value none;
    return none;
}

const Value& At(const Value& v, std::size_t i) {
    return (v.kind == Value::Kind::Array && i < v.items.size()) ? v.items[i] : Missing();
}

bool ReadInt(const Value& v, int& out) {
    if (v.kind != Value::Kind::Number) return false;
    out = static_cast<int>(v.number);
    return true;
}

template <std::size_t N>
bool ReadInts(const Value& v, std::array<int, N>& out) {
    if (v.kind != Value::Kind::Array || v.items.size() < N) return false;
    for (std::size_t i = 0; i < N; ++i) {
        if (!ReadInt(v.items[i], out[i])) return false;
    }
    return true;
}

bool ReadIntList(const Value& v, std::vector<int>& out) {
    if (v.kind != Value::Kind::Array) return false;
    out.clear();
    for (const Value& e : v.items) {
        int x = 0;
        if (!ReadInt(e, x)) return false;
        out.push_back(x);
    }
    return true;
}

// Td: the page's td(), a truncating integer division that is 0 when the divisor is 0.
double Td(double a, double b) {
    return b ? std::trunc(a / b) : 0.0;
}

void AddTo(OutMap& out, const std::string& k, double p) {
    if (p > 0) out[k] += p;
}

// noDrop: the scaled no-drop share for n players, 0x440AF0..0x440B44 in the game.
double NoDrop(double nd, double total, int n) {
    if (!nd) return 0;
    if (n <= 1) return nd;
    double v = nd / (nd + total);
    for (int i = n - 1; i > 0; --i) v = v * (nd / (nd + total));
    const double inv = 1.0 - v;
    if (inv == 0) return 0;
    return std::trunc(((1.0 - inv) * total) / inv);
}

std::string ModsKey(const std::array<int, 4>& m) {
    return std::to_string(m[0]) + "," + std::to_string(m[1]) + "," + std::to_string(m[2]) + "," + std::to_string(m[3]);
}

std::array<int, 4> MergeMods(const std::array<int, 4>& p, const std::array<int, 4>& own) {
    std::array<int, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) out[i] = p[i] ? std::max(p[i], own[i]) : own[i];
    return out;
}

const OutMap& GoldMap() {
    static const OutMap gold{{"g", 1.0}};
    return gold;
}

}  // namespace

bool LoadData(const std::string& text, Data& out, std::string& error) {
    Value root;
    if (!json::Parse(text, root, error)) return false;
    Data d;
    if (!ReadInt(root.Field("txtCount"), d.txtCount) || !ReadInt(root.Field("goldItem"), d.goldItem)) {
        error = "txtCount or goldItem missing";
        return false;
    }

    for (const Value& t : root.Field("types").items) {
        Type type;
        if (!ReadInt(At(t, 2), type.flags)) {
            error = "types row malformed";
            return false;
        }
        d.types.push_back(type);
    }

    for (const Value& it : root.Field("items").items) {
        Item item;
        if (!ReadInt(At(it, 0), item.type) || !ReadInt(At(it, 2), item.ilvl) || !ReadInt(At(it, 3), item.flags) ||
            !ReadInt(At(it, 4), item.ratio)) {
            error = "items row malformed";
            return false;
        }
        d.items.push_back(item);
    }

    for (const Value& r : root.Field("ratios").items) {
        std::array<int, 16> row{};
        if (!ReadInts(r, row)) {
            error = "ratios row malformed";
            return false;
        }
        d.ratios.push_back(row);
    }

    for (const Value& u : root.Field("uniques").items) {
        Unique unique;
        if (!ReadInt(At(u, 0), unique.item) || !ReadInt(At(u, 1), unique.ilvl) || !ReadInt(At(u, 2), unique.weight) ||
            !ReadInt(At(u, 3), unique.flags)) {
            error = "uniques row malformed";
            return false;
        }
        d.uniques.push_back(unique);
    }

    for (const Value& s : root.Field("sets").items) {
        Set set;
        if (!ReadInt(At(s, 0), set.item) || !ReadInt(At(s, 1), set.ilvl) || !ReadInt(At(s, 2), set.weight) ||
            !ReadInt(At(s, 3), set.cow)) {
            error = "sets row malformed";
            return false;
        }
        d.sets.push_back(set);
    }

    for (const Value& t : root.Field("tcs").items) {
        TreasureClass tc;
        if (!ReadInt(At(t, 0), tc.group) || !ReadInt(At(t, 1), tc.level) || !ReadInt(At(t, 2), tc.picks) ||
            !ReadInt(At(t, 3), tc.noDrop) || !ReadInts(At(t, 4), tc.mods)) {
            error = "tcs row malformed";
            return false;
        }
        for (const Value& e : At(t, 5).items) {
            std::array<int, 3> entry{};
            if (!ReadInts(e, entry)) {
                error = "tcs entry malformed";
                return false;
            }
            tc.entries.push_back(entry);
        }
        d.tcs.push_back(tc);
    }

    for (const Value& m : root.Field("mons").items) {
        Monster mon;
        if (!ReadInt(At(m, 0), mon.code) || !ReadInt(At(m, 1), mon.flags) || !ReadInts(At(m, 2), mon.base)) {
            error = "mons row malformed";
            return false;
        }
        const Value& per = At(m, 3);
        for (std::size_t dd = 0; dd < 3; ++dd) {
            if (!ReadInts(At(per, dd), mon.tc[dd])) {
                error = "mons drop row malformed";
                return false;
            }
        }
        mon.listed = At(m, 5).number != 0;
        d.mons.push_back(mon);
    }

    for (const Value& s : root.Field("supers").items) {
        Superunique sup;
        if (!ReadInt(At(s, 0), sup.mon) || !ReadInts(At(s, 1), sup.normal) || !ReadInts(At(s, 2), sup.terror)) {
            error = "supers row malformed";
            return false;
        }
        d.supers.push_back(sup);
    }

    for (const Value& z : root.Field("zones").items) {
        Zone zone;
        if (!ReadInts(At(z, 1), zone.base) || !ReadInts(At(z, 5), zone.terror) || !ReadInt(At(z, 0), zone.act)) {
            error = "zones row malformed";
            return false;
        }
        for (std::size_t g = 0; g < 3; ++g) {
            for (std::size_t d = 0; d < 3; ++d) {
                if (!ReadIntList(At(At(z, g + 2), d), zone.groups[g][d])) {
                    error = "zones row malformed";
                    return false;
                }
            }
        }
        d.zones.push_back(zone);
    }

    const Value& cfg = root.Field("cfg");
    std::vector<int> list;
    if (!ReadIntList(cfg.Field("cowMons"), list)) {
        error = "cfg.cowMons malformed";
        return false;
    }
    d.cfg.cowMons.insert(list.begin(), list.end());
    if (!ReadIntList(cfg.Field("cowSets"), list)) {
        error = "cfg.cowSets malformed";
        return false;
    }
    d.cfg.cowSets.insert(list.begin(), list.end());
    d.cfg.iqMode = cfg.Field("iqMode").text;
    d.cfg.iqExcludePE = cfg.Field("iqExcludePE").boolean;
    if (!ReadInt(cfg.Field("iqMaxBonus"), d.cfg.iqMaxBonus) || !ReadInts(cfg.Field("mfTail"), d.cfg.mfTail) ||
        !ReadInt(cfg.Field("heraldBoost"), d.cfg.heraldBoost) || !ReadInts(cfg.Field("terror"), d.cfg.terror)) {
        error = "cfg malformed";
        return false;
    }

    out = std::move(d);
    return true;
}

Engine::Engine(Data data) : data_(std::move(data)) {
    uniByItem_.assign(data_.items.size(), {});
    setByItem_.assign(data_.items.size(), {});
    itemsByType_.assign(data_.types.size(), {});
    for (std::size_t u = 0; u < data_.uniques.size(); ++u) {
        uniByItem_[data_.uniques[u].item].push_back(static_cast<int>(u));
    }
    for (std::size_t s = 0; s < data_.sets.size(); ++s) {
        setByItem_[data_.sets[s].item].push_back(static_cast<int>(s));
    }
    for (std::size_t i = 0; i < data_.items.size(); ++i) {
        itemsByType_[data_.items[i].type].push_back(static_cast<int>(i));
    }
    nw_ = static_cast<int>((data_.items.size() + 31) / 32) + 1;
}

void Engine::SetSettings(const Settings& s) {
    env_ = s;
    itemCache_.clear();
    directCache_.clear();
    tcMapCache_.clear();
    pickCache_.clear();
    pick1Cache_.clear();
}

double Engine::Roll(double v) const {
    return v <= 0 ? 1.0 : std::min(128.0, v) / v;
}

// ITEMS_RollItemQuality 0x4421B0: unique, set, rare, magic, superior, normal, low.
std::array<double, 7> Engine::QDist(int b, const Mods& mods, int ilvl) const {
    const Item& I = data_.items[b];
    const Type& T = data_.types[I.type];
    const auto& R = data_.ratios[I.ratio];
    const int diff = ilvl - I.ilvl;
    const int mf = env_.mf;
    std::array<double, 7> o{};
    double rem = 1;
    if (!(mf != 0 && mf <= -100)) {
        const bool use = mf != 0;
        const double v15 = mf + 100;
        const auto ch = [&](double base, double div, double dim, int tail) {
            const double x = base - Td(diff, div);
            double v = x * 128;
            if (use) {
                const double d = v15 > 110 ? Td(dim * mf, mf + dim) + 100 + Td(mf * tail, 100) : v15;
                if (d) v = Td(12800 * x, d);
            }
            return v;
        };
        double v = ch(R[0], R[1], 250, data_.cfg.mfTail[0]);
        if (v <= R[2]) v = R[2];
        v -= Td(v * mods[3], 1024);
        double p = Roll(v);
        o[0] = rem * p;
        rem *= 1 - p;

        v = ch(R[6], R[7], 500, data_.cfg.mfTail[1]);
        if (v <= R[8]) v = R[8];
        v -= Td(v * mods[2], 1024);
        p = Roll(v);
        o[1] = rem * p;
        rem *= 1 - p;

        if (T.flags & 4) {
            v = ch(R[3], R[4], 600, data_.cfg.mfTail[2]);
            if (v <= R[5]) v = R[5];
            v -= Td(v * mods[1], 1024);
            p = Roll(v);
            o[2] = rem * p;
            rem *= 1 - p;
        }
        if (T.flags & 2) {
            o[3] = rem;
            rem = 0;
        } else {
            const double x = R[9] - Td(diff, R[10]);
            v = x * 128;
            if (use) v = Td(12800 * x, v15);
            if (v <= R[11]) v = R[11];
            v -= Td(v * mods[0], 1024);
            p = Roll(v);
            o[3] = rem * p;
            rem *= 1 - p;
        }
    }
    if (rem > 0) {
        const double v = (R[12] - Td(diff, R[13])) * 128;
        const double ph = Roll(v);
        o[4] = rem * ph;
        const double r2 = rem * (1 - ph);
        const double w = (R[14] - Td(diff, R[15])) * 128;
        const double pl = w > 0 ? std::max(0.0, w - 128) / w : 0;
        o[6] = r2 * pl;
        o[5] = r2 * (1 - pl);
    }
    return o;
}

// The unique picker 0x443910. An empty result stands for "no candidate".
Engine::Choice Engine::UniSel(int b, int req, int ilvl) const {
    if (req >= 0) {
        const Unique& U = data_.uniques[req];
        if ((U.flags & 2) && U.ilvl <= ilvl) return {{"u" + std::to_string(req), 1.0}};
    }
    Choice el;
    double W = 0;
    for (int u : uniByItem_[b]) {
        const Unique& U = data_.uniques[u];
        if ((U.flags & 1) && U.ilvl <= ilvl) {
            const double w = std::max(U.weight, 1);
            el.push_back({"u" + std::to_string(u), w});
            W += w;
        }
    }
    if (W == 0) return {};
    for (auto& e : el) e.second /= W;
    return el;
}

// The set picker, with the cow-set restriction from the cow-set-drops plugin config.
Engine::Choice Engine::SetSel(int b, int req, int ilvl, bool cow) const {
    double W = 0;
    int direct = -1;
    Choice el;
    for (int s : setByItem_[b]) {
        const Set& S = data_.sets[s];
        if (S.ilvl > ilvl) continue;
        if (data_.cfg.cowSets.count(S.cow) && !cow) continue;
        if (s == req) direct = s;
        const double w = S.weight > 0 ? S.weight : 1;
        el.push_back({"s" + std::to_string(s), w});
        W += w;
    }
    if (direct >= 0) return {{"s" + std::to_string(direct), 1.0}};
    if (W == 0) return {};
    for (auto& e : el) e.second /= W;
    return el;
}

void Engine::AddUnique(OutMap& out, int b, int req, double p, const Ctx& c) const {
    const Choice u = UniSel(b, req, c.ilvl);
    if (!u.empty()) {
        for (const auto& e : u) AddTo(out, e.first, e.second * p);
    } else if (data_.items[b].flags & 2) {
        AddTo(out, "x" + std::to_string(b), p);
    } else {
        const int t = data_.items[b].type;
        AddTo(out, ((data_.types[t].flags & 4) ? "r" : "m") + std::to_string(t), p);
    }
}

void Engine::AddSet(OutMap& out, int b, int req, double p, const Ctx& c) const {
    const Choice s = SetSel(b, req, c.ilvl, c.cow);
    if (!s.empty()) {
        for (const auto& e : s) AddTo(out, e.first, e.second * p);
    } else {
        AddTo(out, "m" + std::to_string(data_.items[b].type), p);
    }
}

const OutMap& Engine::ItemOut(int b, const Mods& mods, const Ctx& c) {
    const std::string key = "I" + std::to_string(b) + "|" + ModsKey(mods) + "|" + c.k;
    const auto found = itemCache_.find(key);
    if (found != itemCache_.end()) return found->second;

    OutMap out;
    const Item& I = data_.items[b];
    const Type& T = data_.types[I.type];
    const std::string ti = std::to_string(I.type);
    if (b == data_.goldItem) {
        AddTo(out, "g", 1);
    } else if (T.flags & 1) {
        AddTo(out, "i" + std::to_string(b), 1);
    } else if (T.flags & 16) {
        AddTo(out, "q" + std::to_string(b), 1);
    } else if ((I.flags & 2) || ((T.flags & 2) && (I.flags & 1))) {
        AddUnique(out, b, -1, 1, c);
    } else {
        const auto q = QDist(b, mods, c.ilvl);
        if (q[0] > 0) AddUnique(out, b, -1, q[0], c);
        if (q[1] > 0) AddSet(out, b, -1, q[1], c);
        AddTo(out, "r" + ti, q[2]);
        AddTo(out, "m" + ti, q[3]);
        AddTo(out, "h" + ti, q[4]);
        AddTo(out, "n" + ti, q[5]);
        AddTo(out, "l" + ti, q[6]);
    }
    return itemCache_.emplace(key, std::move(out)).first->second;
}

const OutMap& Engine::DirectOut(int kind, int ref, const Ctx& c) {
    const std::string key = "E" + std::to_string(kind) + "|" + std::to_string(ref) + "|" + c.k;
    const auto found = directCache_.find(key);
    if (found != directCache_.end()) return found->second;

    OutMap out;
    if (kind == 2) {
        AddUnique(out, data_.uniques[ref].item, ref, 1, c);
    } else {
        AddSet(out, data_.sets[ref].item, ref, 1, c);
    }
    return directCache_.emplace(key, std::move(out)).first->second;
}

const OutMap& Engine::EntryMap(const std::array<int, 3>& e, const Mods& mods, const Ctx& c) {
    switch (e[0]) {
        case 0: return ItemOut(e[1], mods, c);
        case 1: return TcMap(e[1], MergeMods(mods, data_.tcs[e[1]].mods), c);
        case 2:
        case 3: return DirectOut(e[0], e[1], c);
        default: return GoldMap();
    }
}

// One pass of a treasure class: the per-pick hit chance (positive picks) or the final hit chance (negative picks).
// The entry is stored before it is filled, so a cycle sees the partial entry, as the page's memo does.
const Engine::TcPick& Engine::TcPickOf(int t, const Mods& mods, const Ctx& c) {
    const std::string key = "P" + std::to_string(t) + "|" + ModsKey(mods) + "|" + c.k;
    const auto found = pickCache_.find(key);
    if (found != pickCache_.end()) return found->second;

    const TreasureClass& T = data_.tcs[t];
    TcPick& r = pickCache_.emplace(key, TcPick{}).first->second;
    r.neg = T.picks < 0;
    r.k = std::max(std::abs(T.picks), 1);
    int total = 0;
    for (const auto& e : T.entries) total += e[2];
    if (!total) return r;

    if (r.neg) {
        OutMap none;
        for (const auto& e : T.entries) {
            for (const auto& kv : EntryMap(e, mods, c)) {
                const double prev = none.count(kv.first) ? none[kv.first] : 1.0;
                none[kv.first] = prev * std::pow(1 - kv.second, e[2]);
            }
        }
        for (const auto& kv : none) r.map[kv.first] = 1 - kv.second;
    } else {
        const double W = total + NoDrop(T.noDrop, total, env_.players);
        for (const auto& e : T.entries) {
            const double w = e[2] / W;
            for (const auto& kv : EntryMap(e, mods, c)) {
                r.map[kv.first] += w * kv.second;
            }
        }
    }
    return r;
}

const OutMap& Engine::TcMap(int t, const Mods& mods, const Ctx& c) {
    const std::string key = "H" + std::to_string(t) + "|" + ModsKey(mods) + "|" + c.k;
    const auto found = tcMapCache_.find(key);
    if (found != tcMapCache_.end()) return found->second;

    const TcPick& P = TcPickOf(t, mods, c);
    OutMap out;
    if (P.neg) {
        out = P.map;
    } else {
        for (const auto& kv : P.map) out[kv.first] = 1 - std::pow(1 - kv.second, P.k);
    }
    return tcMapCache_.emplace(key, std::move(out)).first->second;
}

Engine::IqExtra Engine::Iq(int picks, bool pe) const {
    const int v = env_.iq;
    if (v <= 0 || picks <= 0 || (pe && data_.cfg.iqExcludePE)) return {0, 0};
    const int h = data_.cfg.iqMode == "flat" ? v * 100 : data_.cfg.iqMode == "chance" ? v : picks * v;
    int e0 = h / 100;
    double r = (h % 100) / 100.0;
    const int max = data_.cfg.iqMaxBonus;
    if (max > 0) {
        if (e0 >= max) {
            e0 = max;
            r = 0;
        } else if (e0 + 1 > max) {
            r = 0;
        }
    }
    return {e0, r};
}

OutMap Engine::TopMap(int t, const Ctx& c, bool pe) {
    if (t < 0) return {};
    const TreasureClass& T = data_.tcs[t];
    const TcPick P = TcPickOf(t, T.mods, c);
    if (P.neg) return P.map;
    const IqExtra x = Iq(T.picks, pe);
    OutMap out;
    for (const auto& kv : P.map) {
        const double q = 1 - kv.second;
        out[kv.first] = 1 - ((1 - x.r) * std::pow(q, P.k + x.e0) + x.r * std::pow(q, P.k + x.e0 + 1));
    }
    return out;
}

const std::vector<std::uint32_t>& Engine::ReachBits(int t) {
    const auto found = reachCache_.find(t);
    if (found != reachCache_.end()) return found->second;

    std::vector<std::uint32_t>& r = reachCache_[t];
    r.assign(static_cast<std::size_t>(nw_), 0u);
    for (const auto& e : data_.tcs[t].entries) {
        if (e[0] == 1) {
            const std::vector<std::uint32_t> s = ReachBits(e[1]);
            for (int i = 0; i < nw_; ++i) r[static_cast<std::size_t>(i)] |= s[static_cast<std::size_t>(i)];
            continue;
        }
        int b = data_.goldItem;
        if (e[0] == 0) b = e[1];
        else if (e[0] == 2) b = data_.uniques[e[1]].item;
        else if (e[0] == 3) b = data_.sets[e[1]].item;
        if (b >= 0) r[static_cast<std::size_t>(b >> 5)] |= 1u << (b & 31);
    }
    return r;
}

bool Engine::Reach(int t, const Bases& B) {
    const std::vector<std::uint32_t> r = ReachBits(t);
    for (int b : B.arr) {
        if (r[static_cast<std::size_t>(b >> 5)] & (1u << (b & 31))) return true;
    }
    return false;
}

double Engine::Pick1(int t, const Mods& mods, const Ctx& c, const std::string& R, const Bases& B) {
    const std::string key = "1" + std::to_string(t) + "|" + ModsKey(mods) + "|" + c.k + "|" + R;
    const auto found = pick1Cache_.find(key);
    if (found != pick1Cache_.end()) return found->second;
    pick1Cache_[key] = 0;

    const TreasureClass& T = data_.tcs[t];
    int total = 0;
    for (const auto& e : T.entries) total += e[2];
    double v = 0;
    if (total) {
        if (T.picks < 0) {
            double none = 1;
            for (const auto& e : T.entries) {
                const double p = Entry1(e, mods, c, R, B);
                if (p) none *= std::pow(1 - p, e[2]);
            }
            v = 1 - none;
        } else {
            const double W = total + NoDrop(T.noDrop, total, env_.players);
            for (const auto& e : T.entries) {
                const double p = Entry1(e, mods, c, R, B);
                if (p) v += e[2] / W * p;
            }
        }
    }
    pick1Cache_[key] = v;
    return v;
}

double Engine::Hit1(int t, const Mods& mods, const Ctx& c, const std::string& R, const Bases& B) {
    if (!Reach(t, B)) return 0;
    const TreasureClass& T = data_.tcs[t];
    const double p = Pick1(t, mods, c, R, B);
    return T.picks < 0 ? p : 1 - std::pow(1 - p, std::max(T.picks, 1));
}

double Engine::Entry1(const std::array<int, 3>& e, const Mods& mods, const Ctx& c, const std::string& R,
                      const Bases& B) {
    switch (e[0]) {
        case 0: {
            if (!B.in[e[1]]) return 0;
            const OutMap& m = ItemOut(e[1], mods, c);
            const auto f = m.find(R);
            return f == m.end() ? 0 : f->second;
        }
        case 1: return Hit1(e[1], MergeMods(mods, data_.tcs[e[1]].mods), c, R, B);
        case 2:
        case 3: {
            const int item = e[0] == 2 ? data_.uniques[e[1]].item : data_.sets[e[1]].item;
            if (!B.in[item]) return 0;
            const OutMap& m = DirectOut(e[0], e[1], c);
            const auto f = m.find(R);
            return f == m.end() ? 0 : f->second;
        }
        default: return R == "g" ? 1 : 0;
    }
}

double Engine::Top1(int t, const Ctx& c, bool pe, const std::string& R, const Bases& B) {
    if (t < 0 || !Reach(t, B)) return 0;
    const TreasureClass& T = data_.tcs[t];
    const double p = Pick1(t, T.mods, c, R, B);
    if (T.picks < 0) return p;
    const IqExtra x = Iq(T.picks, pe);
    const double q = 1 - p;
    const int k = std::max(T.picks, 1);
    return 1 - ((1 - x.r) * std::pow(q, k + x.e0) + x.r * std::pow(q, k + x.e0 + 1));
}

Bases Engine::BasesOf(const std::string& R) const {
    Bases B;
    const char kind = R.empty() ? '\0' : R[0];
    const int n = std::stoi(R.substr(1).empty() ? "0" : R.substr(1));
    if (kind == 'g') {
        B.arr = {data_.goldItem};
    } else if (kind == 'i' || kind == 'x' || kind == 'q') {
        B.arr = {n};
    } else if (kind == 'u') {
        B.arr = {data_.uniques[n].item};
    } else if (kind == 's') {
        B.arr = {data_.sets[n].item};
    } else if (n >= 0 && static_cast<std::size_t>(n) < itemsByType_.size()) {
        B.arr = itemsByType_[n];
    }
    B.in.assign(data_.items.size(), 0);
    for (int b : B.arr) B.in[b] = 1;
    return B;
}

// TC level walk 0x397290: consecutive rows of one group while the next row's level is within the walk.
int Engine::Upgrade(int t, int lvl) const {
    if (t < 0 || t >= data_.txtCount || lvl <= 0 || !data_.tcs[t].group) return t;
    int cur = t;
    while (cur + 1 < data_.txtCount && data_.tcs[cur + 1].group == data_.tcs[cur].group) {
        if (data_.tcs[cur + 1].level > lvl) break;
        ++cur;
    }
    return cur;
}

// The treasure class choice, the native selector 0x447070 (see df-engine.js, resolve).
Resolved Engine::Resolve(const Selection& v) const {
    const bool sup = v.superunique;
    const Monster& M = sup ? data_.mons[data_.supers[v.i].mon] : data_.mons[v.i];
    const bool boss = (M.flags & 1) != 0;
    const bool nr = (M.flags & 2) != 0;
    int base = (v.z >= 0 && !boss && !nr) ? data_.zones[v.z].base[v.d] : M.base[v.d];
    if (v.x > 0) base = std::max(base, v.z >= 0 ? data_.zones[v.z].terror[v.d] : data_.cfg.terror[v.d]);
    const int ilvl = std::max(1, base + (sup ? 3 : v.t == 1 ? 2 : v.t == 2 ? 3 : 0));
    int walk = (v.x > 0 || (!boss && !nr)) ? ilvl : 0;
    if (v.x == 2) walk += data_.cfg.heraldBoost;

    int t = -1;
    if (sup) {
        const Superunique& S = data_.supers[v.i];
        if (v.x > 0) t = S.terror[v.d];
        if (t < 0) t = S.normal[v.d];
    } else {
        const auto& T = M.tc[v.d];
        const int col = v.t == 1 ? 1 : v.t == 2 ? 2 : 0;
        if (v.x == 2) {
            t = T[7];
        } else {
            if (v.x == 1) t = T[4 + col];
            if (t < 0) t = T[col];
        }
    }
    const int q = (!sup && (M.flags & 8)) ? M.tc[v.d][3] : -1;

    Resolved r;
    r.ilvl = ilvl;
    r.c.ilvl = ilvl;
    r.c.cow = data_.cfg.cowMons.count(M.code) > 0;
    r.c.k = std::to_string(ilvl) + (r.c.cow ? "c" : "");
    r.tc = Upgrade(t, walk);
    r.qtc = q >= 0 ? Upgrade(q, walk) : -1;
    r.pe = (M.flags & 4) != 0;
    r.approx = sup && v.z < 0;
    return r;
}

}  // namespace esrwiki::drop
