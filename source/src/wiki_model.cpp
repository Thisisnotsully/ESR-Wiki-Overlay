#include <cstdlib>
#include "wiki_model.hpp"
#include "text_fold.hpp"
#include "debug_log.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace esrwiki {
namespace {

constexpr const char* kColorPrefix = "\xC3\xBF" "c";  // U+00FF 'y-diaeresis' followed by 'c'
constexpr std::size_t kColorPrefixLength = 3;

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    for (char ch : line) {
        if (ch == '\t') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    fields.push_back(current);
    return fields;
}

std::string AsciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string PlainText(const std::string& piece) {
    std::string plain;  // colour codes are not text
    for (std::size_t i = 0; i < piece.size(); ++i) {
        if (piece.compare(i, kColorPrefixLength, kColorPrefix) == 0 && i + kColorPrefixLength < piece.size()) {
            i += kColorPrefixLength;
            continue;
        }
        plain += piece[i] == '\x1f' || piece[i] == '\n' ? ' ' : piece[i];
    }
    return plain;
}

}  // namespace

std::vector<std::string> BlockSearchLines(const Block& block) {
    std::vector<std::string> lines;
    const auto add = [&](const std::string& piece) {
        std::string plain = PlainText(piece);
        if (!plain.empty()) lines.push_back(std::move(plain));
    };
    add(block.subtitle);
    add(block.section);
    for (const Field& field : block.fields) add(field.label + ": " + field.value);
    if (!block.runes.empty()) {
        std::string runes;
        for (const RuneIcon& rune : block.runes) runes += (runes.empty() ? "" : " ") + rune.label;
        add(runes);
    }
    for (const Line& line : block.lines)
        if (line.kind != LineKind::Label) add(line.text);  // group labels (item kinds) are not card text
    add(block.note);
    add(block.tag);
    return lines;
}

const std::string& BlockSearchText(const Block& block) {
    if (block.foldedReady) return block.foldedText;
    std::string all = text::SearchTextUtf8(PlainText(block.title));
    for (const std::string& line : BlockSearchLines(block)) {
        all += '\n';
        all += text::SearchTextUtf8(line);
    }
    block.foldedText = std::move(all);
    block.foldedReady = true;
    return block.foldedText;
}

bool BlockMatches(const Block& block, const text::ParsedQuery& query) {
    return text::Matches(query, BlockSearchText(block));
}

std::string EntrySearchText(const Entry& entry) {
    return text::SearchTextUtf8(PlainText(entry.title)) + "\n" + text::SearchTextUtf8(PlainText(entry.subtitle));
}

bool EntryTextMatches(const Entry& entry, const text::ParsedQuery& query) {
    return text::Matches(query, EntrySearchText(entry));
}

std::size_t Database::entryCount() const {
    std::size_t total = 0;
    for (const Page& page : pages) total += page.entries.size();
    return total;
}

std::size_t Database::blockCount() const {
    std::size_t total = 0;
    for (const Page& page : pages)
        for (const Entry& entry : page.entries) total += entry.blocks.size();
    return total;
}

bool ParseDatabase(const std::string& text, Database& out, std::string& error) {
    out = Database{};
    std::istringstream stream(text);
    std::string raw;
    std::size_t lineNumber = 0;
    bool sawHeader = false;

    while (std::getline(stream, raw)) {
        ++lineNumber;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (raw.empty()) continue;

        const std::vector<std::string> fields = SplitTabs(raw);
        const std::string& tag = fields[0];
        auto fail = [&](const char* reason) {
            error = "line " + std::to_string(lineNumber) + ": " + reason;
            return false;
        };
        auto field = [&](std::size_t index) -> std::string {
            return index < fields.size() ? fields[index] : std::string();
        };

        if (!sawHeader) {
            if (tag != "#ESRWIKI") return fail("missing #ESRWIKI header");
            if (field(1) != "2") return fail("unsupported format version, expected 2");
            sawHeader = true;
            continue;
        }

        if (tag == "PAGE") {
            if (fields.size() < 4) return fail("PAGE needs id, title and group");
            Page page;
            page.id = fields[1];
            page.title = fields[2];
            page.group = fields[3];
            page.compact = field(4) == "compact";
            out.pages.push_back(std::move(page));
            continue;
        }
        if (tag == "ENTRY") {
            if (out.pages.empty()) return fail("ENTRY before any PAGE");
            Entry entry;
            entry.title = field(1);
            entry.subtitle = field(2);
            entry.wide = field(3) == "wide";
            entry.compact = field(3) == "compact";
            entry.sortable = field(3) == "sortable";
            for (std::size_t flag = 3; flag < fields.size(); ++flag) {
                if (fields[flag] == "fiveacross") entry.fiveAcross = true;
                if (fields[flag] == "keeporder") entry.keepOrder = true;
                if (fields[flag] == "rowsort") entry.rowSort = true;
                if (fields[flag] == "socketsort") entry.socketSort = true;
                if (fields[flag] == "split") entry.splitLayout = 1;
                if (fields[flag] == "splitflip") entry.splitLayout = 2;
                if (fields[flag] == "campaignmap") entry.campaignMap = true;
            }
            if (entry.title.empty()) return fail("ENTRY needs a title");
            out.pages.back().entries.push_back(std::move(entry));
            continue;
        }

        if (out.pages.empty() || out.pages.back().entries.empty())
            return fail("record before any ENTRY");
        Entry& entry = out.pages.back().entries.back();

        if (tag == "BLOCK") {
            Block block;
            block.item = field(1) == "1";
            block.title = field(2);
            block.subtitle = field(3);
            block.section = field(4);
            block.tier = field(5);
            block.family = field(6);
            block.level = std::atoi(field(7).c_str());
            entry.blocks.push_back(std::move(block));
            continue;
        }

        // Records below belong to the most recent block. Guide sections often have
        // no BLOCK line, so one is created on demand.
        if (entry.blocks.empty()) entry.blocks.emplace_back();
        Block& block = entry.blocks.back();

        if (tag == "FIELD") {
            if (fields.size() < 3) return fail("FIELD needs a label and value");
            block.fields.push_back(Field{fields[1], fields[2]});
        } else if (tag == "IMG") {
            if (fields.size() < 2) return fail("IMG needs a key");
            block.images.push_back(fields[1]);
            if (fields.size() > 2 && fields[2] == "small") block.smallArt = true;
            if (fields.size() > 2 && fields[2] == "tiny") block.tinyArt = true;
            if (fields.size() > 2 && fields[2] == "original") block.originalSize = true;
        } else if (tag == "TAG") {
            block.tag = fields.size() > 1 ? fields[1] : std::string();
            // A two-character "\\n" in the file is a line break in the tag.
            for (std::size_t at = block.tag.find("\\n"); at != std::string::npos; at = block.tag.find("\\n", at + 1))
                block.tag.replace(at, 2, "\n");
        } else if (tag == "NOTE") {
            block.note = fields.size() > 1 ? fields[1] : std::string();
        } else if (tag == "RUNE") {
            if (fields.size() < 3) return fail("RUNE needs a key and label");
            block.runes.push_back(RuneIcon{fields[1], fields[2], fields.size() > 3 && fields[3] == "lod"});
        } else if (tag == "LINE") {
            if (fields.size() < 3) return fail("LINE needs a kind and text");
            const std::string& kind = fields[1];
            LineKind lineKind = LineKind::Text;
            if (kind == "H") lineKind = LineKind::Heading;
            else if (kind == "S") lineKind = LineKind::Stat;
            else if (kind == "R") lineKind = LineKind::Row;
            else if (kind == "C") lineKind = LineKind::Header;
            else if (kind == "L") lineKind = LineKind::Label;
            else if (kind != "T") return fail("LINE kind must be H, T, S, R, C or L");
            block.lines.push_back(Line{lineKind, fields[2]});
        } else {
            return fail("unknown record type");
        }
    }

    if (!sawHeader) {
        error = "empty database";
        return false;
    }
    return true;
}

std::vector<std::size_t> FilterEntries(const Page& page, const std::string& query) {
    const text::ParsedQuery parsed = text::ParseQuery(query);
    std::vector<std::size_t> matches;
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
        const Entry& entry = page.entries[index];
        bool matched = parsed.empty() || EntryTextMatches(entry, parsed);
        for (std::size_t i = 0; !matched && i < entry.blocks.size(); ++i) {
            matched = BlockMatches(entry.blocks[i], parsed);
        }
        if (matched) matches.push_back(index);
    }
    return matches;
}

bool HasRuneBlocks(const Entry& entry) {
    for (const Block& block : entry.blocks) {
        if (!block.runes.empty()) return true;
    }
    return false;
}

// How many sockets a block has: its runes for runewords, or the Sockets field for gemwords (which have no runes).
static std::size_t BlockSocketCount(const Block& block) {
    if (!block.runes.empty()) return block.runes.size();
    for (const Field& field : block.fields) {
        if (field.label != "Sockets") continue;
        std::size_t count = 0;
        for (char c : field.value) {
            if (c < '0' || c > '9') break;
            count = count * 10 + static_cast<std::size_t>(c - '0');
        }
        return count;
    }
    return 0;
}

std::vector<std::size_t> SocketCounts(const Entry& entry) {
    std::vector<std::size_t> counts;
    for (const Block& block : entry.blocks) {
        const std::size_t count = BlockSocketCount(block);
        if (count == 0) continue;
        if (std::find(counts.begin(), counts.end(), count) == counts.end()) counts.push_back(count);
    }
    std::sort(counts.begin(), counts.end());
    return counts;
}

bool BlockMatches(const Block& block, const std::string& query) {
    return BlockMatches(block, text::ParseQuery(query));
}

bool IsRowSort(BlockSort sort) {
    return sort == BlockSort::RowMinIlvl || sort == BlockSort::RowMaxIlvl || sort == BlockSort::RowRarity
        || sort == BlockSort::RowRarityHigh;
}


// The number in a table cell, ignoring colour codes; -1 when there is none.
static long CellNumber(const std::string& cell) {
    std::size_t i = 0;
    while (i < cell.size()) {
        if (static_cast<unsigned char>(cell[i]) == 0xC3 && i + 3 < cell.size() && static_cast<unsigned char>(cell[i + 1]) == 0xBF && cell[i + 2] == 'c') {
            i += 4;  // "ÿc" and the colour letter
            continue;
        }
        if (cell[i] >= '0' && cell[i] <= '9') {
            long value = 0;
            while (i < cell.size() && cell[i] >= '0' && cell[i] <= '9') value = value * 10 + (cell[i++] - '0');
            return value;
        }
        ++i;
    }
    return -1;
}

static std::vector<std::string> SplitRowCells(const std::string& text) {
    std::vector<std::string> cells(1);
    for (const char c : text) {
        if (c == '\x1f') cells.emplace_back();
        else cells.back() += c;
    }
    return cells;
}

std::vector<Line> SortTableRows(const std::vector<Line>& lines, BlockSort sort) {
    if (!IsRowSort(sort)) return lines;
    const char* const column = sort == BlockSort::RowMinIlvl ? "Min iLvl" : sort == BlockSort::RowMaxIlvl ? "Max iLvl" : "Rarity";
    const bool highFirst = sort == BlockSort::RowMaxIlvl || sort == BlockSort::RowRarityHigh;
    std::vector<Line> out = lines;
    std::size_t index = 0;
    while (index < out.size()) {
        if (out[index].kind != LineKind::Row) { ++index; continue; }
        std::size_t end = index;
        while (end < out.size() && out[end].kind == LineKind::Row) ++end;
        // the header row just above the rows says which column holds the number
        std::size_t position = std::string::npos;
        if (index > 0 && out[index - 1].kind == LineKind::Header) {
            const std::vector<std::string> names = SplitRowCells(out[index - 1].text);
            for (std::size_t c = 0; c < names.size(); ++c)
                if (names[c] == column) position = c;
            // A translated page names the columns in its own language; they keep the English order (Name, Rare, Min iLvl,
            // Max iLvl, Req Lvl, Rarity, Group, Stats).
            const bool byName = position != std::string::npos;
            if (position == std::string::npos && names.size() == 8)
                position = sort == BlockSort::RowMinIlvl ? 2 : sort == BlockSort::RowMaxIlvl ? 3 : 5;
            DebugLogOnce(std::string("sort:") + column + ":" + std::to_string(names.size()) + (position == std::string::npos ? ":none" : ":ok"),
                         std::string("row sort by ") + column + ": " +
                             (position == std::string::npos ? "no matching column (the header has " + std::to_string(names.size()) + " cells)"
                                                            : "column " + std::to_string(position) + (byName ? " found by name" : " found by position")));
        }
        if (position != std::string::npos) {
            std::vector<long> keys;
            for (std::size_t r = index; r < end; ++r) {
                const std::vector<std::string> cells = SplitRowCells(out[r].text);
                long value = position < cells.size() ? CellNumber(cells[position]) : -1;
                if (value < 0 && highFirst && sort == BlockSort::RowMaxIlvl) value = 1000000;  // no maximum item level: no upper limit, so it counts as the highest
                keys.push_back(value);
            }
            std::vector<std::size_t> order(end - index);
            for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return highFirst ? keys[a] > keys[b] : keys[a] < keys[b];
            });
            std::vector<Line> block;
            block.reserve(order.size());
            for (const std::size_t k : order) block.push_back(out[index + k]);
            std::copy(block.begin(), block.end(), out.begin() + static_cast<std::ptrdiff_t>(index));
        }
        index = end;
    }
    return out;
}

BlockSort ResolveBlockSort(const Entry& entry, BlockSort sort) {
    if (entry.rowSort) return IsRowSort(sort) ? sort : BlockSort::Default;  // the blocks keep their order
    if (entry.socketSort) return BlockSort::Default;  // gemwords keep their order; the Sockets filter picks
    if (sort != BlockSort::Default) return sort;
    // Only sortable entries have a level sort; everything else keeps its section order.
    return entry.sortable ? BlockSort::LevelLowFirst : BlockSort::Default;
}

// Lower-cased titles, indexed like the blocks, so a name sort compares them without copying.
static std::vector<std::string> LowerTitles(const std::vector<Block>& blocks) {
    std::vector<std::string> titles;
    titles.reserve(blocks.size());
    for (const Block& block : blocks) titles.push_back(AsciiLower(block.title));
    return titles;
}

std::vector<std::size_t> ArrangeBlocks(const Entry& entry, std::size_t socketFilter, const std::string& tierFilter,
                                       const std::string& familyFilter, BlockSort sort, const std::string& query) {
    sort = ResolveBlockSort(entry, sort);
    const text::ParsedQuery parsed = text::ParseQuery(query);
    const bool wholeEntry = parsed.empty() || EntryTextMatches(entry, parsed);
    text::ParsedQuery excludedOnly;  // with the whole entry kept, a block still drops out for an excluded term
    for (const text::QueryTerm& term : parsed.terms)
        if (term.excluded) excludedOnly.terms.push_back(term);
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < entry.blocks.size(); ++i) {
        const Block& block = entry.blocks[i];
        if (socketFilter != 0 && BlockSocketCount(block) != socketFilter) continue;
        if (!tierFilter.empty() && block.tier != tierFilter) continue;
        if (!familyFilter.empty() && block.family != familyFilter) continue;
        if (!BlockMatches(block, wholeEntry ? excludedOnly : parsed)) continue;
        order.push_back(i);
    }

    if (entry.sortable) {
        const auto& blocks = entry.blocks;
        switch (sort) {
            case BlockSort::Name: {
                const std::vector<std::string> titles = LowerTitles(blocks);
                std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                    return titles[a] < titles[b];
                });
                break;
            }
            case BlockSort::LevelHighFirst:
                std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                    return blocks[a].level > blocks[b].level;
                });
                break;
            default:  // LevelLowFirst
                std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                    return blocks[a].level < blocks[b].level;
                });
                break;
        }
        return order;
    }

    if (sort == BlockSort::Name) {
        const std::vector<std::string> titles = LowerTitles(entry.blocks);
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return titles[a] < titles[b];
        });
        return order;
    }

    // Keep each section together, in the order its first block appears.
    std::vector<std::string> sectionOrder;
    std::vector<std::size_t> rank(order.size(), 0);
    for (std::size_t k = 0; k < order.size(); ++k) {
        const std::string& section = entry.blocks[order[k]].section;
        auto found = std::find(sectionOrder.begin(), sectionOrder.end(), section);
        if (found == sectionOrder.end()) {
            sectionOrder.push_back(section);
            found = sectionOrder.end() - 1;
        }
        rank[k] = static_cast<std::size_t>(found - sectionOrder.begin());
    }
    std::vector<std::size_t> keys(order.size());
    for (std::size_t k = 0; k < keys.size(); ++k) keys[k] = k;
    std::stable_sort(keys.begin(), keys.end(), [&](std::size_t a, std::size_t b) { return rank[a] < rank[b]; });
    std::vector<std::size_t> grouped;
    grouped.reserve(order.size());
    for (std::size_t k : keys) grouped.push_back(order[k]);
    return grouped;
}

std::size_t PageCount(std::size_t total, std::size_t perPage) {
    if (perPage == 0) return 1;
    return std::max<std::size_t>(1, (total + perPage - 1) / perPage);
}

std::vector<ColorRun> SplitColorCodes(const std::string& text) {
    std::vector<ColorRun> runs;
    ColorRun current;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, kColorPrefixLength, kColorPrefix) == 0 && i + kColorPrefixLength < text.size()) {
            if (!current.text.empty()) runs.push_back(current);
            current.text.clear();
            current.code = text[i + kColorPrefixLength];
            i += kColorPrefixLength + 1;
            continue;
        }
        current.text.push_back(text[i]);
        ++i;
    }
    if (!current.text.empty() || runs.empty()) runs.push_back(current);
    return runs;
}

namespace {

// The Runes page gives each rune's lines in up to three groups, one per socket column of the website's table, always in
// this order: weapons (and gloves), helms (boots, charms; on today's site also staffs, wands, orbs and jewellery), shields
// (body armour, belts). Neighbouring columns with the same lines are one group under their joined labels, and one group
// for all three columns is "All Items". The labels are the website's own words, translated on the localized pages, so the
// groups are told apart by their place and by the page's own label text, never by English words.
constexpr unsigned kAllRuneColumns = 7;  // bit c: column c

// The column (0, 1 or 2) an item kind takes its rune bonus from, by the English card category; -1 when not one.
int RuneColumnOfTitle(const std::string& title) {
    if (title == "Weapons" || title == "Gloves") return 0;
    if (title == "Helms" || title == "Boots" || title == "Charms") return 1;
    if (title == "Torso" || title == "Shields" || title == "Belts") return 2;
    return -1;
}

// The same by an item picture (image keys are not translated): -1 for a picture of no socketable kind.
int RuneColumnOfImage(const std::string& key) {
    auto starts = [&key](const char* prefix) { return key.rfind(prefix, 0) == 0; };
    if (starts("items_weapon_") || starts("items_armor_glove")) return 0;
    if (starts("items_armor_helm") || starts("items_armor_circlet") || starts("items_armor_boot") || starts("items_misc_charm"))
        return 1;
    if (starts("items_armor_armor") || starts("items_armor_shield") || starts("items_armor_belt")) return 2;
    return -1;
}

// The column of the cards of `entry` (a Runewords category): its English title, else what most of its pictures show
// (the translated data has translated titles).
int RuneColumnOfEntry(const Entry& entry) {
    const int byTitle = RuneColumnOfTitle(entry.title);
    if (byTitle >= 0) return byTitle;
    int votes[3] = {0, 0, 0};
    for (const Block& block : entry.blocks)
        for (const std::string& image : block.images) {
            const int column = RuneColumnOfImage(image);
            if (column >= 0) ++votes[column];
        }
    int best = -1;
    for (int c = 0; c < 3; ++c)
        if (votes[c] > 0 && (best < 0 || votes[c] > votes[best])) best = c;
    return best;
}

// A rune page group label line ("\xC3\xBF" "c0Helm / Boots:"): its text without the colon, else empty.
std::string RuneGroupLabel(const Line& line) {
    if (line.kind != LineKind::Stat || line.text.compare(0, 4, "\xC3\xBF" "c0") != 0) return std::string();
    std::string plain;
    for (const ColorRun& run : SplitColorCodes(line.text)) plain += run.text;
    if (plain.empty() || plain.back() != ':') return std::string();
    plain.pop_back();
    while (!plain.empty() && plain.back() == ' ') plain.pop_back();  // "Tous les objets :"
    return plain;
}

// The group labels of a rune block, in order.
std::vector<std::string> RuneGroupLabels(const Block& block) {
    std::vector<std::string> labels;
    for (const Line& line : block.lines) {
        std::string label = RuneGroupLabel(line);
        if (!label.empty()) labels.push_back(std::move(label));
    }
    return labels;
}

// The page's three column labels, from the first rune with one group per column; empty when no rune has three.
std::vector<std::string> RuneColumnLabels(const Page& page) {
    for (const Entry& entry : page.entries)
        for (const Block& block : entry.blocks) {
            std::vector<std::string> labels = RuneGroupLabels(block);
            if (labels.size() == 3 && labels[0] != labels[1] && labels[1] != labels[2]) return labels;
        }
    return {};
}

// The columns (bit masks) of each group of a rune: by the page's column labels when every group label is one column or
// neighbouring columns joined with " / "; else by place: one group is every column (All Items), two are weapons and the
// rest, three are one column each.
std::vector<unsigned> RuneGroupColumns(const std::vector<std::string>& labels, const std::vector<std::string>& columns) {
    std::vector<unsigned> masks;
    if (columns.size() == 3) {
        const std::pair<std::string, unsigned> spans[] = {
            {columns[0], 1u}, {columns[1], 2u}, {columns[2], 4u}, {columns[0] + " / " + columns[1], 3u},
            {columns[1] + " / " + columns[2], 6u}, {columns[0] + " / " + columns[1] + " / " + columns[2], 7u}};
        for (const std::string& label : labels) {
            unsigned mask = 0;
            for (const auto& span : spans)
                if (label == span.first) mask = span.second;
            if (mask == 0) break;
            masks.push_back(mask);
        }
        if (masks.size() == labels.size()) return masks;
        masks.clear();
    }
    if (labels.size() == 1) return {kAllRuneColumns};
    if (labels.size() == 2) return {1u, 6u};
    for (std::size_t i = 0; i < labels.size(); ++i) masks.push_back(i < 3 ? 1u << i : 0u);
    return masks;
}

// "Ko" -> "items_misc_rune_ko_rune", as the rune builders name the art.
std::string RuneImageKey(const std::string& label) {
    std::string slug;
    bool gap = false;
    for (char ch : label) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0 && c < 0x80) {
            if (gap && !slug.empty()) slug += '_';
            gap = false;
            slug += static_cast<char>(std::tolower(c));
        } else {
            gap = true;
        }
    }
    return "items_misc_rune_" + slug + "_rune";
}

bool HasImage(const Block& block, const std::string& key) {
    return std::find(block.images.begin(), block.images.end(), key) != block.images.end();
}

// Lower case ASCII, single spaces, no edge spaces: labels compare by this.
std::string NormalizeLabel(const std::string& text) {
    std::string out;
    for (char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == ' ' || c == '\t') {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += c < 0x80 ? static_cast<char>(std::tolower(c)) : ch;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// The " / " separated parts of a label, normalized.
std::vector<std::string> LabelPieces(const std::string& label) {
    std::vector<std::string> pieces;
    std::size_t from = 0;
    while (from <= label.size()) {
        std::size_t to = label.find(" / ", from);
        if (to == std::string::npos) to = label.size();
        std::string piece = NormalizeLabel(label.substr(from, to - from));
        if (!piece.empty()) pieces.push_back(std::move(piece));
        from = to + 3;
    }
    return pieces;
}

// The column of one hovered card: the website puts Staff, Wand and Orb in the helm column, so their art says column 1;
// else the card's own socketable group labels (Label lines) when they name exactly one of the Runes page's columns
// (a label part equal to a part of a column label, compared as text, so any language the page labels agree with);
// else the category rule.
int RuneColumnOfCard(const Database& db, const Entry& entry, const Block& block) {
    for (const std::string& image : block.images)
        if (image.rfind("items_weapon_staff_", 0) == 0 || image.rfind("items_weapon_orb_", 0) == 0 ||
            image.rfind("items_weapon_wand_", 0) == 0)
            return 1;
    for (const Page& page : db.pages) {
        if (page.id != "runes") continue;
        const std::vector<std::string> columns = RuneColumnLabels(page);
        if (columns.size() != 3) break;
        int matched = -1;
        for (const Line& line : block.lines) {
            if (line.kind != LineKind::Label) continue;
            for (const std::string& piece : LabelPieces(line.text))
                for (int c = 0; c < 3; ++c) {
                    const std::vector<std::string> own = LabelPieces(columns[static_cast<std::size_t>(c)]);
                    if (std::find(own.begin(), own.end(), piece) == own.end()) continue;
                    if (matched >= 0 && matched != c) return RuneColumnOfEntry(entry);  // groups of several columns
                    matched = c;
                }
        }
        if (matched >= 0) return matched;
        break;
    }
    return RuneColumnOfEntry(entry);
}

std::vector<Line> RuneBonusLinesForColumn(const Database& db, const std::string& imageKey, const std::string& runeLabel,
                                          bool lod, int column) {
    std::vector<Line> out;
    // The art of a LoD rune whose name is a Kanji rune too is "<name>_lod_rune"; a runeword names the plain art.
    std::vector<std::string> keys;
    if (lod && imageKey.size() > 5 && imageKey.compare(imageKey.size() - 5, 5, "_rune") == 0)
        keys.push_back(imageKey.substr(0, imageKey.size() - 5) + "_lod_rune");
    keys.push_back(imageKey);
    for (const Page& page : db.pages) {
        if (page.id != "runes") continue;
        const Block* found = nullptr;
        for (std::size_t k = 0; k < keys.size() && found == nullptr; ++k) {
            // A LoD rune named like a Kanji rune is found by its "_lod_" art only; the plain art would be the Kanji card.
            // The LoD runes are the page's second entry.
            const bool plainOfLod = lod && keys.size() > 1 && k + 1 == keys.size();
            for (std::size_t e = 0; e < page.entries.size() && found == nullptr; ++e) {
                if (plainOfLod && e != 1) continue;
                for (const Block& block : page.entries[e].blocks)
                    if (HasImage(block, keys[k])) { found = &block; break; }
            }
        }
        if (found == nullptr) {  // data without rune art: the English card title
            const std::string wanted = runeLabel + " Rune";
            for (const Entry& entry : page.entries) {
                if ((entry.title == "LoD Runes") != lod) continue;
                for (const Block& block : entry.blocks)
                    if (block.title == wanted) { found = &block; break; }
                if (found != nullptr) break;
            }
        }
        if (found == nullptr) return out;
        const std::vector<unsigned> masks = RuneGroupColumns(RuneGroupLabels(*found), RuneColumnLabels(page));
        const unsigned wantedMask = column >= 0 && column < 3 ? 1u << column : 0u;
        std::size_t group = 0;
        bool include = true;  // lines before the first label belong to every item
        for (const Line& line : found->lines) {
            if (!RuneGroupLabel(line).empty()) {
                include = group < masks.size() && (masks[group] & wantedMask) != 0;
                ++group;
                continue;
            }
            if (include) out.push_back(line);
        }
        return out;
    }
    return out;
}

}  // namespace

std::vector<Line> RuneBonusLines(const Database& db, const RuneIcon& rune, const Entry& itemEntry) {
    return RuneBonusLinesForColumn(db, rune.imageKey, rune.label, rune.lod, RuneColumnOfEntry(itemEntry));
}

std::vector<Line> RuneBonusLines(const Database& db, const RuneIcon& rune, const Entry& itemEntry, const Block& card) {
    return RuneBonusLinesForColumn(db, rune.imageKey, rune.label, rune.lod, RuneColumnOfCard(db, itemEntry, card));
}

std::vector<Line> RuneBonusLines(const Database& db, const std::string& runeLabel, bool lod,
                                 const std::string& entryTitle) {
    int column = RuneColumnOfTitle(entryTitle);
    if (column < 0) {  // a translated category title: the Runewords entry of that name
        for (const Page& page : db.pages) {
            if (page.id != "runewords") continue;
            for (const Entry& entry : page.entries)
                if (entry.title == entryTitle) column = RuneColumnOfEntry(entry);
        }
    }
    return RuneBonusLinesForColumn(db, RuneImageKey(runeLabel), runeLabel, lod, column);
}

}  // namespace esrwiki
