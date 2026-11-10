#include "campaign_live.hpp"

#include <algorithm>
#include <set>

#include "text_fold.hpp"

namespace esrwiki::campaign {

namespace {

// `name` without a trailing "(...)" (the official names' "(formely Lost City)").
std::string WithoutParenthesis(const std::string& name) {
    const std::size_t open = name.rfind('(');
    if (open == std::string::npos || open == 0 || name.find(')', open) == std::string::npos) return name;
    std::string out = name.substr(0, open);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// A key without a trailing "level <digits>" word pair (English names only).
std::string WithoutLevel(const std::string& key) {
    const std::size_t space = key.rfind(' ');
    if (space == std::string::npos) return key;
    const std::string number = key.substr(space + 1);
    if (number.empty() || !std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; })) return key;
    const std::string rest = key.substr(0, space);
    const std::string word = " level";
    if (rest.size() <= word.size() || rest.compare(rest.size() - word.size(), word.size(), word) != 0) return key;
    return rest.substr(0, rest.size() - word.size());
}

struct Index {
    std::multimap<std::string, int> exact;  // every name in every language, the official name
    std::multimap<std::string, int> base;   // the English and official names without "(...)" and "Level N"
    std::map<int, std::string> englishBase; // area -> its English name's base key
};

void Add(std::multimap<std::string, int>& into, const std::string& key, int id) {
    if (key.empty()) return;
    const auto range = into.equal_range(key);
    for (auto it = range.first; it != range.second; ++it)
        if (it->second == id) return;
    into.emplace(key, id);
}

Index MakeIndex(const Data& data) {
    Index index;
    for (const Area& area : data.areas) {
        for (const auto& [lang, name] : area.names) {
            Add(index.exact, MapsNameKey(name), area.id);
            if (lang == "enUS") {
                const std::string base = WithoutLevel(MapsNameKey(WithoutParenthesis(name)));
                Add(index.base, base, area.id);
                index.englishBase[area.id] = base;
            }
        }
        if (!area.officialName.empty()) {
            Add(index.exact, MapsNameKey(area.officialName), area.id);
            Add(index.base, WithoutLevel(MapsNameKey(WithoutParenthesis(area.officialName))), area.id);
        }
    }
    return index;
}

// The areas a name stands for (sorted ids). `levels`: also the other levels of every area found (a card).
std::vector<int> Lookup(const Index& index, const std::string& name, bool levels) {
    std::set<int> found;
    const std::string key = MapsNameKey(name);
    if (key.empty()) return {};
    for (const auto* map : {&index.exact, &index.base}) {
        const auto range = map->equal_range(key);
        for (auto it = range.first; it != range.second; ++it) found.insert(it->second);
    }
    if (levels && !found.empty()) {
        std::set<std::string> bases;
        for (const int id : found)
            if (const auto it = index.englishBase.find(id); it != index.englishBase.end() && !it->second.empty()) bases.insert(it->second);
        for (const std::string& base : bases) {
            const auto range = index.base.equal_range(base);
            for (auto it = range.first; it != range.second; ++it)
                if (const auto own = index.englishBase.find(it->second); own != index.englishBase.end() && own->second == base)
                    found.insert(it->second);
        }
    }
    return std::vector<int>(found.begin(), found.end());
}

}  // namespace

std::string MapsNameKey(const std::string& name) {
    const std::u32string folded = text::SearchText(text::DecodeUtf8(name));
    const std::vector<std::u32string> words = text::SplitWords(folded);
    std::u32string joined;
    for (const std::u32string& word : words) {
        if (!joined.empty()) joined += U' ';
        joined += word;
    }
    return text::EncodeUtf8(joined);
}

LiveMaps MatchMapsPage(const Data& data, const mapspage::Page& page, const mapspage::Page* english) {
    LiveMaps out;
    out.page = page;
    const Index index = MakeIndex(data);

    const bool teleportByPosition = english != nullptr && english->teleport.size() == page.teleport.size();
    for (std::size_t i = 0; i < page.teleport.size(); ++i) {
        std::vector<int> ids = Lookup(index, page.teleport[i].area, false);
        if (ids.empty() && teleportByPosition) ids = Lookup(index, english->teleport[i].area, false);
        if (ids.empty()) out.unmatchedTeleport.push_back(i);
        for (const int id : ids) out.teleportOf.emplace(id, i);  // the first row naming an area wins
    }

    const bool cardsByPosition = english != nullptr && english->cards.size() == page.cards.size();
    for (std::size_t i = 0; i < page.cards.size(); ++i) {
        const mapspage::SpecialCard& card = page.cards[i];
        if (card.note) {
            if (out.note.empty()) out.note = card.lines;
            continue;
        }
        std::vector<int> ids = Lookup(index, card.name, true);
        if (ids.empty() && cardsByPosition && !english->cards[i].note) ids = Lookup(index, english->cards[i].name, true);
        if (ids.empty()) out.unmatchedCards.push_back(i);
        for (const int id : ids) out.cardOf.emplace(id, i);
    }
    return out;
}

std::string MapsNoteText(const std::vector<std::string>& lines) {
    // Han, kana, CJK punctuation and full-width forms; Hangul is written with spaces and is not one of them.
    const auto unspaced = [](char32_t c) {
        return (c >= 0x2E80 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
               (c >= 0xFF00 && c <= 0xFFEF);
    };
    std::u32string out;
    for (const std::string& line : lines) {
        const std::u32string next = text::DecodeUtf8(line);
        if (next.empty()) continue;
        if (!out.empty() && !unspaced(out.back()) && !unspaced(next.front())) out += U' ';
        out += next;
    }
    return text::EncodeUtf8(out);
}

std::string MapsCardText(const mapspage::SpecialCard& card) {
    std::string text;
    for (const std::string* line : {&card.size, &card.location}) {
        if (line->empty()) continue;
        if (!text.empty()) text += '\n';
        text += *line;
    }
    return text;
}

}  // namespace esrwiki::campaign
