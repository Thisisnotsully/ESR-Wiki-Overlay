// Tests for the search suggestions (search_index.cpp).
#include "search_index.hpp"
#include "spec_data.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

using namespace esrwiki;

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

Block Card(const std::string& title, std::vector<std::string> lines, std::vector<std::pair<std::string, std::string>> fields = {}) {
    Block block;
    block.item = true;
    block.title = title;
    for (auto& line : lines) block.lines.push_back(Line{LineKind::Stat, line});
    for (auto& field : fields) block.fields.push_back(Field{field.first, field.second});
    return block;
}

Database Sample() {
    Database db;
    Page runewords;
    runewords.id = "runewords";
    runewords.title = "Runewords";
    Entry weapons;
    weapons.title = "Weapons";
    weapons.blocks.push_back(Card("Spirit", {"+2 To All Skills", "\xC3\xBF" "c1+25-35% Faster Cast Rate"}, {{"Required Level", "25"}}));
    weapons.blocks.push_back(Card("Breath of the Dying", {"Indestructible", "+5% To Life Steal"}));
    weapons.blocks.push_back(Card("Ice Breaker", {"Cold damage"}));
    runewords.entries.push_back(weapons);
    Page uniques;
    uniques.id = "uniques";
    uniques.title = "Uniques";
    Entry rings;
    rings.title = "Caf\xC3\xA9 Rings";  // Café
    rings.blocks.push_back(Card("Stone of Jordan", {"+1 To All Skills", "Increase Maximum Mana 25%"}));
    rings.blocks.push_back(Card("\xD0\x93\xD0\xBE\xD1\x80\xD1\x8B\xD0\xBD\xD1\x8F", {"\xD0\x81\xD0\xBA\xD0\xBE\xD1\x80\xD0\xBE\xD1\x81\xD1\x82\xD1\x8C"}));  // Горыня / Ёскорость
    uniques.entries.push_back(rings);
    db.pages = {runewords, uniques};
    return db;
}
}  // namespace

int main(int argc, char** argv) {
    const Database db = Sample();
    search::Index index;
    index.Build(db);
    Check(index.Size() > 8, "index has records");

    auto hits = index.Query(db, "spirit", 0);
    Check(!hits.empty() && hits[0].label == "Spirit" && hits[0].block == 0 && hits[0].entry == 0 && hits[0].page == 0, "exact title first");
    Check(!hits.empty() && hits[0].context == "Runewords \xE2\x80\xBA Weapons", "context shows page and entry");

    hits = index.Query(db, "spir", 0);
    Check(!hits.empty() && hits[0].label == "Spirit" && hits[0].labelMarks.size() == 4, "prefix match with marks");

    hits = index.Query(db, "BREAKER", 0);
    Check(!hits.empty() && hits[0].label == "Ice Breaker", "case folded, word prefix");

    hits = index.Query(db, "spirt", 0);
    Check(!hits.empty() && hits[0].label == "Spirit", "one typo still finds it");
    Check(!hits.empty() && !hits[0].exact, "a typo hit is not exact");
    hits = index.Query(db, "spir", 0);
    Check(!hits.empty() && hits[0].exact, "a prefix hit is exact");

    hits = index.Query(db, "bkr", 0);
    Check(!hits.empty() && hits[0].label.find("Break") != std::string::npos, "subsequence finds Breaker / Breath");

    hits = index.Query(db, "life steal", 0);
    Check(!hits.empty() && hits[0].label == "Breath of the Dying" && !hits[0].snippet.empty() && hits[0].snippet.find("Life Steal") != std::string::npos,
          "text search: the card, with the matching line");
    Check(!hits.empty() && !hits[0].snippetMarks.empty(), "text search marks the words");

    hits = index.Query(db, "faster cast", 0);
    Check(!hits.empty() && hits[0].label == "Spirit" && hits[0].snippet == "+25-35% Faster Cast Rate", "colour codes are not searched or shown");

    hits = index.Query(db, "all skills", 1);
    Check(hits.size() >= 2, "two cards share the stat");
    Check(!hits.empty() && hits[0].label == "Stone of Jordan", "the current page ranks first");
    hits = index.Query(db, "all skills", 0);
    Check(!hits.empty() && hits[0].label == "Spirit", "and the other page first when it is current");

    hits = index.Query(db, "cafe", 1);
    Check(!hits.empty() && hits[0].entry == 0 && hits[0].block == -1, "accents fold: cafe finds Caf\xC3\xA9 (the entry)");

    hits = index.Query(db, "\xD0\xB3\xD0\xBE\xD1\x80\xD1\x8B\xD0\xBD\xD1\x8F", 0);  // горыня in lower case
    Check(!hits.empty() && hits[0].block == 1, "Cyrillic case folds");
    hits = index.Query(db, "\xD0\xB5\xD0\xBA\xD0\xBE\xD1\x80\xD0\xBE\xD1\x81\xD1\x82\xD1\x8C", 0);  // екорость finds Ёкорость
    Check(!hits.empty(), "ё matches е");

    hits = index.Query(db, "runew", 1);
    Check(!hits.empty() && hits[0].entry == -1 && hits[0].page == 0, "a page title is a target");

    Check(index.Query(db, "a", 0).empty(), "one letter gives nothing");
    Check(index.Query(db, "", 0).empty(), "empty gives nothing");
    Check(index.Query(db, "zzzzqq", 0).empty(), "nonsense gives nothing");
    Check(index.Query(db, "all skills", 0, 1).size() == 1, "limit is respected");

    // The page filter uses the same folding and word matching, so a jump is never filtered away by the typed text.
    Check(FilterEntries(db.pages[1], "CAFE").size() == 1, "filter folds accents and case");
    Check(FilterEntries(db.pages[0], "faster rate").size() == 1, "filter finds words on different lines of a card");
    Check(FilterEntries(db.pages[0], "faster zzz").empty(), "filter needs every word");
    Check(ArrangeBlocks(db.pages[0].entries[0], 0, "", "", BlockSort::Default, "life steal").size() == 1, "blocks are arranged by the same match");

    // ---- The query language, shared by the suggestions and the filter ----
    {
        const text::ParsedQuery q = text::ParseQuery("Fire \"Cold  Damage\" -poison -\"life steal\"");
        Check(q.terms.size() == 4, "four terms");
        Check(q.HasRequired() && q.HasExcluded(), "required and excluded");
        const auto req = q.RequiredTerms();
        Check(req.size() == 2 && req[0] == "fire" && req[1] == "cold damage", "required terms in order, folded");
        const auto shown = q.RequiredDisplay();
        Check(shown.size() == 2 && shown[0] == "Fire" && shown[1] == "\"Cold  Damage\"", "required terms as typed");
        const auto ex = q.ExcludedDisplay();
        Check(ex.size() == 2 && ex[0] == "poison" && ex[1] == "\"life steal\"", "excluded terms as typed");
        Check(text::ParseQuery("-").empty(), "a lone minus is nothing");
        Check(text::ParseQuery("spirit -").terms.size() == 1 && !text::ParseQuery("spirit -").HasExcluded(), "a trailing minus is ignored");
        const text::ParsedQuery open = text::ParseQuery("\"faster cast");
        Check(open.terms.size() == 2 && !open.terms[0].phrase && open.RequiredTerms()[1] == "cast", "an unclosed quote is plain text");
        Check(text::ParseQuery("-jordan").HasExcluded() && !text::ParseQuery("-jordan").HasRequired(), "only excluded: nothing required");
        const text::ParsedQuery dash = text::ParseQuery("-life-steal");
        Check(dash.terms.size() == 1 && dash.terms[0].excluded && dash.terms[0].phrase && dash.terms[0].folded == "life steal",
              "-life-steal excludes the words together");
        Check(text::ParseQuery("life-steal").RequiredTerms().size() == 2, "punctuation splits words");
        Check(text::ParseQuery("\xE2\x80\x9C" "faster cast" "\xE2\x80\x9D").terms.size() == 1, "curly quotes make a phrase");
        Check(text::ParseQuery("faster\xE3\x80\x80" "cast").terms.size() == 2, "the ideographic space separates");
        Check(text::ParseQuery("\xE7\x81\xAB\xE7\x84\xB0\xE3\x80\x81\xE5\x86\xB0\xE9\x9C\x9C").terms.size() == 2, "CJK comma separates");
        Check(text::ParseQuery("\xEF\xBC\xB3\xEF\xBC\xB0\xEF\xBC\xA9\xEF\xBC\xB2\xEF\xBC\xA9\xEF\xBC\xB4").RequiredTerms()[0] == "spirit",
              "full-width letters fold to ASCII");
    }

    // ---- Folding: Greek, Latin Extended-B / Additional (Vietnamese), Cyrillic ----
    Check(text::FoldUtf8("\xCE\x86\xCE\x88\xCE\x89\xCE\x8A\xCE\x8C\xCE\x8E\xCE\x8F") ==
              "\xCE\xB1\xCE\xB5\xCE\xB7\xCE\xB9\xCE\xBF\xCF\x85\xCF\x89", "Greek tonos capitals fold");  // ΆΈΉΊΌΎΏ -> αεηιουω
    Check(text::FoldUtf8("\xCF\x82\xCE\xA3\xCE\x90\xE1\xBC\x84") == "\xCF\x83\xCF\x83\xCE\xB9\xCE\xB1", "final sigma, dialytika, polytonic");  // ςΣΐἄ -> σσια
    Check(text::FoldUtf8("Ki\xE1\xBA\xBFm Th\xE1\xBA\xA7n Ph\xC6\xB0\xC6\xA1ng \xC4\x90\xE1\xBB\x87") == "kiem than phuong de",
          "Vietnamese folds");  // Kiếm Thần Phương Đệ
    Check(text::FoldUtf8("\xC8\x98\xC8\x9B\xC7\x8E\xC6\x80") == "stab", "Latin Extended-B folds");  // Șțǎƀ
    Check(text::FoldUtf8("\xD0\x81\xD0\x99\xD0\x96") == "\xD0\xB5\xD0\xB8\xD0\xB6", "Cyrillic folds");  // ЁЙЖ -> еиж
    Check(text::Fold(U"ÀỆἈx").size() == 4, "one code point out for each one in");

    // ---- A sample with the cases the review asked for ----
    {
        Database db2;
        Page p;
        p.id = "cards";
        p.title = "Cards";
        Entry rw;
        rw.title = "Runewords";
        Block spirit = Card("Spirit", {"+2 To All Skills", "+25-35% Faster Cast Rate"});
        spirit.note = "Best in a Monarch for casters";
        rw.blocks.push_back(spirit);
        rw.blocks.push_back(Card("Insight", {"Level 12 Meditation Aura When Equipped", "+35% Faster Cast Rate"}));
        rw.blocks.push_back(Card("Resolve", {"+10 Resist All", "Cast faster"}));
        rw.blocks.push_back(Card("Ist Charm", {"+1 Magic Find"}));
        rw.blocks.push_back(Card("\xCE\x91\xCF\x83\xCF\x80\xCE\xAF\xCE\xB4\xCE\xB1\xCF\x82", {"\xCE\x9A\xCF\x81\xCF\x8D\xCE\xBF"}));  // Ασπίδας / Κρύο
        rw.blocks.push_back(Card("Ki\xE1\xBA\xBFm Th\xE1\xBA\xA7n", {"S\xC3\xA1t th\xC6\xB0\xC6\xA1ng"}));  // Kiếm Thần / Sát thương
        rw.blocks.push_back(Card("\xE7\x81\xAB\xE7\x84\xB0\xE3\x80\x81\xE5\x86\xB0\xE9\x9C\x9C", {"\xE5\x86\xB0\xE9\x9C\x9C"}));  // 火焰、冰霜
        p.entries.push_back(rw);
        Entry enigmaEntry;
        enigmaEntry.title = "Enigma";
        enigmaEntry.blocks.push_back(Card("Enigma", {"+2 To All Skills", "Teleport"}));
        p.entries.push_back(enigmaEntry);
        Entry many;
        many.title = "Bases";
        for (int i = 0; i < 5; ++i) many.blocks.push_back(Card("Shield " + std::to_string(i), {"Defense"}));
        many.blocks.push_back(Card("Shirt", {"Light"}));
        p.entries.push_back(many);
        Entry more;  // at most 6 cards of one entry are listed: the next 5 shields are in another entry
        more.title = "More Bases";
        for (int i = 5; i < 10; ++i) more.blocks.push_back(Card("Shield " + std::to_string(i), {"Defense"}));
        p.entries.push_back(more);
        db2.pages = {p};
        search::Index ix;
        ix.Build(db2);
        const auto labels = [&](const std::string& q) {
            std::vector<std::string> out;
            for (const auto& h : ix.Query(db2, q, 0, 50)) out.push_back(h.label);
            return out;
        };
        const auto has = [](const std::vector<std::string>& v, const std::string& x) {
            return std::find(v.begin(), v.end(), x) != v.end();
        };

        // quotes
        Check(has(labels("\"faster cast\""), "Spirit") && has(labels("\"faster cast\""), "Insight"), "phrase finds the words together");
        Check(!has(labels("\"faster cast\""), "Resolve"), "phrase: not in the other order");
        Check(has(labels("cast faster"), "Resolve") && has(labels("cast faster"), "Spirit"), "plain words: any order");
        Check(!has(labels("\"aster cast\""), "Spirit"), "a phrase is whole words");
        Check(BlockMatches(db2.pages[0].entries[0].blocks[0], "\"faster cast\"") && !BlockMatches(db2.pages[0].entries[0].blocks[2], "\"faster cast\""),
              "the filter reads phrases the same way");
        // minus
        auto v = labels("faster -insight");
        Check(has(v, "Spirit") && !has(v, "Insight"), "minus excludes a card");
        v = labels("cast -\"faster cast\"");
        Check(v.size() == 1 && v[0] == "Resolve", "minus excludes a phrase");
        Check(labels("-insight").empty(), "only excluded terms: no suggestions");
        Check(ArrangeBlocks(db2.pages[0].entries[0], 0, "", "", BlockSort::Default, "-insight").size() == 6, "only excluded terms: the filter drops those cards");
        Check(ArrangeBlocks(db2.pages[0].entries[0], 0, "", "", BlockSort::Default, "runewords -insight").size() == 6,
              "the entry matches, but an excluded card still drops out");
        // degrade gracefully
        Check(labels("spirit -") == labels("spirit"), "a lone minus is ignored");
        Check(has(labels("\"spirit"), "Spirit"), "an unclosed quote still searches");
        Check(FilterEntries(db2.pages[0], "\"spirit").size() == 1 && FilterEntries(db2.pages[0], "-").size() == 4, "the filter degrades the same way");
        // note
        auto hits2 = ix.Query(db2, "monarch casters", 0, 8);
        Check(!hits2.empty() && hits2[0].label == "Spirit" && hits2[0].snippet.find("Monarch") != std::string::npos, "the note is searched, with a snippet");
        Check(FilterEntries(db2.pages[0], "monarch").size() == 1 && BlockMatches(db2.pages[0].entries[0].blocks[0], "monarch"), "the filter searches the note");
        // folding through search
        Check(has(labels("\xCE\x91\xCE\xA3\xCE\xA0\xCE\x99\xCE\x94\xCE\x91\xCE\xA3"), "\xCE\x91\xCF\x83\xCF\x80\xCE\xAF\xCE\xB4\xCE\xB1\xCF\x82"),
              "Greek: capitals without tonos find the card");  // ΑΣΠΙΔΑΣ
        Check(has(labels("\xCE\xBA\xCF\x81\xCF\x85\xCE\xBF"), "\xCE\x91\xCF\x83\xCF\x80\xCE\xAF\xCE\xB4\xCE\xB1\xCF\x82"), "Greek text search");  // κρυο
        Check(has(labels("kiem than"), "Ki\xE1\xBA\xBFm Th\xE1\xBA\xA7n") && has(labels("sat thuong"), "Ki\xE1\xBA\xBFm Th\xE1\xBA\xA7n"),
              "Vietnamese without marks finds the card");
        Check(FilterEntries(db2.pages[0], "SAT THUONG").size() == 1, "the filter folds Vietnamese");
        Check(has(labels("\xE5\x86\xB0\xE9\x9C\x9C"), "\xE7\x81\xAB\xE7\x84\xB0\xE3\x80\x81\xE5\x86\xB0\xE9\x9C\x9C"), "a two-ideograph word is found (no short-word rule)");
        Check(has(labels("\xEF\xBC\xB3\xEF\xBC\xB0\xEF\xBC\xA9\xEF\xBC\xB2\xEF\xBC\xA9\xEF\xBC\xB4"), "Spirit"), "full-width query finds Spirit");
        // short words match at word starts only
        Check(!has(labels("is"), "Resolve") && !BlockMatches(db2.pages[0].entries[0].blocks[2], "is"), "'is' does not match inside 'Resist'");
        Check(has(labels("is"), "Ist Charm") && BlockMatches(db2.pages[0].entries[0].blocks[3], "is"), "'is' matches the start of 'Ist'");
        Check(has(labels("+2 all"), "Spirit"), "'+2' matches at the start of '2'");
        Check(has(labels("sist"), "Resolve"), "three letters or more match inside a word");
        // de-duplication
        auto e = ix.Query(db2, "enigma", 0, 8);
        int enigmaRows = 0;
        for (const auto& h : e) if (h.label == "Enigma") ++enigmaRows;
        Check(enigmaRows == 1 && !e.empty() && e[0].block == 0 && e[0].entry == 1, "the card stands for its entry: one Enigma row");
        // fuzzy gating
        auto sh = ix.Query(db2, "shield", 0, 20);
        bool anyFuzzy = false;
        for (const auto& h : sh) anyFuzzy = anyFuzzy || !h.exact;
        Check(sh.size() >= 8 && !anyFuzzy, "eight or more exact hits: no typo pass");
        auto sp = ix.Query(db2, "spirt", 0, 20);
        Check(!sp.empty() && sp[0].label == "Spirit" && !sp[0].exact, "few exact hits: the typo pass runs");
        auto mixed = ix.Query(db2, "insigt", 0, 20);
        Check(!mixed.empty() && mixed[0].label == "Insight", "typo finds Insight");
        {
            // an exact body hit always ranks above every typo hit, even a strong one
            Database db3 = db2;
            db3.pages[0].entries[0].blocks[1].lines.push_back(Line{LineKind::Stat, "spirt of the old"});
            db3.pages[0].entries[0].blocks[1].foldedReady = false;  // the copied search cache is stale
            search::Index ix3;
            ix3.Build(db3);
            auto r = ix3.Query(db3, "spirt", 0, 20);
            bool order = !r.empty() && r[0].exact && r[0].label == "Insight";
            bool seenFuzzy = false;
            for (const auto& h : r) {
                if (!h.exact) seenFuzzy = true;
                else if (seenFuzzy) order = false;
            }
            Check(order && seenFuzzy, "fuzzy hits rank below every exact hit");
        }
        // count and hint
        Check(ix.CountMatches("faster cast") == 3, "count of exact matches");
        const search::QueryHint none = ix.Hint("teleport insight");
        Check(none.valid && none.matches == 0 && none.missing.empty() && none.required.size() == 2 && none.required[1] == "insight",
              "hint: no card has all of the terms, each exists");
        const search::QueryHint gone = ix.Hint("Teleport zzqx -cast");
        Check(gone.matches == 0 && gone.missing.size() == 1 && gone.missing[0] == "zzqx" && gone.excluded.size() == 1, "hint: the term found nowhere");
        Check(!ix.Hint("-cast").valid, "hint: only excluded is not a search");

        // ---- The filter and the suggestions agree on the same query ----
        const char* queries[] = {"faster cast", "\"faster cast\"", "cast -insight", "is", "+2 all", "monarch", "enigma", "teleport skills",
                                 "kiem", "\xCE\xBA\xCF\x81\xCF\x85\xCE\xBF", "\"spirit", "skills -\"faster cast\"", "runewords", "defense -light"};
        for (const char* q : queries) {
            std::set<int> fromFilter, fromIndex;
            for (std::size_t i : FilterEntries(db2.pages[0], q)) fromFilter.insert(static_cast<int>(i));
            for (const auto& h : ix.Query(db2, q, 0, 1000))
                if (h.exact && h.entry >= 0) fromIndex.insert(h.entry);
            if (fromFilter != fromIndex) std::cerr << "  query: " << q << "\n";
            Check(fromFilter == fromIndex, "filter and suggestions find the same entries");
            for (std::size_t b = 0; b < db2.pages[0].entries[0].blocks.size(); ++b) {
                bool inIndex = false;
                for (const auto& h : ix.Query(db2, q, 0, 1000)) inIndex = inIndex || (h.exact && h.entry == 0 && h.block == static_cast<int>(b));
                const bool inFilter = BlockMatches(db2.pages[0].entries[0].blocks[b], q);
                if (search::Parse(q).HasRequired() && inIndex != inFilter) {
                    std::cerr << "  query: " << q << " block " << b << "\n";
                    Check(false, "filter and suggestions agree on each card");
                }
            }
        }
    }

    // ---- Records from outside the Database: the Specializations board's perks (SetExtra), as the overlay gives them ----
    {
        const auto readPage = [](const std::string& path, spec::Data& out) {
            std::ifstream file(path, std::ios::binary);
            std::stringstream content;
            content << file.rdbuf();
            std::string why;
            return spec::Parse(content.str(), out, why);
        };
        // One record per perk, as OverlayState::SpecSearchRecords makes them: name, page > tab, the tooltip's lines.
        const auto perkRecords = [](const spec::Data& board, int page) {
            std::vector<search::ExtraRecord> records;
            const auto addLines = [](std::vector<std::string>& lines, const std::string& text) {
                std::size_t start = 0;
                while (start <= text.size()) {
                    const std::size_t end = std::min(text.find('\n', start), text.size());
                    if (end > start) lines.push_back(text.substr(start, end - start));
                    start = end + 1;
                }
            };
            for (std::size_t t = 0; t < board.tabs.size(); ++t)
                for (std::size_t i = 0; i < board.tabs[t].perks.size(); ++i) {
                    search::ExtraRecord record;
                    record.label = board.tabs[t].perks[i].name;
                    record.context = "Specializations \xE2\x80\xBA " + board.tabs[t].label;
                    addLines(record.lines, board.tabs[t].perks[i].text);
                    addLines(record.lines, board.tabs[t].perks[i].hint);
                    record.page = page;
                    record.group = static_cast<int>(t);
                    record.item = static_cast<int>(i);
                    records.push_back(std::move(record));
                }
            return records;
        };
        const std::string englishPath = argc > 1 ? argv[1] : "data/website-specializations/specializations.htm";
        const std::string germanPath = argc > 2 ? argv[2] : "data/locales/deDE/specializations.htm";
        spec::Data board;
        Check(readPage(englishPath, board), "perks: the website's Specializations page parses");
        Database sdb = Sample();
        Page specPage;  // the Database's page is only a placeholder, as in wiki-data.txt
        specPage.id = "specializations";
        specPage.title = "Specializations";
        Entry placeholder;
        placeholder.title = "Specializations";
        specPage.entries.push_back(placeholder);
        sdb.pages.push_back(specPage);
        const int specIndex = static_cast<int>(sdb.pages.size()) - 1;
        std::size_t perkCount = 0;
        for (const spec::Tab& tab : board.tabs) perkCount += tab.perks.size();

        search::Index ix;
        ix.Build(sdb);
        const std::size_t base = ix.Size();
        Check(ix.Query(sdb, "bottomless", 0).empty(), "perks: nothing before SetExtra");
        ix.SetExtra(perkRecords(board, specIndex));
        Check(ix.Size() == base + perkCount && ix.Extra().size() == perkCount, "perks: one record per perk");
        const auto perkName = [&](const search::Hit& h) {
            if (h.extraGroup < 0 || h.extraItem < 0 || static_cast<std::size_t>(h.extraGroup) >= board.tabs.size()) return std::string();
            const auto& perks = board.tabs[static_cast<std::size_t>(h.extraGroup)].perks;
            return static_cast<std::size_t>(h.extraItem) < perks.size() ? perks[static_cast<std::size_t>(h.extraItem)].name : std::string();
        };

        auto byName = ix.Query(sdb, "bottomless vials", 0);
        Check(!byName.empty() && byName[0].kind == search::HitKind::Perk && byName[0].label == "Bottomless Vials" && byName[0].exact,
              "perks: found by name, first");
        Check(!byName.empty() && perkName(byName[0]) == "Bottomless Vials" && byName[0].page == specIndex && byName[0].entry < 0 &&
                  byName[0].block < 0,
              "perks: the hit gives the tab and perk it was given, and the page");
        Check(!byName.empty() && byName[0].context == "Specializations \xE2\x80\xBA " + board.tabs[static_cast<std::size_t>(byName[0].extraGroup)].label,
              "perks: context is page > specialization");
        Check(!byName.empty() && byName[0].labelMarks.size() == 15 && byName[0].snippet.empty(), "perks: a title match marks the name");

        auto byText = ix.Query(sdb, "rejuvenation", 0, 100);
        bool transmutation = false, snippetOk = false;
        for (const auto& h : byText) {
            Check(h.kind == search::HitKind::Perk, "perks: description word finds only perks here");
            if (h.label == "Transmutation") {
                transmutation = true;
                snippetOk = h.snippet.find("rejuvenation") != std::string::npos && !h.snippetMarks.empty() && h.labelMarks.empty();
            }
        }
        Check(transmutation && snippetOk, "perks: a word of the description finds the perk, with the line as snippet");
        Check(byText.size() == ix.CountMatches("rejuvenation") && ix.Hint("rejuvenation").matches == byText.size(),
              "perks: counted like any record");

        auto folded = ix.Query(sdb, "S\xC3\x89RENDIPITY", 0);  // SÉRENDIPITY
        Check(!folded.empty() && folded[0].label == "Serendipity" && folded[0].exact, "perks: case and accents folded");
        auto typo = ix.Query(sdb, "serendipty", 0);
        Check(!typo.empty() && typo[0].label == "Serendipity" && !typo[0].exact, "perks: the typo pass finds a perk");

        auto vials = ix.Query(sdb, "vials -bottomless", 0, 100);
        bool blessed = false, bottomless = false;
        for (const auto& h : vials) {
            blessed = blessed || h.label == "Blessed Vials";
            bottomless = bottomless || h.label == "Bottomless Vials";
        }
        Check(blessed && !bottomless, "perks: -word excludes");
        auto phrase = ix.Query(sdb, "\"mana potions\" -healing", 0, 100);
        for (const auto& h : phrase) Check(h.snippet.find("ealing") == std::string::npos, "perks: phrase with exclusion");

        // Every perk has the respec line: counted in full, listed at most 6 per specialization (as cards per entry).
        Check(ix.CountMatches("\"respec points\"") == perkCount, "perks: phrase in the tooltip hint counts every perk");
        auto respec = ix.Query(sdb, "\"respec points\"", 0, 1000);
        Check(respec.size() == 6 * board.tabs.size(), "perks: at most 6 hits per specialization");

        // The in-page dimming: which perks match, as positions in Extra() (Spectral Arsenal, Ethereal Mastery, Ethereal Exchange).
        std::set<std::string> ethereal;
        for (std::size_t at : ix.MatchingExtra("ethereal -mastery"))
            if (at < ix.Extra().size()) ethereal.insert(ix.Extra()[at].label);
        Check(ethereal == std::set<std::string>{"Spectral Arsenal", "Ethereal Exchange"}, "perks: MatchingExtra for the in-page dimming");
        Check(ix.MatchingExtra("").empty(), "perks: no text matches nothing");

        // The database's own hits are unchanged.
        Check(!ix.Query(sdb, "spirit", 0).empty() && ix.Query(sdb, "spirit", 0)[0].label == "Spirit", "perks: database hits stay");
        ix.Build(sdb);  // a rebuilt database (language switch) keeps the perks until they are replaced
        Check(!ix.Query(sdb, "bottomless", 0).empty(), "perks: kept by Build");
        ix.SetExtra({});
        Check(ix.Query(sdb, "bottomless", 0).empty() && ix.Size() == base && ix.CountMatches("rejuvenation") == 0,
              "perks: gone when cleared");

        // Another language: the page's own words, nothing translated.
        spec::Data german;
        Check(readPage(germanPath, german), "perks: the German page parses");
        ix.SetExtra(perkRecords(german, specIndex));
        auto de = ix.Query(sdb, "fluchtige gebraue", 0);  // Fluechtige Gebraeue (accents folded)
        Check(!de.empty() && de[0].label == "Fl\xC3\xBC" "chtige Gebr\xC3\xA4" "ue", "perks: German name, umlauts folded");
        Check(ix.Query(sdb, "bottomless", 0).empty(), "perks: the English names are gone in German");
        ix.SetExtra(perkRecords(board, specIndex));
        Check(!ix.Query(sdb, "bottomless", 0).empty(), "perks: back to English");

        // Two sources side by side (the overlay: the perks are source 0, the campaign map's areas source 1). Each is replaced
        // on its own, hits say their source, and the 6 per group cap counts each source's groups apart.
        {
            ix.SetExtra(0, perkRecords(board, specIndex));
            std::vector<search::ExtraRecord> areas;
            for (int i = 0; i < 8; ++i) {
                search::ExtraRecord area;
                area.label = i == 0 ? "Zzyzx Hollow" : "Area " + std::to_string(i);
                area.context = "Welcome \xE2\x80\xBA Campaign Progression";
                area.lines = {"Qwertyx", "\xC3\xBF" "c1Ignored colour"};
                area.page = specIndex;
                area.group = 0;
                area.item = 100 + i;
                area.source = 7;  // overwritten by SetExtra(1, ...)
                areas.push_back(area);
            }
            ix.SetExtra(1, areas);
            Check(ix.Extra().size() == perkCount + 8 && ix.Extra().front().source == 0 && ix.Extra().back().source == 1,
                  "sources: both lists kept, in source order");
            const auto zz = ix.Query(sdb, "zzyzx", 0);
            Check(!zz.empty() && zz[0].kind == search::HitKind::Perk && zz[0].extraSource == 1 && zz[0].extraItem == 100 &&
                      zz[0].extraGroup == 0 && zz[0].context == "Welcome \xE2\x80\xBA Campaign Progression",
                  "sources: an area hit gives its source, group and item");
            const auto bv = ix.Query(sdb, "bottomless vials", 0);
            Check(!bv.empty() && bv[0].extraSource == 0 && bv[0].label == "Bottomless Vials", "sources: the perks are still found");
            Check(ix.Query(sdb, "qwertyx", 0, 100).size() == 6 && ix.CountMatches("qwertyx") == 8,
                  "sources: at most 6 hits per group, counted in full");
            Check(ix.CountMatches("ignored") == 8, "sources: colour codes removed from the lines");
            ix.SetExtra(0, perkRecords(german, specIndex));  // a new language for the perks keeps the areas
            Check(!ix.Query(sdb, "zzyzx", 0).empty() && ix.Query(sdb, "bottomless", 0).empty(), "sources: replacing one keeps the other");
            std::size_t areaMatches = 0;
            for (std::size_t at : ix.MatchingExtra("qwertyx"))
                if (at < ix.Extra().size() && ix.Extra()[at].source == 1) ++areaMatches;
            Check(areaMatches == 8, "sources: MatchingExtra gives positions of every source");
            ix.SetExtra(1, {});
            Check(ix.Query(sdb, "zzyzx", 0).empty() && !ix.Query(sdb, "fluchtige gebraue", 0).empty() && ix.Extra().size() == perkCount,
                  "sources: an empty list removes only its source");
            ix.Build(sdb);
            Check(!ix.Query(sdb, "fluchtige gebraue", 0).empty(), "sources: kept by Build");
            ix.SetExtra({});
            Check(ix.Extra().empty(), "sources: SetExtra(list) replaces every source");
        }
    }

    if (failures == 0) std::cout << "search_index tests passed\n";
    return failures == 0 ? 0 : 1;
}
