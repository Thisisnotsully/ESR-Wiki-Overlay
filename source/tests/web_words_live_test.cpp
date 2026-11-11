// Checks that the C++ website-words builders produce exactly what tools/web_words.py writes.
//   web_words_live_test <runewords.htm> <gemwords.htm> <gems.htm> <web-word-art.txt> <wiki-data.txt with both website pages>
//                       [<folder with the older six-cell runewords.htm, gemwords.htm and expected-wiki-data.txt>]
// The saved pages are the five-cell layout (Bonuses / Socketable Bonuses, since November 2026); the optional folder
// holds pages of the older six-cell layout (three per-slot columns), which must still give what the Python tool wrote.
#include <fstream>
#include <iostream>
#include <sstream>

#include "web_words_live.hpp"

static std::string ReadAll(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

static std::string WithoutPage(const std::string& text, const std::string& id) {
    const std::string marker = "PAGE\t" + id + "\t";
    const std::size_t begin = text.find("\n" + marker);
    if (begin == std::string::npos) return text;
    const std::size_t end = text.find("\nPAGE\t", begin + 1);
    return text.substr(0, begin) + (end == std::string::npos ? std::string() : text.substr(end));
}

#define CHECK(cond, msg) do { if (!(cond)) { std::cerr << "FAIL: " << msg << "\n"; return 1; } } while (0)

int main(int argc, char** argv) {
    if (argc != 6 && argc != 7) {
        std::cerr << "usage: web_words_live_test runewords.htm gemwords.htm gems.htm web-word-art.txt wiki-data.txt [old-layout folder]\n";
        return 2;
    }
    const std::string rw = ReadAll(argv[1]), gw = ReadAll(argv[2]), gems = ReadAll(argv[3]), art = ReadAll(argv[4]);
    const std::string expected = ReadAll(argv[5]);
    std::string out, error;
    CHECK(esrwiki::BuildWebRunewordsWikiData(expected, rw, gems, art, out, error), "runewords: " << error);
    if (out != expected) { std::ofstream("web_runewords_actual.txt", std::ios::binary) << out; std::cerr << "FAIL: runewords differ (web_runewords_actual.txt)\n"; return 1; }
    CHECK(esrwiki::BuildWebGemwordsWikiData(expected, gw, art, out, error), "gemwords: " << error);
    if (out != expected) { std::ofstream("web_gemwords_actual.txt", std::ios::binary) << out; std::cerr << "FAIL: gemwords differ (web_gemwords_actual.txt)\n"; return 1; }
    // Insert paths: data without the page gets it back in the same place.
    std::string without = WithoutPage(expected, "gemwords");
    CHECK(without != expected, "test setup: gemwords not removed");
    CHECK(esrwiki::BuildWebGemwordsWikiData(without, gw, art, out, error), "gemwords insert: " << error);
    CHECK(out == expected, "inserted gemwords page is not in the original place");
    without = WithoutPage(WithoutPage(expected, "gemwords"), "runewords");
    CHECK(esrwiki::BuildWebRunewordsWikiData(without, rw, gems, art, out, error), "runewords insert: " << error);
    std::string step;
    CHECK(esrwiki::BuildWebGemwordsWikiData(out, gw, art, step, error), "gemwords after runewords: " << error);
    CHECK(step == expected, "inserted pages are not in the place of the originals");
    // The five-cell layout: the page's own headings, the website's required level, socketable bonus groups under their
    // labels (LINE L), plain socketable bonuses without one.
    CHECK(expected.find("LINE\tS\t\xC3\xBF" "c4Bonuses\nLINE\tS\t\xC3\xBF" "c3+10 to Minimum Damage to Attacks\n") != std::string::npos,
          "Boar: bonuses heading");
    CHECK(expected.find("LINE\tS\t\xC3\xBF" "c4Socketable Bonuses\nLINE\tL\tWeapon (except Orb, Staff, Wand) / Missile / Mythical Arcane Staff\n"
                        "LINE\tS\t\xC3\xBF" "c3+15% Enhanced Damage\nLINE\tL\tStaff / Wand / Orb\nLINE\tS\t\xC3\xBF" "c3+30 Defense\n") != std::string::npos,
          "Boar: socketable bonus groups");
    CHECK(expected.find("LINE\tS\t\xC3\xBF" "c4Socketable Bonuses\nLINE\tS\t\xC3\xBF" "c3Adds 75 Poison Damage to Attacks over 2 Seconds\n") != std::string::npos,
          "Melody: plain socketable bonuses");
    CHECK(expected.find("Rune Bonuses:") == std::string::npos, "the old rune bonus heading is left");
    if (argc == 7) {  // the older six-cell layout
        const std::string folder = argv[6];
        const std::string oldRw = ReadAll(folder + "/runewords.htm"), oldGw = ReadAll(folder + "/gemwords.htm");
        const std::string oldExpected = ReadAll(folder + "/expected-wiki-data.txt");
        CHECK(!oldRw.empty() && !oldGw.empty() && !oldExpected.empty(), "old layout: fixture missing");
        std::string oldStep;
        CHECK(esrwiki::BuildWebRunewordsWikiData("PAGE\trunes\tRunes\tWiki\n", oldRw, gems, art, oldStep, error), "old runewords: " << error);
        CHECK(esrwiki::BuildWebGemwordsWikiData(oldStep, oldGw, art, out, error), "old gemwords: " << error);
        if (out != oldExpected) { std::ofstream("web_words_old_actual.txt", std::ios::binary) << out; std::cerr << "FAIL: old layout differs (web_words_old_actual.txt)\n"; return 1; }
        CHECK(out.find("Rune Bonuses:") != std::string::npos && out.find("\nLINE\tL\t") == std::string::npos, "old layout: per-slot columns");
    }
    // Broken, cut-off and wrong pages are refused.
    const std::string junk = "<html><body><table><tr><td>x</td></tr></table></body></html>";
    CHECK(!esrwiki::BuildWebRunewordsWikiData(expected, junk, gems, art, out, error), "broken runewords accepted");
    CHECK(!esrwiki::BuildWebRunewordsWikiData(expected, rw.substr(0, rw.size() / 2), gems, art, out, error), "cut-off runewords accepted");
    CHECK(!esrwiki::BuildWebRunewordsWikiData(expected, rw, junk, art, out, error), "broken gems page accepted");
    CHECK(!esrwiki::BuildWebGemwordsWikiData(expected, junk, art, out, error), "broken gemwords accepted");
    CHECK(!esrwiki::BuildWebGemwordsWikiData(expected, gw.substr(0, gw.size() / 2), art, out, error), "cut-off gemwords accepted");
    std::cout << "web_words_live_test: all checks passed\n";
    return 0;
}
