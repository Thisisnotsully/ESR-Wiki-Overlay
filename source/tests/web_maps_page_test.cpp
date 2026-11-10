// Tests for the official Maps page reader. Run with the data/website-maps folder and the data/locales folder as arguments.
#include "web_maps_page.hpp"

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
}  // namespace

int main(int argc, char** argv) {
    using namespace esrwiki::mapspage;
    std::string error;
    Page none;
    Check(!Parse("<html><p>nothing</p></html>", none, error), "a page without tables fails");

    const char* tiny =
        "<table><tr><td><b>Teleport</b></td></tr><tr><td><table>"
        "<tr><td><font>Forgotten\n   Tower</font></td><td>No &amp; walls</td></tr>"
        "<tr><td>Chaos</td><td>No</td></tr></table></td></tr></table>"
        "<table><tr><td colspan=3>Special</td></tr><tr>"
        "<td align=center height=\"120\"><font><b> Cow </b></font><br><font> Open<br><font color=x> Location: Act 1 X</font>"
        "<br><font> </font></font></td>"
        "<td HEIGHT=120><font>Careful<br>below</font></td></tr></table>";
    Page p;
    Check(Parse(tiny, p, error), "tiny page parses");
    Check(p.teleport.size() == 2 && p.teleport[0].area == "Forgotten Tower" && p.teleport[0].text == "No & walls",
          "teleport rows, whitespace and entities");
    Check(p.teleportTitle == "Teleport", "teleport title");
    Check(p.cards.size() == 2 && p.cards[0].name == "Cow" && p.cards[0].size == "Open" &&
              p.cards[0].location == "Location: Act 1 X", "card text nodes in order");
    Check(p.cards.size() == 2 && p.cards[1].note && p.cards[1].lines.size() == 2, "note cell");
    Check(p.specialTitle == "Special", "special title");

    if (argc > 1) {
        const std::string dir = argv[1];
        const char* pages[] = {"maps.htm", "locales/deDE/maps.htm", "locales/jaJP/maps.htm", "locales/ruRU/maps.htm"};
        for (const char* name : pages) {
            const std::string html = ReadFile(dir + "/" + name);
            Check(!html.empty(), std::string(name) + " read");
            Page page;
            Check(Parse(html, page, error), std::string(name) + " parses");
            Check(page.teleport.size() == 5, std::string(name) + ": 5 teleport rows (" + std::to_string(page.teleport.size()) + ")");
            Check(page.cards.size() == 18, std::string(name) + ": 18 cards (" + std::to_string(page.cards.size()) + ")");
            int named = 0, notes = 0;
            for (const SpecialCard& c : page.cards) {
                if (c.note) ++notes;
                else {
                    ++named;
                    Check(!c.name.empty() && !c.size.empty() && !c.location.empty(), std::string(name) + ": card has three lines");
                }
            }
            Check(named == 17 && notes == 1, std::string(name) + ": 17 areas and the closing note");
            Check(!page.teleportTitle.empty() && !page.specialTitle.empty(), std::string(name) + ": section titles");
            if (std::string(name) == "maps.htm") {
                Check(page.teleport.size() == 5 && page.teleport[0].area == "Forgotten Tower Level 5" &&
                          page.teleport[0].text == "No Teleport across the walls", "English first teleport row");
                Check(page.teleport.size() == 5 && page.teleport[4].area == "Chaos Sanctuary", "English last teleport row");
                Check(page.cards.size() == 18 && page.cards[1].name == "Blood Raven's Workshop" &&
                          page.cards[1].size == "Level 1 and Level 2" &&
                          page.cards[1].location == "Location: Act 1 Tamoe Highland", "English card");
                Check(page.teleportTitle == "Teleport Restrictions" && page.specialTitle == "Special Dungeons/Areas", "English titles");
            }
            if (std::string(name) == "locales/deDE/maps.htm")
                Check(page.cards.size() == 18 && page.cards[1].name == "Blutrabes Werkstatt", "German card name");
        }
    }
    // The copies the plugin ships: data/locales/<code>/maps.htm for all twelve translated languages (the English one is
    // checked above). Every one is the Maps page: 5 teleport rows and 18 cells, with every name and line filled in.
    if (argc > 2) {
        const std::string locales = argv[2];
        const char* codes[] = {"deDE", "esES", "esMX", "frFR", "itIT", "jaJP", "koKR", "plPL", "ptBR", "ruRU", "zhCN", "zhTW"};
        for (const char* code : codes) {
            const std::string name = std::string(code) + "/maps.htm";
            const std::string html = ReadFile(locales + "/" + name);
            Check(!html.empty(), name + " read");
            Page page;
            Check(Parse(html, page, error), name + " parses");
            Check(page.teleport.size() == 5, name + ": 5 teleport rows (" + std::to_string(page.teleport.size()) + ")");
            Check(page.cards.size() == 18, name + ": 18 cells (" + std::to_string(page.cards.size()) + ")");
            for (const TeleportRow& row : page.teleport) Check(!row.area.empty() && !row.text.empty(), name + ": teleport row has area and text");
            int named = 0, notes = 0;
            for (const SpecialCard& c : page.cards) {
                if (c.note) {
                    ++notes;
                    Check(!c.lines.empty(), name + ": the note has text");
                } else {
                    ++named;
                    Check(!c.name.empty() && !c.size.empty() && !c.location.empty(), name + ": card has name, size and location");
                }
            }
            Check(named == 17 && notes == 1, name + ": 17 areas and the closing note");
            Check(!page.teleportTitle.empty() && !page.specialTitle.empty(), name + ": section titles");
            Check(page.teleportTitle != "Teleport Restrictions", name + ": the translated page, not the English one");
        }
    }
    if (failures == 0) std::cout << "web_maps_page tests passed\n";
    return failures == 0 ? 0 : 1;
}
