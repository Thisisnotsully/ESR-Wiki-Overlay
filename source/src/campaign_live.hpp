// The official Maps page (web_maps_page.hpp) matched to the Campaign Progression map (campaign_data.hpp), as data
// without any Win32. The plugin reads the page in the loader's language (the shipped copy first, then the website's
// live page), matches it here on its own thread and hands the result to the overlay, which then shows the website's
// own wording for the Special areas cards and for the teleport caveats of the matching areas.
//
// Matching, for every teleport row and every special-area card of the page:
//   1. By name. Names are compared case- and accent-folded with punctuation as spaces (text_fold.hpp). A row matches
//      the areas that have that name in any language, as their official (Maps page) name, or, for the English and
//      official names, the same name without a trailing "(...)" or "Level N" (so "Naraku" finds Naraku Level 1-3).
//      A card (it may stand for several levels) then also takes every area whose English name without "Level N" is
//      the same as one it found; a teleport row (one level) does not.
//   2. When the name finds nothing and an English copy of the page is given with as many rows (or cards) as this
//      page, the row at the same position on the English page is matched by name instead. The translated pages are
//      the English page's layout with the text translated, so this covers names the game words differently.
// Rows and cards that still match nothing are listed (unmatchedTeleport, unmatchedCards); the overlay shows them as
// notes under the Special areas tab. Nothing here changes the page's text. Platform-neutral.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "campaign_data.hpp"
#include "web_maps_page.hpp"

namespace esrwiki::campaign {

// The website page that the shipped maps.htm is a saved copy of (the translated one is <code>/ + this name).
inline constexpr const char* kMapsSitePage = "Eastern%20Sun%20Resurrected%20Maps.html";

struct LiveMaps {
    mapspage::Page page;                                // as parsed, the website's wording untouched
    std::map<int, std::size_t> teleportOf;              // area id -> index into page.teleport
    std::map<int, std::size_t> cardOf;                  // area id -> index into page.cards
    std::vector<std::size_t> unmatchedTeleport;         // indices into page.teleport
    std::vector<std::size_t> unmatchedCards;            // indices into page.cards (named cards only, not the note)
    std::vector<std::string> note;                      // the page's closing note cell, its lines in order (may be empty)
};

// The folded comparison key of a name (lower case, no accents, punctuation as single spaces, trimmed).
std::string MapsNameKey(const std::string& name);

// Matches `page` to the areas of `data`. `english` (may be null) is the English copy of the page, used by position
// for rows whose name finds nothing (see the top of this file).
LiveMaps MatchMapsPage(const Data& data, const mapspage::Page& page, const mapspage::Page* english);

// The closing note's lines as one paragraph: the page breaks it into lines; they are joined with a space, except next
// to Chinese or Japanese characters (which are written without spaces).
std::string MapsNoteText(const std::vector<std::string>& lines);

// The text a Special areas card shows for a matched card: its size line and its location line, as the page has them,
// one per line (lines that are empty are left out).
std::string MapsCardText(const mapspage::SpecialCard& card);

}  // namespace esrwiki::campaign
