// Platform-neutral model for the ESR wiki overlay.
//
// Reads the tab-separated file written by tools/extract_wiki.py (format 2).
// Nothing here touches Win32, so the parser, search and paging can be tested on
// any host.
//
// Structure: a Page holds Entries (the rows in the left list, e.g. "Weapons" or
// "Kanji Runes"). An Entry holds Blocks (the things shown in the detail pane,
// e.g. one runeword). A Block with item == true is a real game item and gets
// hover tooltips in the overlay.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "text_fold.hpp"

namespace esrwiki {

enum class LineKind : char {
    Heading = 'H',  // bold section title inside a block
    Text = 'T',     // plain body text
    Stat = 'S',     // colored game text (D2 color codes), e.g. item stats
    Row = 'R',      // table row; cells are separated by U+001F
    Header = 'C',   // table header row, same cell format
};

struct Line {
    LineKind kind = LineKind::Text;
    std::string text;
};

struct Field {
    std::string label;  // e.g. "Required Level"
    std::string value;  // e.g. "60"
};

struct RuneIcon {
    std::string imageKey;  // file name without extension, under images/
    std::string label;     // rune name shown beside the icon, e.g. "Ist"
    bool lod = false;      // an LoD rune: its label is orange, not in the Kanji colours
};

struct Block {
    bool item = false;          // true for real game items (weapons, runewords, runes, gems, ...)
    std::string title;          // item or section name; may be empty for plain guide sections
    std::string subtitle;
    std::string section;        // group header shared by neighbouring items, e.g. a set name or "3 Sockets"
    std::vector<Field> fields;
    std::vector<std::string> images;  // image keys for item art
    bool tinyArt = false;             // art and its space shrunk to 50% (IMG ... tiny): rings, amulets, charms
    bool smallArt = false;            // art drawn at half size (IMG ... small)
    bool originalSize = false;        // art drawn at the picture's own pixel size (IMG ... original)
    std::vector<RuneIcon> runes;      // rune sequence, in order
    std::vector<Line> lines;
    std::string tier;     // Normal, Exceptional or Elite for uniques; empty otherwise
    std::string family;   // weapon family for the sort bar; empty otherwise
    int level = 0;        // required level, for sorting
    std::string note;               // extra notes shown beside the card; not part of the tooltip
    std::string tag;                // short label drawn under the art in a card tile, e.g. "(Coupon only)"
    // Search cache: the title and BlockSearchLines, made by text::SearchTextUtf8 and joined by '\n', built the first
    // time the block is searched.
    mutable std::string foldedText;
    mutable bool foldedReady = false;
};

struct Entry {
    std::string title;     // shown in the left list
    std::string subtitle;  // shown under the title in the detail pane
    std::vector<Block> blocks;
    bool sortable = false;  // item cards can be sorted by level, tier, family or name
    bool wide = false;     // one card per row, with its stats and notes in a column beside it
    bool compact = false;  // small cards, six per row (as on the rune library)
    int splitLayout = 0;      // 1: lines on the left, picture and name on the right; 2: the other way round (flags "split", "splitflip")
    bool rowSort = false;     // the sort chips order table rows by Min iLvl, Max iLvl or Rarity (flag "rowsort")
    bool keepOrder = false;   // type chips in the order the blocks appear, not alphabetical (flag "keeporder")
    bool socketSort = false;  // a Sockets filter like runewords, and no sort chips (flag "socketsort")
    bool fiveAcross = false;  // item cards, five per row (flag "fiveacross" after "sortable")
    // The Campaign Progression map is drawn under the entry's text (flag "campaignmap"). A flag rather than the title, so the
    // translated copies of wiki-data.txt are recognised too.
    bool campaignMap = false;
};

struct Page {
    std::string id;
    std::string title;
    std::string group;  // tab row this page belongs to, e.g. "Guide"
    bool compact = false;  // small item tiles (many per row), e.g. the rune library
    std::vector<Entry> entries;
};

struct Database {
    std::vector<Page> pages;
    std::size_t entryCount() const;  // rows across all pages
    std::size_t blockCount() const;  // detail blocks across all pages
};

// Parses the output of extract_wiki.py. Returns false and fills `error` when a
// record is malformed; `out` is left unspecified in that case.
bool ParseDatabase(const std::string& text, Database& out, std::string& error);

// Indexes of entries on `page` whose title and subtitle, or any one block, match `query` (the query language of
// text_fold.hpp: every word, "phrases", -excluded; case and accents folded). An empty query matches every entry.
std::vector<std::size_t> FilterEntries(const Page& page, const std::string& query);

// How the rows of an entry's item blocks are ordered. Sockets sorts by the
// number of runes in a runeword; Name sorts by title.
// Default picks the sort for the entry: fewest sockets for runewords, lowest level for
// sortable item lists. Filters (socket count, tier, weapon family) are applied before
// the sort.
enum class BlockSort { Default, Name, LevelLowFirst, LevelHighFirst, RowMinIlvl, RowMaxIlvl, RowRarity, RowRarityHigh };

// The sorts that reorder the rows of the tables inside a block (entries with the "rowsort" flag) instead of the blocks.
bool IsRowSort(BlockSort sort);

// The lines of a block with the rows of each table put in the order of `sort`: Min iLvl lowest first, Max iLvl highest
// first, Rarity lowest first. Rows keep their order when the column is missing, and equal values keep their order.
std::vector<Line> SortTableRows(const std::vector<Line>& lines, BlockSort sort);

// The sort that applies to an entry, with Default resolved.
BlockSort ResolveBlockSort(const Entry& entry, BlockSort sort);

// Block indexes of `entry` in display order. `socketFilter` keeps only blocks with
// that many runes (0 keeps every block). Blocks with a section stay grouped under
// it, in the order each section first appears, unless sorted by Name.
// `query` (see FilterEntries) keeps only blocks that match it, unless the entry's own title and subtitle match;
// then every block is kept except those with an excluded term.
std::vector<std::size_t> ArrangeBlocks(const Entry& entry, std::size_t socketFilter, const std::string& tierFilter,
                                       const std::string& familyFilter, BlockSort sort,
                                       const std::string& query = std::string());

// True when the block matches `query` (see FilterEntries): its title, subtitle, section, fields, rune labels, lines,
// note and tag. The search suggestions (search_index.hpp) match a card on exactly this text.
bool BlockMatches(const Block& block, const std::string& query);
bool BlockMatches(const Block& block, const text::ParsedQuery& query);
// True when the entry's own title and subtitle match `query`.
bool EntryTextMatches(const Entry& entry, const text::ParsedQuery& query);
// The searchable lines of a block besides its title, colour codes removed, in a fixed order: subtitle, section,
// "label: value" for each field, the rune labels (one line), the lines, note, tag. Empty pieces are left out.
std::vector<std::string> BlockSearchLines(const Block& block);
// The block's cached search text (see Block::foldedText).
const std::string& BlockSearchText(const Block& block);
// The entry's title and subtitle (colour codes removed) made by text::SearchTextUtf8, joined by '\n'.
std::string EntrySearchText(const Entry& entry);

// True when any block in the entry is a runeword (has rune icons).
bool HasRuneBlocks(const Entry& entry);

// Distinct rune counts across the entry's blocks, ascending (for the filter chips).
std::vector<std::size_t> SocketCounts(const Entry& entry);

// Number of pages needed for `total` rows at `perPage` rows each (at least 1).
std::size_t PageCount(std::size_t total, std::size_t perPage);

// One run of text that shares a color. `code` is the D2 color character
// ('0'-'9', ';', ':', 'C', ...) or '\0' for the default color.
struct ColorRun {
    std::string text;
    char code = '\0';
};

// Splits a string containing D2 color codes ("\xC3\xBF" 'c' code) into runs.
std::vector<ColorRun> SplitColorCodes(const std::string& text);

// The stat lines a rune gives when socketed in an item of the card category `entryTitle`
// ("Weapons", "Helms", "Torso", "Shields", ...), taken from the Runes page: the "All Items" lines
// plus the lines for that kind of item. `lod` picks the LoD copy of a name that exists in both
// sets (Ko). Empty when the rune is not on the Runes page or gives nothing for that item.
std::vector<Line> RuneBonusLines(const Database& db, const std::string& runeLabel, bool lod,
                                 const std::string& entryTitle);

}  // namespace esrwiki
