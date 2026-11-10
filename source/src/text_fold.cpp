#include "text_fold.hpp"

namespace esrwiki::text {
namespace {

// Generated from the Unicode 13 character database (canonical decomposition: the base letter, in lower case; final
// sigma as sigma). One code point out for each one in.
// U+0180..U+024F
constexpr char16_t kLatinExtB[0xD0] = {
    0x0062, 0x0062, 0x0062, 0x0062, 0x0185, 0x0185, 0x0254, 0x0063, 0x0063, 0x0256, 0x0064, 0x0064,
    0x0064, 0x018D, 0x01DD, 0x0259, 0x025B, 0x0066, 0x0066, 0x0067, 0x0263, 0x0195, 0x0269, 0x0069,
    0x006B, 0x006B, 0x006C, 0x019B, 0x026F, 0x006E, 0x006E, 0x006F, 0x006F, 0x006F, 0x01A3, 0x01A3,
    0x0070, 0x0070, 0x0280, 0x01A8, 0x01A8, 0x0283, 0x01AA, 0x0074, 0x0074, 0x0074, 0x0074, 0x0075,
    0x0075, 0x028A, 0x0076, 0x0079, 0x0079, 0x007A, 0x007A, 0x0292, 0x01B9, 0x01B9, 0x01BA, 0x01BB,
    0x01BD, 0x01BD, 0x01BE, 0x01BF, 0x01C0, 0x01C1, 0x01C2, 0x01C3, 0x01C6, 0x0064, 0x01C6, 0x01C9,
    0x006C, 0x01C9, 0x01CC, 0x006E, 0x01CC, 0x0061, 0x0061, 0x0069, 0x0069, 0x006F, 0x006F, 0x0075,
    0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x01DD, 0x0061, 0x0061,
    0x0061, 0x0061, 0x00E6, 0x00E6, 0x0067, 0x0067, 0x0067, 0x0067, 0x006B, 0x006B, 0x006F, 0x006F,
    0x006F, 0x006F, 0x0292, 0x0292, 0x006A, 0x01F3, 0x0064, 0x01F3, 0x0067, 0x0067, 0x0195, 0x01BF,
    0x006E, 0x006E, 0x0061, 0x0061, 0x00E6, 0x00E6, 0x006F, 0x006F, 0x0061, 0x0061, 0x0061, 0x0061,
    0x0065, 0x0065, 0x0065, 0x0065, 0x0069, 0x0069, 0x0069, 0x0069, 0x006F, 0x006F, 0x006F, 0x006F,
    0x0072, 0x0072, 0x0072, 0x0072, 0x0075, 0x0075, 0x0075, 0x0075, 0x0073, 0x0073, 0x0074, 0x0074,
    0x021D, 0x021D, 0x0068, 0x0068, 0x006E, 0x0064, 0x0223, 0x0223, 0x007A, 0x007A, 0x0061, 0x0061,
    0x0065, 0x0065, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x0079, 0x0079,
    0x006C, 0x006E, 0x0074, 0x0237, 0x0238, 0x0239, 0x0061, 0x0063, 0x0063, 0x006C, 0x0074, 0x0073,
    0x007A, 0x0242, 0x0242, 0x0062, 0x0289, 0x028C, 0x0065, 0x0065, 0x006A, 0x006A, 0x024B, 0x0071,
    0x0072, 0x0072, 0x0079, 0x0079,
};
// U+0370..U+03FF
constexpr char16_t kGreek[0x90] = {
    0x0371, 0x0371, 0x0373, 0x0373, 0x02B9, 0x0375, 0x0377, 0x0377, 0x0378, 0x0379, 0x037A, 0x037B,
    0x037C, 0x037D, 0x003B, 0x03F3, 0x0380, 0x0381, 0x0382, 0x0383, 0x0384, 0x00A8, 0x03B1, 0x00B7,
    0x03B5, 0x03B7, 0x03B9, 0x038B, 0x03BF, 0x038D, 0x03C5, 0x03C9, 0x03B9, 0x03B1, 0x03B2, 0x03B3,
    0x03B4, 0x03B5, 0x03B6, 0x03B7, 0x03B8, 0x03B9, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BE, 0x03BF,
    0x03C0, 0x03C1, 0x03A2, 0x03C3, 0x03C4, 0x03C5, 0x03C6, 0x03C7, 0x03C8, 0x03C9, 0x03B9, 0x03C5,
    0x03B1, 0x03B5, 0x03B7, 0x03B9, 0x03C5, 0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x03B5, 0x03B6, 0x03B7,
    0x03B8, 0x03B9, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BE, 0x03BF, 0x03C0, 0x03C1, 0x03C3, 0x03C3,
    0x03C4, 0x03C5, 0x03C6, 0x03C7, 0x03C8, 0x03C9, 0x03B9, 0x03C5, 0x03BF, 0x03C5, 0x03C9, 0x03D7,
    0x03D0, 0x03D1, 0x03D2, 0x03D2, 0x03D2, 0x03D5, 0x03D6, 0x03D7, 0x03D9, 0x03D9, 0x03DB, 0x03DB,
    0x03DD, 0x03DD, 0x03DF, 0x03DF, 0x03E1, 0x03E1, 0x03E3, 0x03E3, 0x03E5, 0x03E5, 0x03E7, 0x03E7,
    0x03E9, 0x03E9, 0x03EB, 0x03EB, 0x03ED, 0x03ED, 0x03EF, 0x03EF, 0x03F0, 0x03F1, 0x03F2, 0x03F3,
    0x03B8, 0x03F5, 0x03F6, 0x03F8, 0x03F8, 0x03F2, 0x03FB, 0x03FB, 0x03FC, 0x037B, 0x037C, 0x037D,
};
// U+1E00..U+1EFF
constexpr char16_t kLatinExtAdditional[0x100] = {
    0x0061, 0x0061, 0x0062, 0x0062, 0x0062, 0x0062, 0x0062, 0x0062, 0x0063, 0x0063, 0x0064, 0x0064,
    0x0064, 0x0064, 0x0064, 0x0064, 0x0064, 0x0064, 0x0064, 0x0064, 0x0065, 0x0065, 0x0065, 0x0065,
    0x0065, 0x0065, 0x0065, 0x0065, 0x0065, 0x0065, 0x0066, 0x0066, 0x0067, 0x0067, 0x0068, 0x0068,
    0x0068, 0x0068, 0x0068, 0x0068, 0x0068, 0x0068, 0x0068, 0x0068, 0x0069, 0x0069, 0x0069, 0x0069,
    0x006B, 0x006B, 0x006B, 0x006B, 0x006B, 0x006B, 0x006C, 0x006C, 0x006C, 0x006C, 0x006C, 0x006C,
    0x006C, 0x006C, 0x006D, 0x006D, 0x006D, 0x006D, 0x006D, 0x006D, 0x006E, 0x006E, 0x006E, 0x006E,
    0x006E, 0x006E, 0x006E, 0x006E, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F,
    0x0070, 0x0070, 0x0070, 0x0070, 0x0072, 0x0072, 0x0072, 0x0072, 0x0072, 0x0072, 0x0072, 0x0072,
    0x0073, 0x0073, 0x0073, 0x0073, 0x0073, 0x0073, 0x0073, 0x0073, 0x0073, 0x0073, 0x0074, 0x0074,
    0x0074, 0x0074, 0x0074, 0x0074, 0x0074, 0x0074, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075,
    0x0075, 0x0075, 0x0075, 0x0075, 0x0076, 0x0076, 0x0076, 0x0076, 0x0077, 0x0077, 0x0077, 0x0077,
    0x0077, 0x0077, 0x0077, 0x0077, 0x0077, 0x0077, 0x0078, 0x0078, 0x0078, 0x0078, 0x0079, 0x0079,
    0x007A, 0x007A, 0x007A, 0x007A, 0x007A, 0x007A, 0x0068, 0x0074, 0x0077, 0x0079, 0x0061, 0x017F,
    0x1E9C, 0x1E9D, 0x00DF, 0x1E9F, 0x0061, 0x0061, 0x0061, 0x0061, 0x0061, 0x0061, 0x0061, 0x0061,
    0x0061, 0x0061, 0x0061, 0x0061, 0x0061, 0x0061, 0x0061, 0x0061, 0x0061, 0x0061, 0x0061, 0x0061,
    0x0061, 0x0061, 0x0061, 0x0061, 0x0065, 0x0065, 0x0065, 0x0065, 0x0065, 0x0065, 0x0065, 0x0065,
    0x0065, 0x0065, 0x0065, 0x0065, 0x0065, 0x0065, 0x0065, 0x0065, 0x0069, 0x0069, 0x0069, 0x0069,
    0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F,
    0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F, 0x006F,
    0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075, 0x0075,
    0x0075, 0x0075, 0x0079, 0x0079, 0x0079, 0x0079, 0x0079, 0x0079, 0x0079, 0x0079, 0x1EFB, 0x1EFB,
    0x1EFD, 0x1EFD, 0x0079, 0x0079,
};
// U+1F00..U+1FFF
constexpr char16_t kGreekExtended[0x100] = {
    0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1,
    0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B5, 0x03B5, 0x03B5, 0x03B5, 0x03B5, 0x03B5, 0x1F16, 0x1F17,
    0x03B5, 0x03B5, 0x03B5, 0x03B5, 0x03B5, 0x03B5, 0x1F1E, 0x1F1F, 0x03B7, 0x03B7, 0x03B7, 0x03B7,
    0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7,
    0x03B9, 0x03B9, 0x03B9, 0x03B9, 0x03B9, 0x03B9, 0x03B9, 0x03B9, 0x03B9, 0x03B9, 0x03B9, 0x03B9,
    0x03B9, 0x03B9, 0x03B9, 0x03B9, 0x03BF, 0x03BF, 0x03BF, 0x03BF, 0x03BF, 0x03BF, 0x1F46, 0x1F47,
    0x03BF, 0x03BF, 0x03BF, 0x03BF, 0x03BF, 0x03BF, 0x1F4E, 0x1F4F, 0x03C5, 0x03C5, 0x03C5, 0x03C5,
    0x03C5, 0x03C5, 0x03C5, 0x03C5, 0x1F58, 0x03C5, 0x1F5A, 0x03C5, 0x1F5C, 0x03C5, 0x1F5E, 0x03C5,
    0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9,
    0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03B1, 0x03B1, 0x03B5, 0x03B5, 0x03B7, 0x03B7, 0x03B9, 0x03B9,
    0x03BF, 0x03BF, 0x03C5, 0x03C5, 0x03C9, 0x03C9, 0x1F7E, 0x1F7F, 0x03B1, 0x03B1, 0x03B1, 0x03B1,
    0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1,
    0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03B7,
    0x03B7, 0x03B7, 0x03B7, 0x03B7, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9,
    0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03C9, 0x03B1, 0x03B1, 0x03B1, 0x03B1,
    0x03B1, 0x1FB5, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x03B1, 0x1FBD, 0x03B9, 0x1FBF,
    0x1FC0, 0x00A8, 0x03B7, 0x03B7, 0x03B7, 0x1FC5, 0x03B7, 0x03B7, 0x03B5, 0x03B5, 0x03B7, 0x03B7,
    0x03B7, 0x1FBF, 0x1FBF, 0x1FBF, 0x03B9, 0x03B9, 0x03B9, 0x03B9, 0x1FD4, 0x1FD5, 0x03B9, 0x03B9,
    0x03B9, 0x03B9, 0x03B9, 0x03B9, 0x1FDC, 0x1FFE, 0x1FFE, 0x1FFE, 0x03C5, 0x03C5, 0x03C5, 0x03C5,
    0x03C1, 0x03C1, 0x03C5, 0x03C5, 0x03C5, 0x03C5, 0x03C5, 0x03C5, 0x03C1, 0x00A8, 0x00A8, 0x0060,
    0x1FF0, 0x1FF1, 0x03C9, 0x03C9, 0x03C9, 0x1FF5, 0x03C9, 0x03C9, 0x03BF, 0x03BF, 0x03C9, 0x03C9,
    0x03C9, 0x00B4, 0x1FFE, 0x1FFF,
};

char32_t FoldOne(char32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c == 0xA0) return U' ';
    if (c >= 0xC0 && c <= 0xFF) {
        static const char* const kLatin1 =  // U+00C0..U+00FF: the base letter, or '-' to keep the letter (lower case)
            "aaaaaa-ceeeeiiii" "-nooooo-ouuuuy--"
            "aaaaaa-ceeeeiiii" "-nooooo-ouuuuy-y";
        const char base = kLatin1[c - 0xC0];
        if (base != '-') return static_cast<char32_t>(base);
        return (c <= 0xDE && c != 0xD7) ? c + 32 : c;  // Æ Ð Þ to lower case; ß × ÷ and the lower case ones stay
    }
    if (c >= 0x100 && c <= 0x17F) {
        static const struct { char32_t last; char base; } kExtA[] = {
            {0x105, 'a'}, {0x10D, 'c'}, {0x111, 'd'}, {0x11B, 'e'}, {0x123, 'g'}, {0x127, 'h'}, {0x131, 'i'}, {0x133, 'i'},
            {0x135, 'j'}, {0x138, 'k'}, {0x142, 'l'}, {0x14B, 'n'}, {0x153, 'o'}, {0x159, 'r'}, {0x161, 's'}, {0x167, 't'},
            {0x173, 'u'}, {0x175, 'w'}, {0x178, 'y'}, {0x17E, 'z'}, {0x17F, 's'}};
        for (const auto& range : kExtA)
            if (c <= range.last) return static_cast<char32_t>(range.base);
    }
    if (c >= 0x180 && c <= 0x24F) return kLatinExtB[c - 0x180];
    if (c >= 0x370 && c <= 0x3FF) return kGreek[c - 0x370];
    if (c >= 0x400 && c <= 0x45F) {
        if (c >= 0x410 && c <= 0x42F) c += 32;            // А-Я
        else if (c <= 0x40F) c += 80;                     // Ѐ-Џ
        if (c == 0x451) return 0x435;                     // ё
        if (c == 0x439) return 0x438;                     // й
        return c;
    }
    if (c >= 0x1E00 && c <= 0x1EFF) return kLatinExtAdditional[c - 0x1E00];
    if (c >= 0x1F00 && c <= 0x1FFF) return kGreekExtended[c - 0x1F00];
    if (c == 0x3000) return U' ';                         // ideographic space
    if (c >= 0xFF01 && c <= 0xFF5E) {                     // full-width ASCII
        c -= 0xFEE0;
        return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    return c;
}

bool IsQuerySpace(char32_t c) { return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r'; }

// Words of folded text with their start positions.
void SplitWordsAt(const std::u32string& folded, std::size_t from, std::size_t to, std::vector<std::u32string>& words,
                  std::vector<std::size_t>& starts) {
    std::size_t start = 0;
    bool inWord = false;
    for (std::size_t i = from; i < to; ++i) {
        const bool w = IsWordChar(folded[i]);
        if (w && !inWord) { start = i; inWord = true; }
        if (!w && inWord) { words.push_back(folded.substr(start, i - start)); starts.push_back(start); inWord = false; }
    }
    if (inWord) { words.push_back(folded.substr(start, to - start)); starts.push_back(start); }
}

std::u32string Trimmed(const std::u32string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && IsQuerySpace(s[a])) ++a;
    while (b > a && IsQuerySpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// A word shorter than 3 letters (not ideographic) only matches at the start of a word.
bool StartOnly(const QueryTerm& term) {
    if (term.phrase || term.words.size() != 1 || term.words[0].size() >= 3) return false;
    for (char32_t c : term.words[0])
        if (IsIdeographic(c)) return false;
    return true;
}

template <typename S>
bool Separator(const S& text, std::size_t at) { return text[at] == ' ' || text[at] == '\n'; }

template <typename S>
std::size_t FindTermIn(const S& text, const std::vector<S>& words, bool phrase, bool startOnly, std::size_t from,
                       std::size_t* length) {
    if (words.empty()) return S::npos;
    const S& first = words[0];
    if (!phrase && !startOnly) {
        const std::size_t p = text.find(first, from);
        if (p != S::npos && length) *length = first.size();
        return p;
    }
    for (std::size_t p = text.find(first, from); p != S::npos; p = text.find(first, p + 1)) {
        if (p > 0 && !Separator(text, p - 1)) continue;
        if (!phrase) {
            if (length) *length = first.size();
            return p;
        }
        std::size_t q = p + first.size();
        bool ok = true;
        for (std::size_t k = 1; ok && k < words.size(); ++k) {
            if (q >= text.size() || text[q] != ' ') { ok = false; break; }
            while (q < text.size() && text[q] == ' ') ++q;
            if (text.compare(q, words[k].size(), words[k]) != 0) { ok = false; break; }
            q += words[k].size();
        }
        if (!ok || (q < text.size() && !Separator(text, q))) continue;
        if (length) *length = q - p;
        return p;
    }
    return S::npos;
}

}  // namespace

char32_t FoldChar(char32_t c) { return FoldOne(c); }

bool IsWordChar(char32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (c <= 0xBF) return c == 0xAA || c == 0xB5 || c == 0xBA;                       // Latin-1 symbols; ª µ º are letters
    if (c == 0xD7 || c == 0xF7) return false;                                          // × ÷
    if (c >= 0x2000 && c <= 0x206F) return false;                                      // general punctuation
    if (c >= 0x20A0 && c <= 0x20CF) return false;                                      // currency
    if (c >= 0x2190 && c <= 0x23FF) return false;                                      // arrows, maths, technical
    if (c >= 0x2500 && c <= 0x27BF) return false;                                      // box drawing, shapes, dingbats
    if (c >= 0x2900 && c <= 0x2BFF) return false;                                      // arrows, maths
    if (c >= 0x2E00 && c <= 0x2E7F) return false;                                      // supplemental punctuation
    if (c >= 0x3000 && c <= 0x303F) return c >= 0x3005 && c <= 0x3007;                // CJK punctuation; 々 〆 〇 are letters
    if (c == 0x30FB) return false;                                                     // katakana middle dot
    if ((c >= 0xFE10 && c <= 0xFE1F) || (c >= 0xFE30 && c <= 0xFE6F) || c == 0xFEFF) return false;
    if ((c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) ||
        (c >= 0xFF5B && c <= 0xFF65) || (c >= 0xFFE0 && c <= 0xFFEE))
        return false;                                                                  // full-width punctuation
    return true;
}

bool IsIdeographic(char32_t c) {
    return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x2E80 && c <= 0x2FDF) || (c >= 0x3040 && c <= 0x31FF) ||
           (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA960 && c <= 0xA97F) ||
           (c >= 0xAC00 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF66 && c <= 0xFFDC) ||
           (c >= 0x20000 && c <= 0x3FFFF);
}

std::u32string DecodeUtf8(const std::string& s) {
    std::u32string out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t code = c;
        std::size_t extra = 0;
        if (c >= 0xF0) { code = c & 0x07; extra = 3; }
        else if (c >= 0xE0) { code = c & 0x0F; extra = 2; }
        else if (c >= 0xC0) { code = c & 0x1F; extra = 1; }
        ++i;
        for (; extra > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80; --extra, ++i)
            code = (code << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        out += code;
    }
    return out;
}

std::string EncodeUtf8(const std::u32string& text) {
    std::string out;
    for (char32_t c : text) {
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::u32string Fold(const std::u32string& text) {
    std::u32string out = text;
    for (char32_t& c : out) c = FoldOne(c);
    return out;
}

std::string FoldUtf8(const std::string& text) {
    bool ascii = true;
    for (unsigned char c : text)
        if (c >= 0x80) { ascii = false; break; }
    if (ascii) {
        std::string out = text;
        for (char& c : out)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        return out;
    }
    return EncodeUtf8(Fold(DecodeUtf8(text)));
}

std::vector<std::u32string> SplitWords(const std::u32string& folded) {
    std::vector<std::u32string> words;
    std::vector<std::size_t> starts;
    SplitWordsAt(folded, 0, folded.size(), words, starts);
    return words;
}

std::u32string SearchText(const std::u32string& text) {
    std::u32string out = Fold(text);
    for (char32_t& c : out)
        if (c != U'\n' && !IsWordChar(c)) c = U' ';
    return out;
}

std::string SearchTextUtf8(const std::string& text) {
    bool ascii = true;
    for (unsigned char c : text)
        if (c >= 0x80) { ascii = false; break; }
    if (!ascii) return EncodeUtf8(SearchText(DecodeUtf8(text)));
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        else if (c != '\n' && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))) c = ' ';
    }
    return out;
}

bool ParsedQuery::HasRequired() const {
    for (const QueryTerm& term : terms)
        if (!term.excluded) return true;
    return false;
}

bool ParsedQuery::HasExcluded() const {
    for (const QueryTerm& term : terms)
        if (term.excluded) return true;
    return false;
}

std::vector<std::string> ParsedQuery::RequiredTerms() const {
    std::vector<std::string> out;
    for (const QueryTerm& term : terms)
        if (!term.excluded) out.push_back(term.folded);
    return out;
}

std::vector<std::string> ParsedQuery::RequiredDisplay() const {
    std::vector<std::string> out;
    for (const QueryTerm& term : terms)
        if (!term.excluded) out.push_back(term.display);
    return out;
}

std::vector<std::string> ParsedQuery::ExcludedDisplay() const {
    std::vector<std::string> out;
    for (const QueryTerm& term : terms)
        if (term.excluded) out.push_back(term.display);
    return out;
}

std::vector<std::u32string> ParsedQuery::RequiredWords() const {
    std::vector<std::u32string> out;
    for (const QueryTerm& term : terms)
        if (!term.excluded) out.insert(out.end(), term.words.begin(), term.words.end());
    return out;
}

ParsedQuery ParseQuery(const std::string& utf8) {
    ParsedQuery query;
    const std::u32string original = DecodeUtf8(utf8);
    std::u32string f = Fold(original);
    for (char32_t& c : f) {
        if (c == 0x201C || c == 0x201D || c == 0x201E) c = U'"';  // curly quotes
        else if (c == 0x2212) c = U'-';                           // minus sign
    }
    const std::size_t n = f.size();
    const auto add = [&](std::vector<std::u32string> words, bool phrase, bool excluded, const std::u32string& display) {
        if (words.empty()) return;
        QueryTerm term;
        term.phrase = phrase;
        term.excluded = excluded;
        for (const std::u32string& w : words) {
            term.words8.push_back(EncodeUtf8(w));
            if (!term.folded.empty()) term.folded += ' ';
            term.folded += term.words8.back();
        }
        term.words = std::move(words);
        term.display = EncodeUtf8(display);
        for (const QueryTerm& other : query.terms)
            if (other.words == term.words && other.phrase == term.phrase && other.excluded == term.excluded) return;
        query.terms.push_back(std::move(term));
    };
    std::size_t i = 0;
    while (i < n) {
        if (IsQuerySpace(f[i])) { ++i; continue; }
        bool excluded = false;
        if (f[i] == U'-') {
            ++i;
            if (i >= n || IsQuerySpace(f[i])) continue;  // a lone '-' is plain text
            excluded = true;
        }
        if (f[i] == U'"') {
            const std::size_t close = f.find(U'"', i + 1);
            if (close != std::u32string::npos) {
                std::vector<std::u32string> words;
                std::vector<std::size_t> starts;
                SplitWordsAt(f, i + 1, close, words, starts);
                add(std::move(words), true, excluded, U"\"" + Trimmed(original.substr(i + 1, close - i - 1)) + U"\"");
                i = close + 1;
                continue;
            }
            ++i;  // never closed: the quote is plain text
        }
        std::size_t end = i;
        while (end < n && !IsQuerySpace(f[end]) && f[end] != U'"') ++end;
        std::vector<std::u32string> words;
        std::vector<std::size_t> starts;
        SplitWordsAt(f, i, end, words, starts);
        if (excluded && words.size() > 1) {
            // "-life-steal": the words as they are written together
            const std::size_t from = starts.front(), to = starts.back() + words.back().size();
            add(std::move(words), true, true, original.substr(from, to - from));
        } else {
            for (std::size_t k = 0; k < words.size(); ++k) {
                const std::u32string display = original.substr(starts[k], words[k].size());
                add({words[k]}, false, excluded, display);
            }
        }
        i = end;
    }
    return query;
}

std::size_t FindTerm(const std::u32string& text, const QueryTerm& term, std::size_t from, std::size_t* length) {
    return FindTermIn(text, term.words, term.phrase, StartOnly(term), from, length);
}

std::size_t FindTerm(const std::string& text, const QueryTerm& term, std::size_t from, std::size_t* length) {
    return FindTermIn(text, term.words8, term.phrase, StartOnly(term), from, length);
}

bool HasTerm(const std::string& text, const QueryTerm& term) { return FindTerm(text, term) != std::string::npos; }

bool Matches(const ParsedQuery& query, const std::string& text) {
    for (const QueryTerm& term : query.terms)
        if (HasTerm(text, term) == term.excluded) return false;
    return true;
}

}  // namespace esrwiki::text
