#include <cstdlib>
#include "wiki_model.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

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

bool Contains(const std::string& haystack, const std::string& lowerNeedle) {
    return AsciiLower(haystack).find(lowerNeedle) != std::string::npos;
}

bool BlockContains(const Block& block, const std::string& needle) {
    if (Contains(block.title, needle) || Contains(block.subtitle, needle) || Contains(block.section, needle))
        return true;
    if (Contains(block.tag, needle)) return true;
    for (const Field& field : block.fields) {
        if (Contains(field.label, needle) || Contains(field.value, needle)) return true;
    }
    for (const Line& line : block.lines) {
        if (Contains(line.text, needle)) return true;
    }
    for (const RuneIcon& rune : block.runes) {
        if (Contains(rune.label, needle)) return true;
    }
    return false;
}

}  // namespace

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
            else if (kind != "T") return fail("LINE kind must be H, T, S, R or C");
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
    const std::string needle = AsciiLower(query);
    std::vector<std::size_t> matches;
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
        const Entry& entry = page.entries[index];
        bool matched = needle.empty() || Contains(entry.title, needle) || Contains(entry.subtitle, needle);
        for (std::size_t i = 0; !matched && i < entry.blocks.size(); ++i) {
            matched = BlockContains(entry.blocks[i], needle);
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

std::vector<std::size_t> SocketCounts(const Entry& entry) {
    std::vector<std::size_t> counts;
    for (const Block& block : entry.blocks) {
        if (block.runes.empty()) continue;
        if (std::find(counts.begin(), counts.end(), block.runes.size()) == counts.end())
            counts.push_back(block.runes.size());
    }
    std::sort(counts.begin(), counts.end());
    return counts;
}

bool BlockMatches(const Block& block, const std::string& query) {
    return BlockContains(block, AsciiLower(query));
}

BlockSort ResolveBlockSort(const Entry& entry, BlockSort sort) {
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
    const std::string needle = AsciiLower(query);
    const bool wholeEntry = needle.empty() || Contains(entry.title, needle) || Contains(entry.subtitle, needle);
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < entry.blocks.size(); ++i) {
        const Block& block = entry.blocks[i];
        if (socketFilter != 0 && block.runes.size() != socketFilter) continue;
        if (!tierFilter.empty() && block.tier != tierFilter) continue;
        if (!familyFilter.empty() && block.family != familyFilter) continue;
        if (!wholeEntry && !BlockContains(block, needle)) continue;
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

std::vector<Line> RuneBonusLines(const Database& db, const std::string& runeLabel, bool lod,
                                 const std::string& entryTitle) {
    std::vector<Line> out;
    const std::string wanted = runeLabel + " Rune";
    const bool weapon = entryTitle.find("Weapon") != std::string::npos;
    const bool helm = entryTitle.find("Helm") != std::string::npos;
    auto keep = [&](const std::string& heading) {
        if (heading == "All Items") return true;
        if (weapon) return heading == "Weapon";
        if (helm) return heading == "Helm" || heading == "Armor/Helm/Shield";
        return heading == "Shield" || heading == "Armor/Helm/Shield";
    };
    for (const Page& page : db.pages) {
        if (page.id != "runes") continue;
        for (const Entry& entry : page.entries) {
            if ((entry.title == "LoD Runes") != lod) continue;
            for (const Block& block : entry.blocks) {
                if (block.title != wanted) continue;
                bool include = true;
                for (const Line& line : block.lines) {
                    std::string plain;
                    for (const ColorRun& run : SplitColorCodes(line.text)) plain += run.text;
                    if (!plain.empty() && plain.back() == ':' && line.text.compare(0, 4, "\xC3\xBF" "c0") == 0) {
                        include = keep(plain.substr(0, plain.size() - 1));
                        continue;
                    }
                    if (include) out.push_back(line);
                }
                return out;
            }
        }
    }
    return out;
}

}  // namespace esrwiki
