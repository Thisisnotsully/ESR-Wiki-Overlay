// Tests for matching the official Maps page to the Campaign Progression map. Arguments: data/campaign-map.txt, the
// data/website-maps folder and the data/locales folder.
#include "campaign_live.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

namespace {
int failures = 0;
void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}
std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool Has(const std::map<int, std::size_t>& map, int id) { return map.find(id) != map.end(); }
}  // namespace

int main(int argc, char** argv) {
    using namespace esrwiki;
    Check(campaign::MapsNameKey("  Blood Raven's  WORKSHOP ") == "blood raven s workshop", "key folds case and punctuation");
    Check(campaign::MapsNameKey("Caf\xC3\xA9 \xC3\x89t\xC3\xA9") == "cafe ete", "key folds accents");
    Check(campaign::MapsNameKey("").empty(), "empty key");
    Check(campaign::MapsNoteText({"CAUTION: a", "b.", "", "c"}) == "CAUTION: a b. c", "note lines joined with spaces");
    // U+4E00 U+4E8C, then "3": no space next to Chinese; Hangul U+AC00 keeps its space.
    Check(campaign::MapsNoteText({"\xE4\xB8\x80", "\xE4\xBA\x8C", "3"}) == "\xE4\xB8\x80\xE4\xBA\x8C" "3", "Chinese lines joined without spaces");
    Check(campaign::MapsNoteText({"\xEA\xB0\x80", "\xEA\xB0\x80"}) == "\xEA\xB0\x80 \xEA\xB0\x80", "Korean lines keep the space");
    if (argc < 4) {
        std::cerr << "usage: campaign_live_test campaign-map.txt website-maps-dir locales-dir\n";
        return 2;
    }
    campaign::Data data;
    std::string error;
    Check(campaign::Parse(ReadFile(argv[1]), data, error), "campaign map parses: " + error);
    mapspage::Page english;
    Check(mapspage::Parse(ReadFile(std::string(argv[2]) + "/maps.htm"), english, error), "English page parses");

    const campaign::LiveMaps en = campaign::MatchMapsPage(data, english, nullptr);
    Check(en.unmatchedTeleport.empty(), "English: every teleport row matches (" + std::to_string(en.unmatchedTeleport.size()) + " do not)");
    Check(en.unmatchedCards.empty(), "English: every card matches (" + std::to_string(en.unmatchedCards.size()) + " do not)");
    for (const std::size_t i : en.unmatchedCards) std::cerr << "  unmatched card: " << english.cards[i].name << "\n";
    for (const int id : {25, 37, 60, 102, 108}) Check(Has(en.teleportOf, id), "English teleport row for area " + std::to_string(id));
    Check(!Has(en.teleportOf, 21), "Tower Cellar Level 1 has no teleport row");
    for (const int id : {39, 12, 16, 55, 59, 65, 44, 77, 86, 94, 87, 84, 125, 126, 127, 115, 214, 130, 119, 116, 122, 113, 123, 124})
        Check(Has(en.cardOf, id), "English card for area " + std::to_string(id));
    Check(!Has(en.cardOf, 137) && !Has(en.cardOf, 138), "areas the page does not list have no card");
    Check(en.cardOf.count(12) == 1 && en.cardOf.count(16) == 1 && en.cardOf.at(12) == en.cardOf.at(16), "both workshop levels share a card");
    Check(!en.note.empty(), "the closing note is kept");
    Check(en.cardOf.count(12) == 1 && campaign::MapsCardText(english.cards[en.cardOf.at(12)]) == "Level 1 and Level 2\nLocation: Act 1 Tamoe Highland",
          "card text is the page's size and location lines");

    const char* codes[] = {"deDE", "esES", "esMX", "frFR", "itIT", "jaJP", "koKR", "plPL", "ptBR", "ruRU", "zhCN", "zhTW"};
    for (const char* code : codes) {
        mapspage::Page page;
        const bool parsed = mapspage::Parse(ReadFile(std::string(argv[3]) + "/" + code + "/maps.htm"), page, error);
        Check(parsed, std::string(code) + ": page parses");
        if (!parsed) continue;
        const campaign::LiveMaps live = campaign::MatchMapsPage(data, page, &english);
        Check(live.unmatchedTeleport.empty() && live.unmatchedCards.empty(), std::string(code) + ": every row and card matches");
        Check(live.teleportOf == en.teleportOf, std::string(code) + ": teleport rows match the same areas as English");
        Check(live.cardOf == en.cardOf, std::string(code) + ": cards match the same areas as English");
        Check(!live.note.empty(), std::string(code) + ": closing note");
        Check(live.page.cards.size() == page.cards.size() && live.page.cards[1].name == page.cards[1].name, std::string(code) + ": text kept as parsed");
        // Without the English page, a page whose names differ from the game's is left with unmatched rows, never wrong ones.
        const campaign::LiveMaps alone = campaign::MatchMapsPage(data, page, nullptr);
        for (const auto& [id, at] : alone.cardOf)
            Check(en.cardOf.count(id) == 1 && en.cardOf.at(id) == at, std::string(code) + ": a name match agrees with English (area " + std::to_string(id) + ")");
    }
    // A page with a different number of cards falls back to names only.
    mapspage::Page shorter = english;
    shorter.cards.pop_back();
    const campaign::LiveMaps cut = campaign::MatchMapsPage(data, shorter, &english);
    Check(cut.cardOf.size() == en.cardOf.size(), "names alone still match the English cards");
    mapspage::Page unknown = english;
    unknown.teleport[0].area = "Somewhere Else";
    unknown.cards[1].name = "Nowhere Known";
    const campaign::LiveMaps odd = campaign::MatchMapsPage(data, unknown, nullptr);
    Check(odd.unmatchedTeleport.size() == 1 && odd.unmatchedTeleport[0] == 0, "an unknown teleport row is listed");
    Check(odd.unmatchedCards.size() == 1 && odd.unmatchedCards[0] == 1, "an unknown card is listed");

    if (failures == 0) std::cout << "campaign_live tests passed\n";
    return failures == 0 ? 0 : 1;
}
