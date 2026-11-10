// Search suggestions for the wiki's search box, as data without any Win32.
//
// The index holds a record for every page, every entry and every block (card) of a Database. A query is read with the
// query language of text_fold.hpp (every word, "exact phrases", -excluded) and matched in the language of the data
// only: case and accents are folded (é = e, Ё = е, ά = α), nothing is translated.
//
// A record is a hit exactly when the in-page filter would keep it: a card is matched on BlockSearchText (its title,
// subtitle, section, fields, rune names, lines, note and tag), an entry on its title and subtitle, a page on its
// title. Hits are then ranked: by the title (exact, prefix, word prefix, substring, all words) above a match in the
// card's text. Only when fewer than kFuzzyBelow records match exactly, a typo-tolerant pass (letters in order, or
// one edit) adds more hits, always ranked below every exact hit. Hits come with the characters to highlight and say
// where they are (page, entry, block) so the overlay can jump straight there.
//
// Records that are not in the Database can be added with SetExtra (the overlay gives the perks of the Specializations
// board, which come from the website page, and the areas of the Campaign Progression map, which come from their own data
// file). They are matched, ranked, counted and listed like cards (at most 6 per group of one source, as per entry), and
// come back as HitKind::Perk with the source, group and item they were given. Each source is replaced on its own.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "text_fold.hpp"
#include "wiki_model.hpp"

namespace esrwiki::search {

// The typo pass runs only when fewer records than this match the query exactly.
constexpr std::size_t kFuzzyBelow = 8;

// What a hit is: a page (a tab of the overlay), an entry, a card, or a record given with SetExtra.
enum class HitKind { Tab, Entry, Card, Perk };

// A record from outside the Database. Its text is folded and matched exactly as a card's.
struct ExtraRecord {
    std::string label;               // the row's name (a perk's name); its first line of text
    std::string context;             // where it is, as for the other hits: "Specializations \u203A Alchemist"
    std::vector<std::string> lines;  // the rest of its text (description, tooltip), one line each; D2 colour codes are removed
    int page = -1;                   // the Database page it shows on (ranked up on that page, and the hit's `page`), or -1
    int group = -1;                  // opaque for the index (the overlay: the specialization tab); also caps 6 hits per group
    int item = -1;                   // opaque for the index (the overlay: the perk on that tab)
    int source = 0;                  // which list it belongs to (SetExtra(source, ...)); opaque otherwise
};

struct Hit {
    HitKind kind = HitKind::Tab;
    int page = -1;
    int entry = -1;  // -1: the page itself
    int block = -1;  // -1: the entry itself
    int score = 0;
    bool exact = true;                 // false for a typo or letters-in-order match: the typed text is not in the target
    std::string label;                 // what the row says (D2 colour codes removed)
    std::vector<int> labelMarks;       // code point positions in `label` to highlight
    std::string context;               // where it is: "Runewords > Weapons"
    std::string snippet;               // for a match in the card's text: the line that matched (empty for a title match)
    std::vector<int> snippetMarks;     // code point positions in `snippet` to highlight
    int extraGroup = -1;               // HitKind::Perk: the record's group and item, as given to SetExtra
    int extraItem = -1;
    int extraSource = -1;              // HitKind::Perk: the record's source
};

// What to say when a query finds nothing (or to show what was understood).
struct QueryHint {
    bool valid = false;                 // the query has something to look for (a term that is not excluded)
    std::vector<std::string> required;  // the required terms as typed; phrases keep their quotes
    std::vector<std::string> excluded;  // the excluded terms as typed, without the '-'
    std::vector<std::string> missing;   // required terms that are on no page, card or entry even on their own
    std::size_t matches = 0;            // records (pages, entries, cards) that match the whole query exactly
};

class Index {
public:
    // Reads `db`. Call again whenever the database is replaced. `Query` must be given the same database. The records given
    // with SetExtra are kept.
    void Build(const Database& db);
    // Replaces all the records from outside the Database (an empty list removes them). The Database's records are kept.
    void SetExtra(std::vector<ExtraRecord> extra);
    // Replaces only the records of `source` (each record's `source` is set to it); the other sources' records are kept,
    // and the records keep their order by source. An empty list removes that source.
    void SetExtra(int source, std::vector<ExtraRecord> extra);
    const std::vector<ExtraRecord>& Extra() const { return extra_; }
    bool Empty() const { return records_.empty(); }
    std::size_t Size() const { return records_.size(); }
    // Which of the SetExtra records match `query` exactly (as the in-page filter would): their positions in Extra()
    // (every source; a caller looks at each record's `source`).
    std::vector<std::size_t> MatchingExtra(const std::string& query) const;

    // The best `limit` hits for `query`; entries of `currentPage` rank a little higher. A query with no required term,
    // or of one letter, gives none.
    std::vector<Hit> Query(const Database& db, const std::string& query, std::size_t currentPage, std::size_t limit = 8) const;

    // How many records match `query` exactly (no typo pass, no limit).
    std::size_t CountMatches(const std::string& query) const;
    // The terms of `query` and which of them are found nowhere: for "No card has all of: a, b" or "Nothing has: c".
    QueryHint Hint(const std::string& query) const;

private:
    struct Record {
        int page, entry, block;
        std::u32string label;   // text::SearchText of the label: folded, separators as ' ', positions as in the label
        std::string text;       // what the query is matched on (text::SearchTextUtf8; first line the title)
        std::size_t titleEnd;   // bytes of `text` before the first '\n'
        bool sameAsEntry;       // a card whose label is its entry's title
        int extra;              // a SetExtra record: its position in extra_ (page as given, entry and block -1); else -1
    };
    std::vector<Record> records_;    // the Database's records, then the extra ones
    std::size_t databaseRecords_ = 0;
    std::vector<ExtraRecord> extra_;
    void AddExtraRecords();
    static void CleanExtra(ExtraRecord& record);
};

// The query as the index and the filter read it (colour codes removed first).
text::ParsedQuery Parse(const std::string& query);

// Text helpers (exposed for tests and for the overlay's highlighting).
std::string StripColorCodes(const std::string& text);  // removes the "ÿc?" codes; table cell separators become spaces
using text::DecodeUtf8;
using text::EncodeUtf8;
using text::Fold;  // lower case, accents removed, one code point out for each one in

}  // namespace esrwiki::search
