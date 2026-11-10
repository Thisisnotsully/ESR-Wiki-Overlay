// The official Maps page of the ESR website (maps.htm, and the translated copies <code>/maps.htm), as data without
// any Win32: the Teleport Restrictions rows and the Special Dungeons/Areas cards.
//
// The page is an old <table>/<font> layout without classes. The reader goes by structure only, never by the words,
// so the translated pages read the same way:
//   - Teleport Restrictions: the first innermost table whose every row has exactly two cells (area, restriction).
//   - Special Dungeons/Areas: every <td height="120"> cell. A cell with a bold title is a card: its text nodes in
//     order are the name, the size line ("Level 1 and Level 2") and the location line ("Location: Act 1 ...").
//     A cell without a bold title is the section's closing note (its text nodes are kept as lines).
// Text nodes are unescaped and whitespace-collapsed. Not wired to the overlay yet. Platform-neutral.
#pragma once

#include <string>
#include <vector>

namespace esrwiki::mapspage {

inline constexpr const char* kPageName = "maps.htm";

struct TeleportRow {
    std::string area;  // "Forgotten Tower Level 5"
    std::string text;  // "No Teleport across the walls"
};

struct SpecialCard {
    bool note = false;       // the closing note cell (no title): name, size and location are empty
    std::string name;        // "Blood Raven's Workshop"
    std::string size;        // "Level 1 and Level 2"
    std::string location;    // "Location: Act 1 Tamoe Highland" (the whole line, in the page's language)
    std::vector<std::string> lines;  // every text node of the cell, in order
};

struct Page {
    std::string teleportTitle;  // the heading cell of the teleport table ("Teleport Restrictions")
    std::vector<TeleportRow> teleport;
    std::string specialTitle;   // the heading cell of the cards' table ("Special Dungeons/Areas")
    std::vector<SpecialCard> cards;  // in page order, the note cell included
};

// Text nodes of an HTML fragment in order (tags dropped, entities unescaped, whitespace collapsed, empty ones skipped).
std::vector<std::string> TextNodes(const std::string& html);

// Reads the page. Fails, leaving `out` untouched, when it has neither teleport rows nor special-area cards.
bool Parse(const std::string& html, Page& out, std::string& error);

}  // namespace esrwiki::mapspage
