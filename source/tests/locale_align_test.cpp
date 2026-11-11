// Checks locale_align: text nodes, alignment, translation of wiki data. With a data folder as argument it also
// translates the shipped English wiki data from the saved French pages and checks the amount translated.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "locale_align.hpp"

using namespace esrwiki::localize;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static std::string Read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int main(int argc, char** argv) {
    // text nodes: entities, white space, script and style
    {
        const auto nodes = ExtractTextNodes("<html><style>p{}</style><body><p class=\"a b\"> Hello&nbsp; &amp; <b>wor&#108;d</b></p><script>var x='<p>no</p>';</script><br/>1 &lt; 2</body></html>");
        CHECK(nodes.size() == 3);
        CHECK(nodes.size() == 3 && nodes[0].text == "Hello &" && nodes[1].text == "world" && nodes[2].text == "1 < 2");
    }
    {
        const auto nodes = ExtractTextNodes("<p>  a \n b  </p><p>x</p>");
        CHECK(nodes.size() == 2 && nodes[0].text == "a b" && nodes[1].text == "x");
    }
    // equal counts pair by position
    {
        const auto pairs = AlignNodes(ExtractTextNodes("<p>One</p><p>Two</p>"), ExtractTextNodes("<p>Un</p><p>Deux</p>"));
        CHECK(pairs.size() == 2 && pairs[0].second == "Un" && pairs[1].second == "Deux");
    }
    // an extra node in the localized page: pairing resumes after it
    {
        std::string en, fr;
        for (int i = 0; i < 20; ++i) { en += "<td class=\"c\">Word " + std::to_string(i) + "</td>"; }
        for (int i = 0; i < 20; ++i) {
            if (i == 8) fr += "<td class=\"extra\">Plus</td>";
            fr += "<td class=\"c\">Mot " + std::to_string(i) + "</td>";
        }
        const auto pairs = AlignNodes(ExtractTextNodes(en), ExtractTextNodes(fr));
        CHECK(pairs.size() == 20);
        CHECK(!pairs.empty() && pairs.back().first == "Word 19" && pairs.back().second == "Mot 19");
    }
    // translating wiki data: exact, numbers, pieces, colour codes, the tier column stays
    {
        Translator t;
        t.AddPage("<p>Fire Resist</p><p>+(5-10)% Enhanced Damage</p><p>Helm</p><p>Gloves</p><p>Normal</p>",
                  "<p>Resistance au feu</p><p>+(5-10)% Degats ameliores</p><p>Casque</p><p>Gants</p><p>Normal</p>");
        t.Finish();
        const std::string data = "ENTRY\tFire Resist\tHelm\nBLOCK\t1\tHelm\t\t\tNormal\tGloves\t0\nLINE\tS\t\xC3\xBF" "c3+(20-30)% Enhanced Damage\nLINE\tS\tHelm, Gloves\nLINE\tS\tNothing here\n";
        TranslateStats stats;
        const std::string out = t.Translate(data, &stats);
        CHECK(out.find("ENTRY\tResistance au feu\tCasque") == 0);
        CHECK(out.find("+(20-30)% Degats ameliores") != std::string::npos);
        CHECK(out.find("\xC3\xBF" "c3+(20-30)% Degats") != std::string::npos);
        CHECK(out.find("Casque, Gants") != std::string::npos);
        CHECK(out.find("Nothing here") != std::string::npos);  // not on the website: stays as it is
        CHECK(out.find("\tNormal\tGants\t") != std::string::npos);
    }
    // Numbers go where the language puts them. The Japanese website names the level first and the chance last; the
    // pair of the website's own texts tells which number is which, by value (24 and 33), else by what stands around
    // them (a percent sign, the word for "level"), never only by their order.
    {
        Translator t;
        t.AddPage("<p>33% Chance to Cast Level 24 Twister on Melee Attack</p><p>+30 Defense</p>",
                  "<p>" "melee \xE6\x94\xBB\xE6\x92\x83\xE6\x99\x82\xE3\x81\xAB\xE3\x83\xAC\xE3\x83\x99\xE3\x83\xAB 24 \xE3\x81\xAE \xE3\x83\x84\xE3\x82\xA4\xE3\x82\xB9\xE3\x82\xBF\xE3\x83\xBC \xE3\x82\x92\xE7\x99\xBA\xE5\x8B\x95\xE3\x81\x99\xE3\x82\x8B 33% \xE3\x81\xAE\xE7\xA2\xBA\xE7\x8E\x87" "</p><p>" "\xE9\x98\xB2\xE5\xBE\xA1\xE5\x8A\x9B +30" "</p>");
        t.Finish();
        const std::string out = t.Translate("LINE\tS\t17% Chance to Cast Level 20 Twister on Melee Attack\nLINE\tS\t+5 Defense 17% Chance to Cast Level 20 Twister on Melee Attack");
        CHECK(out.find("LINE\tS\t" "melee \xE6\x94\xBB\xE6\x92\x83\xE6\x99\x82\xE3\x81\xAB\xE3\x83\xAC\xE3\x83\x99\xE3\x83\xAB 20 \xE3\x81\xAE \xE3\x83\x84\xE3\x82\xA4\xE3\x82\xB9\xE3\x82\xBF\xE3\x83\xBC \xE3\x82\x92\xE7\x99\xBA\xE5\x8B\x95\xE3\x81\x99\xE3\x82\x8B 17% \xE3\x81\xAE\xE7\xA2\xBA\xE7\x8E\x87" "\n") == 0);
        CHECK(out.find("LINE\tS\t" "\xE9\x98\xB2\xE5\xBE\xA1\xE5\x8A\x9B +5 melee \xE6\x94\xBB\xE6\x92\x83\xE6\x99\x82\xE3\x81\xAB\xE3\x83\xAC\xE3\x83\x99\xE3\x83\xAB 20 \xE3\x81\xAE \xE3\x83\x84\xE3\x82\xA4\xE3\x82\xB9\xE3\x82\xBF\xE3\x83\xBC \xE3\x82\x92\xE7\x99\xBA\xE5\x8B\x95\xE3\x81\x99\xE3\x82\x8B 17% \xE3\x81\xAE\xE7\xA2\xBA\xE7\x8E\x87") != std::string::npos);  // a cell joined from pieces
    }
    {  // the same value twice: the percent sign and the word "level" tell them apart
        Translator t;
        t.AddPage("<p>5% Chance to Cast Level 5 Twister on Melee Attack</p>", "<p>" "melee \xE6\x94\xBB\xE6\x92\x83\xE6\x99\x82\xE3\x81\xAB\xE3\x83\xAC\xE3\x83\x99\xE3\x83\xAB 5 \xE3\x81\xAE \xE3\x83\x84\xE3\x82\xA4\xE3\x82\xB9\xE3\x82\xBF\xE3\x83\xBC \xE3\x82\x92\xE7\x99\xBA\xE5\x8B\x95\xE3\x81\x99\xE3\x82\x8B 5% \xE3\x81\xAE\xE7\xA2\xBA\xE7\x8E\x87" "</p>");
        t.Finish();
        CHECK(t.Translate("LINE\tS\t17% Chance to Cast Level 20 Twister on Melee Attack") == "LINE\tS\t" "melee \xE6\x94\xBB\xE6\x92\x83\xE6\x99\x82\xE3\x81\xAB\xE3\x83\xAC\xE3\x83\x99\xE3\x83\xAB 20 \xE3\x81\xAE \xE3\x83\x84\xE3\x82\xA4\xE3\x82\xB9\xE3\x82\xBF\xE3\x83\xBC \xE3\x82\x92\xE7\x99\xBA\xE5\x8B\x95\xE3\x81\x99\xE3\x82\x8B 17% \xE3\x81\xAE\xE7\xA2\xBA\xE7\x8E\x87");
    }
    {  // the website shows other values than the English page: the marks still tell
        Translator t;
        t.AddPage("<p>33% Chance to Cast Level 24 Twister on Melee Attack</p>", "<p>" "melee \xE6\x94\xBB\xE6\x92\x83\xE6\x99\x82\xE3\x81\xAB\xE3\x83\xAC\xE3\x83\x99\xE3\x83\xAB 30 \xE3\x81\xAE \xE3\x83\x84\xE3\x82\xA4\xE3\x82\xB9\xE3\x82\xBF\xE3\x83\xBC \xE3\x82\x92\xE7\x99\xBA\xE5\x8B\x95\xE3\x81\x99\xE3\x82\x8B 40% \xE3\x81\xAE\xE7\xA2\xBA\xE7\x8E\x87" "</p>");
        t.Finish();
        CHECK(t.Translate("LINE\tS\t17% Chance to Cast Level 20 Twister on Melee Attack") == "LINE\tS\t" "melee \xE6\x94\xBB\xE6\x92\x83\xE6\x99\x82\xE3\x81\xAB\xE3\x83\xAC\xE3\x83\x99\xE3\x83\xAB 20 \xE3\x81\xAE \xE3\x83\x84\xE3\x82\xA4\xE3\x82\xB9\xE3\x82\xBF\xE3\x83\xBC \xE3\x82\x92\xE7\x99\xBA\xE5\x8B\x95\xE3\x81\x99\xE3\x82\x8B 17% \xE3\x81\xAE\xE7\xA2\xBA\xE7\x8E\x87");
    }
    {  // the same order in both languages stays as it is
        Translator t;
        t.AddPage("<p>Adds 10-20 Fire Damage and 5% Life</p>", "<p>Ajoute 10-20 degats de feu et 5% de vie</p>");
        t.Finish();
        CHECK(t.Translate("LINE\tS\tAdds 3-4 Fire Damage and 7% Life") == "LINE\tS\tAjoute 3-4 degats de feu et 7% de vie");
    }
    // A text the website joins from two lines of the English page: the first line takes the whole translation and the
    // second is consumed (translated to nothing); a LINE that is only that second text is dropped, but only right after
    // another LINE, and a real translation of the same text elsewhere wins.
    {
        std::string en, fr;
        for (int i = 0; i < 8; ++i) { en += "<td class=\"c\">Word " + std::to_string(i) + "</td>"; fr += "<td class=\"c\">Mot " + std::to_string(i) + "</td>"; }
        en += "<p class=\"d\">Gain 5 life</p><p class=\"d\">for 7 seconds</p>";
        fr += "<p class=\"d\">Gagne 5 vie pendant 7 secondes</p>";
        for (int i = 8; i < 16; ++i) { en += "<td class=\"c\">Word " + std::to_string(i) + "</td>"; fr += "<td class=\"c\">Mot " + std::to_string(i) + "</td>"; }
        const auto pairs = AlignNodesSequential(ExtractTextNodes(en), ExtractTextNodes(fr));
        bool whole = false, consumed = false;
        for (const auto& p : pairs) {
            if (p.first == "Gain 5 life" && p.second == "Gagne 5 vie pendant 7 secondes") whole = true;
            if (p.first == "for 7 seconds" && p.second.empty()) consumed = true;
        }
        CHECK(whole && consumed);
        Translator t;
        t.AddPage(en, fr);
        t.Finish();
        const std::string out = t.Translate("LINE\tS\tGain 5 life\nLINE\tS\tfor 7 seconds\nFIELD\tx\ty\nLINE\tS\tfor 7 seconds\nLINE\tS\tWord 3");
        CHECK(out == "LINE\tS\tGagne 5 vie pendant 7 secondes\nFIELD\tx\ty\nLINE\tS\tfor 7 seconds\nLINE\tS\tMot 3");
        Translator both;
        both.AddPage(en, fr);
        both.AddPage("<p>for 7 seconds</p>", "<p>pendant 7 secondes</p>");
        both.Finish();
        CHECK(both.Translate("LINE\tS\tGain 5 life\nLINE\tS\tfor 7 seconds") == "LINE\tS\tGagne 5 vie pendant 7 secondes\nLINE\tS\tpendant 7 secondes");
    }
    // The words for "level" that mark a number, in every language of the website (the website gives other values than
    // the English page here, so only the marks tell the numbers apart): abbreviations, capitals and accents, and the
    // Chinese word after the number.
    {
        const auto roleCase = [&](const std::string& localized, const std::string& expected) {
            Translator t;
            t.AddPage("<p>10% Chance to Cast Level 5 Fire Bolt</p>", "<p>" + localized + "</p>");
            t.Finish();
            const std::string out = t.Translate("LINE\tS\t3% Chance to Cast Level 9 Fire Bolt");
            if (out != "LINE\tS\t" + expected) { std::printf("FAIL level word case: %s -> %s\n", localized.c_str(), out.c_str()); ++failures; }
        };
        roleCase("Lancia liv. 7 Bolt con 20% di probabilita", "Lancia liv. 9 Bolt con 3% di probabilita");
        roleCase("Lance niv. 7 Bolt avec 20% de chance", "Lance niv. 9 Bolt avec 3% de chance");
        roleCase("Rzuca Poz. 7 Bolt z szansa 20%", "Rzuca Poz. 9 Bolt z szansa 3%");
        roleCase("Rzuca poz. 7 Bolt z szansa 20%", "Rzuca poz. 9 Bolt z szansa 3%");
        roleCase("\xD0\xA3\xD1\x80. 7 Bolt 20% chance", "\xD0\xA3\xD1\x80. 9 Bolt 3% chance");
        roleCase("\xD1\x83\xD1\x80. 7 Bolt 20% chance", "\xD1\x83\xD1\x80. 9 Bolt 3% chance");
        roleCase("N\xC3\x8DVEL 7 Bolt com 20% de chance", "N\xC3\x8DVEL 9 Bolt com 3% de chance");
        roleCase("Lv. 7 Bolt with 20% chance", "Lv. 9 Bolt with 3% chance");
        roleCase("Cast 7\xE7\xBA\xA7 Bolt 20%", "Cast 9\xE7\xBA\xA7 Bolt 3%");
        roleCase("Cast 7 \xE7\xB4\x9A Bolt 20%", "Cast 9 \xE7\xB4\x9A Bolt 3%");
        // "lv" inside another word is not a level word
        Translator t;
        t.AddPage("<p>Gain 10% damage and 5 Life</p>", "<p>Wolves 7 de vie et 20% degats</p>");
        t.Finish();
        CHECK(t.Translate("LINE\tS\tGain 3% damage and 8 Life") == "LINE\tS\tWolves 8 de vie et 3% degats");
    }
    // a node key without a digits part (no '|') does not disagree with anything
    {
        std::vector<TextNode> a, b;
        for (int i = 0; i < 8; ++i) {
            a.push_back(TextNode{"Word " + std::to_string(i), "f" + std::to_string(i)});
            b.push_back(TextNode{"Mot " + std::to_string(i), "f" + std::to_string(i)});
        }
        a.push_back(TextNode{"Odd", "p1"});
        b.push_back(TextNode{"Impair", "p2"});
        for (int i = 8; i < 16; ++i) {
            a.push_back(TextNode{"Word " + std::to_string(i), "f" + std::to_string(i)});
            b.push_back(TextNode{"Mot " + std::to_string(i), "f" + std::to_string(i)});
        }
        const auto pairs = AlignNodesSequential(a, b);
        bool odd = false;
        for (const auto& p : pairs) odd = odd || (p.first == "Odd" && p.second == "Impair");
        CHECK(odd);
    }
    // saved copies: the English data translated from the French pages
    if (argc > 1) {
        const std::string data = argv[1];
        // The Japanese set items: "33% Chance to Cast Level 24 Twister" came out as level 33 with a 24% chance.
        {
            Translator ja;
            ja.AddPage(Read(data + "/website-words/runewords.htm"), Read(data + "/locale-pages/jaJP/runewords.htm"));
            ja.AddPage(Read(data + "/website-sets/sets.htm"), Read(data + "/locale-pages/jaJP/sets.htm"));
            ja.Finish();
            const std::string out = ja.Translate("LINE\tS\t\xC3\xBF" "c333% Chance to Cast Level 24 Twister on Melee Attack");
            CHECK(out == "LINE\tS\t\xC3\xBF" "c3" "melee \xE6\x94\xBB\xE6\x92\x83\xE6\x99\x82\xE3\x81\xAB\xE3\x83\xAC\xE3\x83\x99\xE3\x83\xAB 24 \xE3\x81\xAE \xE3\x83\x84\xE3\x82\xA4\xE3\x82\xB9\xE3\x82\xBF\xE3\x83\xBC \xE3\x82\x92\xE7\x99\xBA\xE5\x8B\x95\xE3\x81\x99\xE3\x82\x8B 33% \xE3\x81\xAE\xE7\xA2\xBA\xE7\x8E\x87");
        }
        // The Chinese unique armors have 13 nodes fewer than the English page (two lines of a description in one node):
        // Stormshield pairs with its own name, never with the name of its base ("Monarch (uit)").
        {
            const auto pairs = AlignNodes(ExtractTextNodes(Read(data + "/website-uniques/unique_armors.htm")),
                                          ExtractTextNodes(Read(data + "/locale-pages/zhCN/unique_armors.htm")));
            bool right = false, wrong = false;
            for (const auto& p : pairs) {
                if (p.first != "Stormshield") continue;
                if (p.second == "\xE6\x9A\xB4\xE9\xA3\x8E\xE4\xB9\x8B\xE7\x9B\xBE") right = true;
                else wrong = true;
            }
            CHECK(right && !wrong);
        }
        Translator t;
        for (const char* page : {"armors.htm", "gems.htm", "prefixes.htm", "suffixes.htm", "unique_armors.htm", "weapons.htm"}) {
            std::string en;
            for (const char* dir : {"website-bases", "website-runes", "website-affixes", "website-uniques"}) {
                en = Read(data + "/" + dir + "/" + page);
                if (!en.empty()) break;
            }
            const std::string fr = Read(data + "/locale-pages/frFR/" + page);
            CHECK(!en.empty() && !fr.empty());
            t.AddPage(en, fr);
        }
        t.Finish();
        CHECK(t.Size() > 5000);
        TranslateStats stats;
        t.Translate(Read(data + "/wiki-data.txt"), &stats);
        std::printf("%zu of %zu cells translated\n", stats.translated, stats.cellsWithWords);
        CHECK(stats.translated > 50000);
    }
    if (failures == 0) std::printf("locale_align_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
