#include "search_index.hpp"

#include "text_fold.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace esrwiki::search {
namespace {

using text::Fold;
using text::QueryTerm;

bool WordStart(const std::u32string& text, std::size_t p) { return p == 0 || text[p - 1] == U' ' || text[p - 1] == U'\n'; }

void MarkSpan(std::vector<int>& marks, const std::u32string& text, std::size_t at, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i)
        if (text[at + i] != U' ') marks.push_back(static_cast<int>(at + i));  // the separators inside a phrase stay plain
}

void SortUnique(std::vector<int>& marks) {
    std::sort(marks.begin(), marks.end());
    marks.erase(std::unique(marks.begin(), marks.end()), marks.end());
}

// Every place of every required term in `text` (made by text::SearchText); returns how many terms were found.
int MarkTerms(const std::u32string& text, const text::ParsedQuery& query, std::vector<int>& marks) {
    int found = 0;
    for (const QueryTerm& term : query.terms) {
        if (term.excluded) continue;
        bool any = false;
        std::size_t length = 0;
        for (std::size_t p = text::FindTerm(text, term, 0, &length); p != std::u32string::npos;
             p = text::FindTerm(text, term, p + 1, &length)) {
            MarkSpan(marks, text, p, length);
            any = true;
        }
        if (any) ++found;
    }
    SortUnique(marks);
    return found;
}

// A subsequence match: the query's letters in order, preferring consecutive letters and word starts.
bool Subsequence(const std::u32string& text, const std::u32string& needle, int& score, std::vector<int>& marks) {
    const std::size_t n = text.size(), m = needle.size();
    if (m < 3 || m > n) return false;
    std::size_t probe = 0;
    for (std::size_t i = 0; i < n && probe < m; ++i)
        if (text[i] == needle[probe]) ++probe;
    if (probe < m) return false;  // not even in order
    int best = -1;
    std::vector<int> bestMarks;
    for (std::size_t start = 0; start < n; ++start) {
        if (text[start] != needle[0]) continue;
        std::vector<int> positions{static_cast<int>(start)};
        std::size_t at = start + 1;
        for (std::size_t k = 1; k < m; ++k) {
            while (at < n && text[at] != needle[k]) ++at;
            if (at >= n) { positions.clear(); break; }
            positions.push_back(static_cast<int>(at++));
        }
        if (positions.empty()) break;
        const int span = positions.back() - positions.front() + 1;
        if (span > static_cast<int>(m) * 3) continue;
        int value = 400 - (span - static_cast<int>(m)) * 12;
        if (WordStart(text, start)) value += 40;
        for (std::size_t k = 1; k < positions.size(); ++k)
            if (positions[k] == positions[k - 1] + 1) value += 6;
        if (value > best) { best = value; bestMarks = positions; }
    }
    if (best < 0) return false;
    score = std::min(best, 600);
    marks = bestMarks;
    return true;
}

// Damerau-Levenshtein distance 1 or less between two words?
bool WithinOneEdit(const std::u32string& a, const std::u32string& b) {
    const std::size_t la = a.size(), lb = b.size();
    if (la > lb + 1 || lb > la + 1) return false;
    std::size_t i = 0;
    while (i < la && i < lb && a[i] == b[i]) ++i;
    if (i == la && i == lb) return true;
    if (la == lb) {
        if (a.compare(i + 1, std::u32string::npos, b, i + 1, std::u32string::npos) == 0) return true;  // one substitution
        return i + 1 < la && a[i] == b[i + 1] && a[i + 1] == b[i] && a.compare(i + 2, std::u32string::npos, b, i + 2, std::u32string::npos) == 0;
    }
    const std::u32string& longer = la > lb ? a : b;
    const std::u32string& shorter = la > lb ? b : a;
    return longer.compare(i + 1, std::u32string::npos, shorter, i, std::u32string::npos) == 0;  // one missing letter
}

bool HasIdeograph(const std::u32string& s) {
    for (char32_t c : s)
        if (text::IsIdeographic(c)) return true;
    return false;
}

struct TitleMatch { int score = 0; std::vector<int> marks; };

// How a title ranks for a query that it already matches. `phrase`: the required words joined by one space.
TitleMatch MatchTitle(const std::u32string& title, const std::u32string& phrase, const text::ParsedQuery& query) {
    TitleMatch result;
    if (title.empty() || phrase.empty()) return result;
    std::size_t end = title.size();
    while (end > 0 && title[end - 1] == U' ') --end;  // trailing punctuation does not count
    std::size_t begin = 0;
    while (begin < end && title[begin] == U' ') ++begin;
    if (title.compare(begin, phrase.size(), phrase) == 0) {
        result.score = begin + phrase.size() == end ? 1000 : 900;
        MarkSpan(result.marks, title, begin, phrase.size());
        return result;
    }
    const bool longEnough = phrase.size() >= 3 || HasIdeograph(phrase);  // a short word only counts at a word start
    for (std::size_t p = title.find(phrase); p != std::u32string::npos; p = title.find(phrase, p + 1)) {
        if (WordStart(title, p)) { result.score = 800; MarkSpan(result.marks, title, p, phrase.size()); return result; }
    }
    const std::size_t plain = title.find(phrase);
    if (plain != std::u32string::npos && longEnough) { result.score = 700; MarkSpan(result.marks, title, plain, phrase.size()); return result; }
    // every required term is somewhere in the title (any order)
    int score = 650, terms = 0;
    std::vector<int> marks;
    for (const QueryTerm& term : query.terms) {
        if (term.excluded) continue;
        std::size_t length = 0;
        const std::size_t p = text::FindTerm(title, term, 0, &length);
        if (p == std::u32string::npos) return result;
        if (!WordStart(title, p)) score -= 40;
        MarkSpan(marks, title, p, length);
        ++terms;
    }
    if (terms > 1) {
        SortUnique(marks);
        result.score = score;
        result.marks = marks;
    }
    return result;
}

std::u32string Join(const std::vector<std::u32string>& words) {
    std::u32string out;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) out += U' ';
        out += words[i];
    }
    return out;
}

// The snippet of a hit found in its text: the line of `lines` that fits the query best, cut round the first match when
// long. `requiredCount`: how many required terms the query has; `phrase`: its required words joined by one space.
void MakeSnippet(const std::vector<std::string>& lines, const text::ParsedQuery& parsed, const std::u32string& phrase,
                 std::size_t requiredCount, Hit& hit) {
    // The line that fits the query best: all the terms, else the most of them; the marks are those terms.
    int bestLine = 0;
    std::string bestText;
    std::u32string bestOriginal;
    std::vector<int> bestMarks;
    for (const std::string& rawLine : lines) {
        // Cell separators and line breaks are shown as spaces (one for one, so the match positions stay valid).
        std::string line = rawLine;
        std::replace_if(line.begin(), line.end(), [](char c) { return c == '\x1f' || c == '\x1e' || c == '\n'; }, ' ');
        const std::u32string original = DecodeUtf8(line);
        const std::u32string lineText = text::SearchText(original);
        std::vector<int> marks;
        const int distinct = MarkTerms(lineText, parsed, marks);
        if (marks.empty()) continue;
        const int value = distinct * 10 + (static_cast<std::size_t>(distinct) == requiredCount ? 100 : 0) +
                          (lineText.find(phrase) != std::u32string::npos ? 50 : 0);
        if (value <= bestLine) continue;
        bestLine = value;
        bestText = line;
        bestOriginal = original;
        bestMarks = std::move(marks);
    }
    if (!bestMarks.empty()) {
        const std::u32string& original = bestOriginal;
        const std::vector<int>& marks = bestMarks;
        hit.snippet = bestText;
        hit.snippetMarks = marks;
        // A long line shows only the part round the first match.
        if (original.size() > 90) {
            const std::size_t first = static_cast<std::size_t>(marks.front());
            const std::size_t from = first > 30 ? first - 30 : 0;
            const std::size_t to = std::min(original.size(), from + 90);
            std::u32string cut = original.substr(from, to - from);
            std::vector<int> shifted;
            const std::size_t lead = from > 0 ? 1 : 0;
            for (int m : marks)
                if (static_cast<std::size_t>(m) >= from && static_cast<std::size_t>(m) < to)
                    shifted.push_back(static_cast<int>(static_cast<std::size_t>(m) - from + lead));
            if (from > 0) cut = U"\u2026" + cut;
            if (to < original.size()) cut += U"\u2026";
            hit.snippet = EncodeUtf8(cut);
            hit.snippetMarks = shifted;
        }
    }
}

std::string BlockLabel(const Block& block, const Entry& entry) {
    if (!block.title.empty()) return StripColorCodes(block.title);
    for (const Line& line : block.lines)
        if (line.kind == LineKind::Heading && !line.text.empty()) return StripColorCodes(line.text);
    return StripColorCodes(entry.title);
}

}  // namespace

std::string StripColorCodes(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) == 0xC3 && i + 3 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xBF &&
            text[i + 2] == 'c') {
            i += 3;
            continue;
        }
        out += text[i] == '\x1f' || text[i] == '\n' ? ' ' : text[i];
    }
    return out;
}

text::ParsedQuery Parse(const std::string& query) { return text::ParseQuery(StripColorCodes(query)); }

void Index::Build(const Database& db) {
    records_.clear();
    const auto add = [&](int p, int e, int b, const std::string& label, std::string matchText, bool sameAsEntry) {
        const std::size_t titleEnd = std::min(matchText.find('\n'), matchText.size());
        records_.push_back({p, e, b, text::SearchText(DecodeUtf8(label)), std::move(matchText), titleEnd, sameAsEntry, -1});
    };
    for (std::size_t p = 0; p < db.pages.size(); ++p) {
        const Page& page = db.pages[p];
        const std::string pageTitle = StripColorCodes(page.title);
        add(static_cast<int>(p), -1, -1, pageTitle, text::SearchTextUtf8(pageTitle), false);
        for (std::size_t e = 0; e < page.entries.size(); ++e) {
            const Entry& entry = page.entries[e];
            const std::string entryTitle = StripColorCodes(entry.title);
            add(static_cast<int>(p), static_cast<int>(e), -1, entryTitle, EntrySearchText(entry), false);
            const std::u32string entryFolded = text::SearchText(DecodeUtf8(entryTitle));
            for (std::size_t b = 0; b < entry.blocks.size(); ++b) {
                const Block& block = entry.blocks[b];
                const std::string& matchText = BlockSearchText(block);
                if (matchText.find_first_not_of(" \n") == std::string::npos) continue;
                const std::string label = BlockLabel(block, entry);
                add(static_cast<int>(p), static_cast<int>(e), static_cast<int>(b), label, matchText,
                    text::SearchText(DecodeUtf8(label)) == entryFolded);
            }
        }
    }
    databaseRecords_ = records_.size();
    AddExtraRecords();
}

// As BlockSearchLines: no colour codes, cell separators and line breaks as spaces, no empty lines.
void Index::CleanExtra(ExtraRecord& record) {
    record.label = StripColorCodes(record.label);
    std::vector<std::string> lines;
    for (const std::string& line : record.lines) {
        std::string plain = StripColorCodes(line);
        std::replace(plain.begin(), plain.end(), '\x1e', ' ');
        if (plain.find_first_not_of(' ') != std::string::npos) lines.push_back(std::move(plain));
    }
    record.lines = std::move(lines);
}

void Index::SetExtra(std::vector<ExtraRecord> extra) {
    records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(std::min(databaseRecords_, records_.size())), records_.end());
    extra_ = std::move(extra);
    for (ExtraRecord& record : extra_) CleanExtra(record);
    AddExtraRecords();
}

void Index::SetExtra(int source, std::vector<ExtraRecord> extra) {
    std::vector<ExtraRecord> next;
    next.reserve(extra_.size() + extra.size());
    bool placed = false;
    const auto addNew = [&]() {
        for (ExtraRecord& record : extra) {
            record.source = source;
            CleanExtra(record);
            next.push_back(std::move(record));
        }
        placed = true;
    };
    for (ExtraRecord& record : extra_) {
        if (record.source == source) continue;
        if (!placed && record.source > source) addNew();
        next.push_back(std::move(record));
    }
    if (!placed) addNew();
    records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(std::min(databaseRecords_, records_.size())), records_.end());
    extra_ = std::move(next);
    AddExtraRecords();
}

// The SetExtra records after the Database's: matched as a card is, on the label (first line) and the lines.
void Index::AddExtraRecords() {
    for (std::size_t i = 0; i < extra_.size(); ++i) {
        const ExtraRecord& extra = extra_[i];
        std::string matchText = text::SearchTextUtf8(extra.label);
        for (const std::string& line : extra.lines) {
            matchText += '\n';
            matchText += text::SearchTextUtf8(line);
        }
        if (matchText.find_first_not_of(" \n") == std::string::npos) continue;
        const std::size_t titleEnd = std::min(matchText.find('\n'), matchText.size());
        records_.push_back({extra.page, -1, -1, text::SearchText(DecodeUtf8(extra.label)), std::move(matchText), titleEnd, false,
                            static_cast<int>(i)});
    }
}

std::vector<std::size_t> Index::MatchingExtra(const std::string& query) const {
    std::vector<std::size_t> out;
    const text::ParsedQuery parsed = Parse(query);
    if (!parsed.HasRequired() && !parsed.HasExcluded()) return out;
    for (std::size_t i = databaseRecords_; i < records_.size(); ++i)
        if (text::Matches(parsed, records_[i].text)) out.push_back(static_cast<std::size_t>(records_[i].extra));
    return out;
}

std::size_t Index::CountMatches(const std::string& query) const {
    const text::ParsedQuery parsed = Parse(query);
    if (!parsed.HasRequired()) return 0;
    std::size_t count = 0;
    for (const Record& record : records_)
        if (text::Matches(parsed, record.text)) ++count;
    return count;
}

QueryHint Index::Hint(const std::string& query) const {
    QueryHint hint;
    const text::ParsedQuery parsed = Parse(query);
    hint.valid = parsed.HasRequired();
    hint.required = parsed.RequiredDisplay();
    hint.excluded = parsed.ExcludedDisplay();
    if (!hint.valid) return hint;
    for (const QueryTerm& term : parsed.terms) {
        if (term.excluded) continue;
        bool anywhere = false;
        for (const Record& record : records_)
            if (text::HasTerm(record.text, term)) { anywhere = true; break; }
        if (!anywhere) hint.missing.push_back(term.display);
    }
    for (const Record& record : records_)
        if (text::Matches(parsed, record.text)) ++hint.matches;
    return hint;
}

std::vector<Hit> Index::Query(const Database& db, const std::string& query, std::size_t currentPage, std::size_t limit) const {
    std::vector<Hit> hits;
    const text::ParsedQuery parsed = Parse(query);
    if (!parsed.HasRequired() || limit == 0) return hits;
    const std::vector<std::u32string> words = parsed.RequiredWords();
    const std::u32string phrase = Join(words);
    if (phrase.size() < 2) return hits;
    const std::string phrase8 = EncodeUtf8(phrase);
    std::size_t requiredCount = 0;
    const QueryTerm* single = nullptr;
    for (const QueryTerm& term : parsed.terms)
        if (!term.excluded) { ++requiredCount; single = &term; }
    const bool allowFuzzy = requiredCount == 1 && !single->phrase && phrase.size() >= 3;

    struct Candidate { std::size_t record; int score; std::vector<int> marks; bool fromText; bool exact; };
    // Cards and entries are listed at most 6 per entry, extra records at most 6 per group.
    const auto Grouped = [](const Record& record) { return record.extra >= 0 || record.entry >= 0; };
    const auto GroupOf = [this](const Record& record) {
        if (record.extra >= 0) {
            const ExtraRecord& extra = extra_[static_cast<std::size_t>(record.extra)];
            return std::pair<int, int>{-2 - extra.source, extra.group};  // a group of its own source
        }
        return std::pair<int, int>{record.page, record.entry};
    };
    std::vector<Candidate> found;
    std::vector<char> matched(records_.size(), 0);
    for (std::size_t i = 0; i < records_.size(); ++i) {
        const Record& record = records_[i];
        if (!text::Matches(parsed, record.text)) continue;
        matched[i] = 1;
        Candidate candidate{i, 0, {}, false, true};
        TitleMatch title = MatchTitle(record.label, phrase, parsed);
        if (title.score > 0) {
            candidate.score = title.score;
            candidate.marks = std::move(title.marks);
        } else {
            // the card's text: every term somewhere in the title or the body
            int score = 260;
            for (const QueryTerm& term : parsed.terms) {
                if (term.excluded) continue;
                const std::size_t at = text::FindTerm(record.text, term);
                if (at < record.titleEnd) score += 20;  // the title has it
                if (at != std::string::npos && (at == 0 || record.text[at - 1] == ' ' || record.text[at - 1] == '\n'))
                    score += 8;  // it starts a word
            }
            if (words.size() > 1 && record.text.find(phrase8, record.titleEnd) != std::string::npos) score += 140;  // as typed, together
            candidate.score = score;
            candidate.fromText = true;
        }
        found.push_back(std::move(candidate));
    }

    // exact hits that can be listed (at most 6 per entry, as below)
    std::size_t listable = 0;
    {
        std::map<std::pair<int, int>, int> perEntry;
        for (const Candidate& c : found) {
            const Record& record = records_[c.record];
            if (Grouped(record) && ++perEntry[GroupOf(record)] > 6) continue;
            ++listable;
        }
    }
    if (listable < kFuzzyBelow && allowFuzzy) {
        for (std::size_t i = 0; i < records_.size(); ++i) {
            if (matched[i]) continue;
            const Record& record = records_[i];
            bool excludedHit = false;
            for (const QueryTerm& term : parsed.terms)
                if (term.excluded && text::HasTerm(record.text, term)) { excludedHit = true; break; }
            if (excludedHit) continue;
            Candidate candidate{i, 0, {}, false, false};
            int score = 0;
            std::vector<int> marks;
            if (Subsequence(record.label, phrase, score, marks)) {
                candidate.score = score;
                candidate.marks = std::move(marks);
            }
            if (phrase.size() >= 5) {
                // one typo in a word; sharing the first two letters counts for more (spirt: Spirit before Shirt)
                const std::u32string& label = record.label;
                for (std::size_t at = 0, end = 0; at < label.size(); at = end + 1) {
                    end = label.find(U' ', at);
                    if (end == std::u32string::npos) end = label.size();
                    const std::size_t size = end - at;
                    if (size + 1 < phrase.size() || size > phrase.size() + 1) continue;
                    const std::u32string word = label.substr(at, size);
                    if (!WithinOneEdit(word, phrase)) continue;
                    const int edit = 470 + (word.compare(0, 2, phrase, 0, 2) == 0 ? 20 : 0);
                    candidate.score = std::max(candidate.score, edit);
                }
            }
            if (candidate.score > 0) found.push_back(std::move(candidate));
        }
    }

    for (Candidate& c : found) {
        const Record& record = records_[c.record];
        if (record.page >= 0 && static_cast<std::size_t>(record.page) == currentPage) c.score += 60;
        if (record.block >= 0) c.score += 4;  // a card beats the entry that holds it when the title is the same
        c.score -= static_cast<int>(std::min<std::size_t>(record.label.size(), 60)) / 6;
    }
    // every exact hit first, then the typo hits
    std::stable_sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
        if (a.exact != b.exact) return a.exact;
        return a.score > b.score;
    });

    // A card found by its title, whose label is its entry's title, stands for the entry: the entry is not listed again.
    std::set<std::pair<int, int>> coveredEntries;
    for (const Candidate& c : found) {
        const Record& record = records_[c.record];
        if (record.block >= 0 && record.sameAsEntry && !c.fromText) coveredEntries.insert({record.page, record.entry});
    }

    std::map<std::pair<int, int>, int> perEntry;
    for (const Candidate& c : found) {
        if (hits.size() >= limit) break;
        const Record& record = records_[c.record];
        if (record.block < 0 && record.entry >= 0 && coveredEntries.count({record.page, record.entry})) continue;
        if (Grouped(record)) {
            int& count = perEntry[GroupOf(record)];
            if (count >= 6) continue;
            ++count;
        }
        Hit hit;
        hit.page = record.page;
        hit.entry = record.entry;
        hit.block = record.block;
        hit.score = c.score;
        hit.exact = c.exact;
        if (record.extra >= 0) {
            const ExtraRecord& extra = extra_[static_cast<std::size_t>(record.extra)];
            hit.kind = HitKind::Perk;
            hit.label = extra.label;
            hit.context = extra.context;
            hit.extraGroup = extra.group;
            hit.extraItem = extra.item;
            hit.extraSource = extra.source;
        } else if (record.entry < 0) {
            hit.kind = HitKind::Tab;
            hit.label = StripColorCodes(db.pages[static_cast<std::size_t>(record.page)].title);
        } else {
            const Page& page = db.pages[static_cast<std::size_t>(record.page)];
            hit.kind = record.block < 0 ? HitKind::Entry : HitKind::Card;
            const Entry& entry = page.entries[static_cast<std::size_t>(record.entry)];
            hit.context = StripColorCodes(page.title);
            if (record.block < 0) {
                hit.label = StripColorCodes(entry.title);
            } else {
                hit.label = BlockLabel(entry.blocks[static_cast<std::size_t>(record.block)], entry);
                if (hit.label != StripColorCodes(entry.title)) hit.context += " \xE2\x80\xBA " + StripColorCodes(entry.title);
            }
        }
        hit.labelMarks = c.marks;
        if (c.fromText) {
            hit.labelMarks.clear();
            if (record.extra >= 0) {
                MakeSnippet(extra_[static_cast<std::size_t>(record.extra)].lines, parsed, phrase, requiredCount, hit);
            } else if (record.block >= 0) {
                const Block& block = db.pages[static_cast<std::size_t>(record.page)].entries[static_cast<std::size_t>(record.entry)]
                                         .blocks[static_cast<std::size_t>(record.block)];
                MakeSnippet(BlockSearchLines(block), parsed, phrase, requiredCount, hit);
            }
            // terms of the query found in the title: highlight them there
            MarkTerms(text::SearchText(DecodeUtf8(hit.label)), parsed, hit.labelMarks);
        }
        hits.push_back(std::move(hit));
    }
    return hits;
}

}  // namespace esrwiki::search
