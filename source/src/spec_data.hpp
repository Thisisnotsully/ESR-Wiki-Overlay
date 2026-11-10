// The Specializations page of the ESR website (specializations.htm), as data without any Win32.
//
// The page is a board: a title, four tabs (one per specialization) and, on each tab, five rows of three perks.
// Hovering a perk shows its tooltip. Everything the overlay draws, the words and the positions, comes from the page, so a
// changed perk, a new tab or a translated page (<code>/specializations.htm) reaches the game without a new build.
#pragma once

#include <string>
#include <vector>

namespace esrwiki::spec {

struct Perk {
    std::string name;   // the tooltip title
    std::string text;   // what the perk does (lines separated by \n)
    std::string hint;   // "Shift + Click to Unlearn." and the respec cost (lines separated by \n)
    double x = 0, y = 0;  // top left of the perk on the board, in the page's pixels
    int icon = 0;         // which picture of the tab's sprite sheet (the sheet is 15 pictures side by side)
};

struct Tab {
    std::string label;  // the tab's text
    std::string sheet;  // the sprite sheet's name: alchemist, blacksmith, jeweler, archaeologist
    double left = 0;    // the tab's left edge on the board
    std::vector<Perk> perks;
};

struct Data {
    std::string title;                // "Specializations"
    std::string hint;                 // the line under the board
    std::vector<std::string> intro;   // the explanation above the board, one entry per line (D2 colour codes inside)
    std::vector<Tab> tabs;
    int defaultTab = 0;               // the tab the page opens on
    // The board's measures, from the page's style (these are the page's own values when it has none).
    double width = 639.5, height = 720;
    double perkWidth = 66, perkHeight = 65;
    double tabTop = 75, tabWidth = 140, tabHeight = 42;
    double titleLeft = 47.5, titleTop = 28, titleWidth = 544.5, titleHeight = 35.5;
    double hintLeft = 170, hintTop = 654.5, hintWidth = 300, hintHeight = 29;
};

// The website page, and the folder-less name the localized copies share (<code>/specializations.htm).
inline constexpr const char* kPageName = "specializations.htm";

// Reads the page. Fails, leaving `out` untouched, when it has no board or no perks.
bool Parse(const std::string& html, Data& out, std::string& error);

}  // namespace esrwiki::spec
