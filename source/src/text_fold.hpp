// Case and accent folding, word splitting and the search query language, as data without any Win32.
//
// Folding maps every code point to exactly one code point, so positions in folded text are positions in the original:
//   - ASCII and full-width ASCII (U+FF01..U+FF5E) go to lower-case ASCII; the ideographic space U+3000 and the
//     no-break space U+00A0 become ' '.
//   - Latin-1 Supplement, Latin Extended-A, Latin Extended-B and Latin Extended Additional (Vietnamese) letters lose
//     their accents and go to lower case (é -> e, ł -> l, ș -> s, ơ -> o, ệ -> e). Letters with no plain base letter
//     (æ, ß, þ, ð, ŋ, ...) only go to lower case where they have a one-code-point lower case.
//   - Greek and Greek Extended (polytonic) go to lower case without tonos, dialytika or breathings, and final sigma is
//     sigma (Ά -> α, ΐ -> ι, ἄ -> α, ς -> σ).
//   - Cyrillic U+0400..U+042F goes to lower case, ё -> е and й -> и. Other Cyrillic letters (U+0460 and up) are kept.
//   - Everything else (CJK, Hangul, Arabic, ...) is kept as it is.
// Nothing is translated: a search finds text in the language it is written in.
//
// Words are runs of word characters (IsWordChar on folded text). ASCII punctuation, Latin-1 symbols, general
// punctuation (dashes, curly quotes, ellipsis), arrows, box drawing, CJK symbols and punctuation (、。「」・...) and
// full-width punctuation separate words. CJK text has no spaces, so a run of ideographs is one word.
//
// The query language (ParseQuery), shared by the search suggestions and the in-page filter:
//   word word        every word must be found (AND), as a substring; a word shorter than 3 letters must start a word
//                    (ideographic and Hangul words are exempt: they have no spaces to start at)
//   "two words"      the words must be found together, as whole words in this order, on one line
//   -word -"phrase"  text that has the word or the phrase does not match
// A '-' that is not followed by a term and a quote that is never closed are plain text (they match nothing by
// themselves and are ignored), so a half-typed query never fails.
#pragma once

#include <string>
#include <vector>

namespace esrwiki::text {

std::u32string DecodeUtf8(const std::string& text);
std::string EncodeUtf8(const std::u32string& text);
char32_t FoldChar(char32_t c);
std::u32string Fold(const std::u32string& text);
// UTF-8 in, folded UTF-8 out. Plain ASCII text takes a fast path.
std::string FoldUtf8(const std::string& text);

// True for a character that is part of a word. Give it folded characters.
bool IsWordChar(char32_t folded);
// True for ideographs, kana and Hangul: scripts written without spaces between words.
bool IsIdeographic(char32_t c);
// The words of folded text, in order.
std::vector<std::u32string> SplitWords(const std::u32string& folded);
// Folded text with every separator turned into ' ' (line breaks stay '\n'): one code point out for each one in.
// This is the text that FindTerm / Matches search.
std::u32string SearchText(const std::u32string& text);
std::string SearchTextUtf8(const std::string& text);

struct QueryTerm {
    std::vector<std::u32string> words;    // folded
    std::vector<std::string> words8;      // the same, UTF-8
    bool phrase = false;    // typed in double quotes: whole words, together, in order
    bool excluded = false;  // typed with a leading '-'
    std::string display;    // as typed (not folded), without the '-'; phrases keep their quotes
    std::string folded;     // the words joined by one space, UTF-8 (for highlighting)
};

struct ParsedQuery {
    std::vector<QueryTerm> terms;  // in the order typed
    bool empty() const { return terms.empty(); }
    // True when the query has something to look for (at least one term that is not excluded). A query of only
    // excluded terms filters a page but gives no suggestions.
    bool HasRequired() const;
    bool HasExcluded() const;
    // The folded text of every required term, words and phrases, in the order typed: what to highlight.
    std::vector<std::string> RequiredTerms() const;
    // The required terms as typed, for messages ("No card has all of: frost, "cold damage"").
    std::vector<std::string> RequiredDisplay() const;
    std::vector<std::string> ExcludedDisplay() const;
    // Every word of the required terms, folded, in order (phrases contribute their words).
    std::vector<std::u32string> RequiredWords() const;
};

ParsedQuery ParseQuery(const std::string& utf8);

// Where `term` is in `text` (made by SearchText), from `from`: the start, or npos; `length` gets the matched length in
// code points (in bytes for the UTF-8 form). The exclusion flag is ignored.
std::size_t FindTerm(const std::u32string& text, const QueryTerm& term, std::size_t from = 0, std::size_t* length = nullptr);
std::size_t FindTerm(const std::string& text, const QueryTerm& term, std::size_t from = 0, std::size_t* length = nullptr);
bool HasTerm(const std::string& text, const QueryTerm& term);
// True when `text` (made by SearchTextUtf8) has every required term and no excluded one.
bool Matches(const ParsedQuery& query, const std::string& text);

}  // namespace esrwiki::text
