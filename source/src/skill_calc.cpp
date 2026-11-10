#include "skill_calc.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

namespace esrwiki::skills {

namespace {

// ---------------------------------------------------------------------------------------------------------------
// UTF-8 and JavaScript string helpers
// ---------------------------------------------------------------------------------------------------------------

// Decodes the code point at s[i]; `len` gets its byte length (1 for a malformed byte, which comes back as itself).
char32_t DecodeAt(std::string_view s, std::size_t i, std::size_t& len) {
    const unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) {
        len = 1;
        return c0;
    }
    std::size_t need = 0;
    char32_t cp = 0;
    if ((c0 & 0xE0) == 0xC0) {
        need = 1;
        cp = c0 & 0x1Fu;
    } else if ((c0 & 0xF0) == 0xE0) {
        need = 2;
        cp = c0 & 0x0Fu;
    } else if ((c0 & 0xF8) == 0xF0) {
        need = 3;
        cp = c0 & 0x07u;
    } else {
        len = 1;
        return c0;
    }
    if (i + need >= s.size()) {
        len = 1;
        return c0;
    }
    for (std::size_t k = 1; k <= need; ++k) {
        const unsigned char ck = static_cast<unsigned char>(s[i + k]);
        if ((ck & 0xC0) != 0x80) {
            len = 1;
            return c0;
        }
        cp = (cp << 6) | (ck & 0x3Fu);
    }
    len = need + 1;
    return cp;
}

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// JavaScript's WhiteSpace and LineTerminator (what \s and String.prototype.trim use).
bool IsJsSpace(char32_t c) {
    if (c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20) return true;
    if (c < 0x80) return false;
    return c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
           c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

// Byte length of the JavaScript whitespace at s[i] (0 when there is none).
std::size_t SpaceAt(std::string_view s, std::size_t i) {
    if (i >= s.size()) return 0;
    std::size_t len = 0;
    const char32_t c = DecodeAt(s, i, len);
    return IsJsSpace(c) ? len : 0;
}

std::string_view TrimJs(std::string_view s) {
    std::size_t b = 0;
    for (std::size_t n = SpaceAt(s, b); n != 0; n = SpaceAt(s, b)) b += n;
    std::size_t e = s.size();
    while (e > b) {
        // step back one code point
        std::size_t p = e - 1;
        while (p > b && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) --p;
        std::size_t len = 0;
        const char32_t c = DecodeAt(s, p, len);
        if (p + len != e || !IsJsSpace(c)) break;
        e = p;
    }
    return s.substr(b, e - b);
}

struct LowerRun {
    std::uint32_t first;
    std::uint32_t last;
    std::uint32_t stride;
    std::int32_t delta;
};

// Generated from Node v22.23.4 String.prototype.toLowerCase (single code point mappings):
// {first, last, stride, delta}
const LowerRun kLowerRuns[] = {
    {0x41, 0x5A, 1, 32}, {0xC0, 0xD6, 1, 32}, {0xD8, 0xDE, 1, 32}, {0x100, 0x12E, 2, 1}, {0x132, 0x136, 2, 1},
    {0x139, 0x147, 2, 1}, {0x14A, 0x176, 2, 1}, {0x178, 0x178, 1, -121}, {0x179, 0x17D, 2, 1}, {0x181, 0x181, 1, 210},
    {0x182, 0x184, 2, 1}, {0x186, 0x186, 1, 206}, {0x187, 0x187, 1, 1}, {0x189, 0x18A, 1, 205}, {0x18B, 0x18B, 1, 1},
    {0x18E, 0x18E, 1, 79}, {0x18F, 0x18F, 1, 202}, {0x190, 0x190, 1, 203}, {0x191, 0x191, 1, 1},
    {0x193, 0x193, 1, 205}, {0x194, 0x194, 1, 207}, {0x196, 0x196, 1, 211}, {0x197, 0x197, 1, 209},
    {0x198, 0x198, 1, 1}, {0x19C, 0x19C, 1, 211}, {0x19D, 0x19D, 1, 213}, {0x19F, 0x19F, 1, 214},
    {0x1A0, 0x1A4, 2, 1}, {0x1A6, 0x1A6, 1, 218}, {0x1A7, 0x1A7, 1, 1}, {0x1A9, 0x1A9, 1, 218}, {0x1AC, 0x1AC, 1, 1},
    {0x1AE, 0x1AE, 1, 218}, {0x1AF, 0x1AF, 1, 1}, {0x1B1, 0x1B2, 1, 217}, {0x1B3, 0x1B5, 2, 1},
    {0x1B7, 0x1B7, 1, 219}, {0x1B8, 0x1B8, 1, 1}, {0x1BC, 0x1BC, 1, 1}, {0x1C4, 0x1C4, 1, 2}, {0x1C5, 0x1C5, 1, 1},
    {0x1C7, 0x1C7, 1, 2}, {0x1C8, 0x1C8, 1, 1}, {0x1CA, 0x1CA, 1, 2}, {0x1CB, 0x1DB, 2, 1}, {0x1DE, 0x1EE, 2, 1},
    {0x1F1, 0x1F1, 1, 2}, {0x1F2, 0x1F4, 2, 1}, {0x1F6, 0x1F6, 1, -97}, {0x1F7, 0x1F7, 1, -56}, {0x1F8, 0x21E, 2, 1},
    {0x220, 0x220, 1, -130}, {0x222, 0x232, 2, 1}, {0x23A, 0x23A, 1, 10795}, {0x23B, 0x23B, 1, 1},
    {0x23D, 0x23D, 1, -163}, {0x23E, 0x23E, 1, 10792}, {0x241, 0x241, 1, 1}, {0x243, 0x243, 1, -195},
    {0x244, 0x244, 1, 69}, {0x245, 0x245, 1, 71}, {0x246, 0x24E, 2, 1}, {0x370, 0x372, 2, 1}, {0x376, 0x376, 1, 1},
    {0x37F, 0x37F, 1, 116}, {0x386, 0x386, 1, 38}, {0x388, 0x38A, 1, 37}, {0x38C, 0x38C, 1, 64},
    {0x38E, 0x38F, 1, 63}, {0x391, 0x3A1, 1, 32}, {0x3A3, 0x3AB, 1, 32}, {0x3CF, 0x3CF, 1, 8}, {0x3D8, 0x3EE, 2, 1},
    {0x3F4, 0x3F4, 1, -60}, {0x3F7, 0x3F7, 1, 1}, {0x3F9, 0x3F9, 1, -7}, {0x3FA, 0x3FA, 1, 1},
    {0x3FD, 0x3FF, 1, -130}, {0x400, 0x40F, 1, 80}, {0x410, 0x42F, 1, 32}, {0x460, 0x480, 2, 1}, {0x48A, 0x4BE, 2, 1},
    {0x4C0, 0x4C0, 1, 15}, {0x4C1, 0x4CD, 2, 1}, {0x4D0, 0x52E, 2, 1}, {0x531, 0x556, 1, 48},
    {0x10A0, 0x10C5, 1, 7264}, {0x10C7, 0x10C7, 1, 7264}, {0x10CD, 0x10CD, 1, 7264}, {0x13A0, 0x13EF, 1, 38864},
    {0x13F0, 0x13F5, 1, 8}, {0x1C89, 0x1C89, 1, 1}, {0x1C90, 0x1CBA, 1, -3008}, {0x1CBD, 0x1CBF, 1, -3008},
    {0x1E00, 0x1E94, 2, 1}, {0x1E9E, 0x1E9E, 1, -7615}, {0x1EA0, 0x1EFE, 2, 1}, {0x1F08, 0x1F0F, 1, -8},
    {0x1F18, 0x1F1D, 1, -8}, {0x1F28, 0x1F2F, 1, -8}, {0x1F38, 0x1F3F, 1, -8}, {0x1F48, 0x1F4D, 1, -8},
    {0x1F59, 0x1F5F, 2, -8}, {0x1F68, 0x1F6F, 1, -8}, {0x1F88, 0x1F8F, 1, -8}, {0x1F98, 0x1F9F, 1, -8},
    {0x1FA8, 0x1FAF, 1, -8}, {0x1FB8, 0x1FB9, 1, -8}, {0x1FBA, 0x1FBB, 1, -74}, {0x1FBC, 0x1FBC, 1, -9},
    {0x1FC8, 0x1FCB, 1, -86}, {0x1FCC, 0x1FCC, 1, -9}, {0x1FD8, 0x1FD9, 1, -8}, {0x1FDA, 0x1FDB, 1, -100},
    {0x1FE8, 0x1FE9, 1, -8}, {0x1FEA, 0x1FEB, 1, -112}, {0x1FEC, 0x1FEC, 1, -7}, {0x1FF8, 0x1FF9, 1, -128},
    {0x1FFA, 0x1FFB, 1, -126}, {0x1FFC, 0x1FFC, 1, -9}, {0x2126, 0x2126, 1, -7517}, {0x212A, 0x212A, 1, -8383},
    {0x212B, 0x212B, 1, -8262}, {0x2132, 0x2132, 1, 28}, {0x2160, 0x216F, 1, 16}, {0x2183, 0x2183, 1, 1},
    {0x24B6, 0x24CF, 1, 26}, {0x2C00, 0x2C2F, 1, 48}, {0x2C60, 0x2C60, 1, 1}, {0x2C62, 0x2C62, 1, -10743},
    {0x2C63, 0x2C63, 1, -3814}, {0x2C64, 0x2C64, 1, -10727}, {0x2C67, 0x2C6B, 2, 1}, {0x2C6D, 0x2C6D, 1, -10780},
    {0x2C6E, 0x2C6E, 1, -10749}, {0x2C6F, 0x2C6F, 1, -10783}, {0x2C70, 0x2C70, 1, -10782}, {0x2C72, 0x2C72, 1, 1},
    {0x2C75, 0x2C75, 1, 1}, {0x2C7E, 0x2C7F, 1, -10815}, {0x2C80, 0x2CE2, 2, 1}, {0x2CEB, 0x2CED, 2, 1},
    {0x2CF2, 0x2CF2, 1, 1}, {0xA640, 0xA66C, 2, 1}, {0xA680, 0xA69A, 2, 1}, {0xA722, 0xA72E, 2, 1},
    {0xA732, 0xA76E, 2, 1}, {0xA779, 0xA77B, 2, 1}, {0xA77D, 0xA77D, 1, -35332}, {0xA77E, 0xA786, 2, 1},
    {0xA78B, 0xA78B, 1, 1}, {0xA78D, 0xA78D, 1, -42280}, {0xA790, 0xA792, 2, 1}, {0xA796, 0xA7A8, 2, 1},
    {0xA7AA, 0xA7AA, 1, -42308}, {0xA7AB, 0xA7AB, 1, -42319}, {0xA7AC, 0xA7AC, 1, -42315},
    {0xA7AD, 0xA7AD, 1, -42305}, {0xA7AE, 0xA7AE, 1, -42308}, {0xA7B0, 0xA7B0, 1, -42258},
    {0xA7B1, 0xA7B1, 1, -42282}, {0xA7B2, 0xA7B2, 1, -42261}, {0xA7B3, 0xA7B3, 1, 928}, {0xA7B4, 0xA7C2, 2, 1},
    {0xA7C4, 0xA7C4, 1, -48}, {0xA7C5, 0xA7C5, 1, -42307}, {0xA7C6, 0xA7C6, 1, -35384}, {0xA7C7, 0xA7C9, 2, 1},
    {0xA7CB, 0xA7CB, 1, -42343}, {0xA7CC, 0xA7DA, 2, 1}, {0xA7DC, 0xA7DC, 1, -42561}, {0xA7F5, 0xA7F5, 1, 1},
    {0xFF21, 0xFF3A, 1, 32}, {0x10400, 0x10427, 1, 40}, {0x104B0, 0x104D3, 1, 40}, {0x10570, 0x1057A, 1, 39},
    {0x1057C, 0x1058A, 1, 39}, {0x1058C, 0x10592, 1, 39}, {0x10594, 0x10595, 1, 39}, {0x10C80, 0x10CB2, 1, 64},
    {0x10D50, 0x10D65, 1, 32}, {0x118A0, 0x118BF, 1, 32}, {0x16E40, 0x16E5F, 1, 32}, {0x16EA0, 0x16EB8, 1, 27},
    {0x1E900, 0x1E921, 1, 34},
};

char32_t LowerOf(char32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    std::size_t lo = 0;
    std::size_t hi = sizeof(kLowerRuns) / sizeof(kLowerRuns[0]);
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (kLowerRuns[mid].last < c) lo = mid + 1;
        else hi = mid;
    }
    if (lo == sizeof(kLowerRuns) / sizeof(kLowerRuns[0])) return c;
    const LowerRun& r = kLowerRuns[lo];
    if (c < r.first || (c - r.first) % r.stride != 0) return c;
    return static_cast<char32_t>(static_cast<std::int64_t>(c) + r.delta);
}

bool IsCasedLetter(char32_t c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    if (LowerOf(c) != c) return true;
    return (c >= 0xDF && c <= 0x24F && c != 0xF7) || (c >= 0x370 && c <= 0x3FF) || (c >= 0x400 && c <= 0x52F);
}

// ---------------------------------------------------------------------------------------------------------------
// Numbers as JavaScript prints them
// ---------------------------------------------------------------------------------------------------------------

// A tiny unsigned big integer (little-endian base 2^32) for exact toFixed rounding.
struct BigUInt {
    std::vector<std::uint32_t> limbs;

    explicit BigUInt(std::uint64_t v) {
        while (v != 0) {
            limbs.push_back(static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
            v >>= 32;
        }
    }
    bool IsZero() const { return limbs.empty(); }
    void Trim() {
        while (!limbs.empty() && limbs.back() == 0) limbs.pop_back();
    }
    void MulSmall(std::uint32_t m) {
        std::uint64_t carry = 0;
        for (std::uint32_t& limb : limbs) {
            const std::uint64_t t = static_cast<std::uint64_t>(limb) * m + carry;
            limb = static_cast<std::uint32_t>(t & 0xFFFFFFFFu);
            carry = t >> 32;
        }
        if (carry != 0) limbs.push_back(static_cast<std::uint32_t>(carry));
    }
    void AddSmall(std::uint32_t a) {
        std::uint64_t carry = a;
        for (std::size_t i = 0; i < limbs.size() && carry != 0; ++i) {
            const std::uint64_t t = static_cast<std::uint64_t>(limbs[i]) + carry;
            limbs[i] = static_cast<std::uint32_t>(t & 0xFFFFFFFFu);
            carry = t >> 32;
        }
        if (carry != 0) limbs.push_back(static_cast<std::uint32_t>(carry));
    }
    bool Bit(std::size_t index) const {
        const std::size_t limb = index / 32;
        if (limb >= limbs.size()) return false;
        return ((limbs[limb] >> (index % 32)) & 1u) != 0;
    }
    void ShiftLeft(std::size_t bits) {
        const std::size_t whole = bits / 32;
        const unsigned part = static_cast<unsigned>(bits % 32);
        if (part != 0) {
            std::uint32_t carry = 0;
            for (std::uint32_t& limb : limbs) {
                const std::uint32_t next = limb >> (32 - part);
                limb = (limb << part) | carry;
                carry = next;
            }
            if (carry != 0) limbs.push_back(carry);
        }
        limbs.insert(limbs.begin(), whole, 0u);
        Trim();
    }
    void ShiftRight(std::size_t bits) {
        const std::size_t whole = bits / 32;
        if (whole >= limbs.size()) {
            limbs.clear();
            return;
        }
        limbs.erase(limbs.begin(), limbs.begin() + static_cast<std::ptrdiff_t>(whole));
        const unsigned part = static_cast<unsigned>(bits % 32);
        if (part != 0) {
            for (std::size_t i = 0; i < limbs.size(); ++i) {
                const std::uint32_t high = i + 1 < limbs.size() ? limbs[i + 1] : 0u;
                limbs[i] = (limbs[i] >> part) | (high << (32 - part));
            }
        }
        Trim();
    }
    // Divides by d in place, returning the remainder.
    std::uint32_t DivSmall(std::uint32_t d) {
        std::uint64_t rem = 0;
        for (std::size_t i = limbs.size(); i-- > 0;) {
            const std::uint64_t cur = (rem << 32) | limbs[i];
            limbs[i] = static_cast<std::uint32_t>(cur / d);
            rem = cur % d;
        }
        Trim();
        return static_cast<std::uint32_t>(rem);
    }
    std::string ToDecimal() const {
        if (IsZero()) return "0";
        BigUInt t = *this;
        std::vector<std::uint32_t> chunks;
        while (!t.IsZero()) chunks.push_back(t.DivSmall(1000000000u));
        std::string out = std::to_string(chunks.back());
        for (std::size_t i = chunks.size() - 1; i-- > 0;) {
            std::string part = std::to_string(chunks[i]);
            out.append(9 - part.size(), '0');
            out += part;
        }
        return out;
    }
};

}  // namespace

namespace js {

std::string NumberToString(double v) {
    if (std::isnan(v)) return "NaN";
    if (v == 0) return "0";
    if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
    if (v < 0) return "-" + NumberToString(-v);
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
    const std::string sci(buf, res.ptr);
    const std::size_t epos = sci.find('e');
    std::string digits;
    for (std::size_t i = 0; i < epos; ++i) {
        if (sci[i] != '.') digits += sci[i];
    }
    const int exp10 = std::atoi(sci.c_str() + epos + 1);
    const int k = static_cast<int>(digits.size());
    const int n = exp10 + 1;
    if (k <= n && n <= 21) return digits + std::string(static_cast<std::size_t>(n - k), '0');
    if (0 < n && n <= 21) return digits.substr(0, static_cast<std::size_t>(n)) + "." + digits.substr(static_cast<std::size_t>(n));
    if (-6 < n && n <= 0) return "0." + std::string(static_cast<std::size_t>(-n), '0') + digits;
    const int e = n - 1;
    std::string out = digits.substr(0, 1);
    if (k > 1) out += "." + digits.substr(1);
    out += e < 0 ? "e-" : "e+";
    out += std::to_string(e < 0 ? -e : e);
    return out;
}

std::string ToFixed(double v, int digits) {
    if (digits < 0) digits = 0;
    if (digits > 100) digits = 100;
    if (std::isnan(v)) return "NaN";
    if (std::fabs(v) >= 1e21 || std::isinf(v)) return NumberToString(v);
    std::string sign;
    if (v < 0) {
        sign = "-";
        v = -v;
    }
    std::string m;
    if (v == 0) {
        m = "0";
    } else {
        int e2 = 0;
        const double frac = std::frexp(v, &e2);
        const std::uint64_t mant = static_cast<std::uint64_t>(std::ldexp(frac, 53));
        const int shift = e2 - 53;
        BigUInt n(mant);
        for (int i = 0; i < digits; ++i) n.MulSmall(10);
        if (shift >= 0) {
            n.ShiftLeft(static_cast<std::size_t>(shift));
        } else {
            const std::size_t k = static_cast<std::size_t>(-shift);
            const bool up = n.Bit(k - 1);  // at or past half: JavaScript picks the larger n on a tie
            n.ShiftRight(k);
            if (up) n.AddSmall(1);
        }
        m = n.ToDecimal();
    }
    if (digits != 0) {
        const std::size_t f = static_cast<std::size_t>(digits);
        if (m.size() <= f) m = std::string(f + 1 - m.size(), '0') + m;
        m = m.substr(0, m.size() - f) + "." + m.substr(m.size() - f);
    }
    return sign + m;
}

double StringToNumber(std::string_view s) {
    const std::string_view t = TrimJs(s);
    if (t.empty()) return 0;
    if (t == "Infinity" || t == "+Infinity") return std::numeric_limits<double>::infinity();
    if (t == "-Infinity") return -std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (t.size() > 2 && t[0] == '0') {
        int base = 0;
        if (t[1] == 'x' || t[1] == 'X') base = 16;
        else if (t[1] == 'o' || t[1] == 'O') base = 8;
        else if (t[1] == 'b' || t[1] == 'B') base = 2;
        if (base != 0) {
            double value = 0;
            for (std::size_t i = 2; i < t.size(); ++i) {
                const char ch = t[i];
                int d = -1;
                if (ch >= '0' && ch <= '9') d = ch - '0';
                else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                if (d < 0 || d >= base) return nan;
                value = value * base + d;
            }
            return value;
        }
    }
    // StrDecimalLiteral: [+-] (digits [. digits] | . digits) [e [+-] digits]
    std::size_t i = 0;
    if (t[i] == '+' || t[i] == '-') ++i;
    std::size_t intDigits = 0;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
        ++i;
        ++intDigits;
    }
    std::size_t fracDigits = 0;
    if (i < t.size() && t[i] == '.') {
        ++i;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
            ++i;
            ++fracDigits;
        }
    }
    if (intDigits + fracDigits == 0) return nan;
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
        std::size_t expDigits = 0;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
            ++i;
            ++expDigits;
        }
        if (expDigits == 0) return nan;
    }
    if (i != t.size()) return nan;
    const std::string copy(t);
    return std::strtod(copy.c_str(), nullptr);
}

std::string ToLower(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    char32_t prev = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        std::size_t len = 0;
        const char32_t c = DecodeAt(utf8, i, len);
        if (c < 0x80) {
            out += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
        } else if (c == 0x130) {
            out += 'i';
            AppendUtf8(out, 0x307);
        } else if (c == 0x3A3) {
            // Final sigma: after a letter and not before one.
            char32_t next = 0;
            if (i + len < utf8.size()) {
                std::size_t nlen = 0;
                next = DecodeAt(utf8, i + len, nlen);
            }
            AppendUtf8(out, (IsCasedLetter(prev) && !IsCasedLetter(next)) ? 0x3C2 : 0x3C3);
        } else {
            AppendUtf8(out, LowerOf(c));
        }
        prev = c;
        i += len;
    }
    return out;
}

}  // namespace js

namespace {

// Math.round
double JsRound(double x) {
    if (std::isnan(x) || std::isinf(x)) return x;
    const double r = std::floor(x);
    return (x - r >= 0.5) ? r + 1 : r;
}

// skill-calc.js num(): two decimals at most.
std::string Num(double v) {
    if (!std::isfinite(v)) return "0";
    return js::NumberToString(JsRound(v * 100) / 100);
}

std::string Signed(double v) { return (v >= 0 ? "+" : "") + Num(v); }

bool Truthy(double v) { return v != 0 && !std::isnan(v); }

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

js::Arg NumArg(double v) {
    js::Arg a;
    a.kind = js::Arg::Kind::Number;
    a.number = v;
    return a;
}

js::Arg StrArg(const std::string& s) {
    js::Arg a;
    a.kind = js::Arg::Kind::String;
    a.text = s;
    return a;
}

}  // namespace

namespace js {

std::string Sprintf(std::string_view tmpl, const std::vector<Arg>& vals) {
    // String(tmpl).replace(/%(\+?)(\d*)(?:\.(\d+))?([dsif%])/g, ...)
    std::string out;
    out.reserve(tmpl.size() + 16);
    std::size_t next = 0;
    for (std::size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] != '%') {
            out += tmpl[i++];
            continue;
        }
        std::size_t j = i + 1;
        bool plus = false;
        if (j < tmpl.size() && tmpl[j] == '+') {
            plus = true;
            ++j;
        }
        while (j < tmpl.size() && IsDigit(tmpl[j])) ++j;  // width: ignored
        bool hasPrec = false;
        int prec = 0;
        if (j + 1 < tmpl.size() && tmpl[j] == '.' && IsDigit(tmpl[j + 1])) {
            hasPrec = true;
            ++j;
            while (j < tmpl.size() && IsDigit(tmpl[j])) {
                if (prec < 1000) prec = prec * 10 + (tmpl[j] - '0');
                ++j;
            }
        }
        const char t = j < tmpl.size() ? tmpl[j] : '\0';
        if (t != 'd' && t != 's' && t != 'i' && t != 'f' && t != '%') {
            out += tmpl[i++];
            continue;
        }
        i = j + 1;
        if (t == '%') {
            out += '%';
            continue;
        }
        const Arg* v = next < vals.size() ? &vals[next] : nullptr;
        ++next;
        if (t == 's') {
            if (v == nullptr || v->kind == Arg::Kind::Undefined) continue;
            out += v->kind == Arg::Kind::String ? v->text : NumberToString(v->number);
            continue;
        }
        double x = 0;
        if (v != nullptr) {
            if (v->kind == Arg::Kind::Number) x = v->number;
            else if (v->kind == Arg::Kind::String) x = StringToNumber(v->text);
        }
        if (!Truthy(x)) x = 0;  // +v || 0 (also turns -0 into 0)
        if (plus && x >= 0) out += '+';
        out += hasPrec ? ToFixed(x, prec) : Num(x);
    }
    return out;
}

}  // namespace js

// ---------------------------------------------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------------------------------------------

namespace {

using calc::Expr;
using calc::Op;

struct Token {
    enum class Kind { Num, Id, Str, Op };
    Kind kind = Kind::Num;
    double number = 0;
    std::string text;
};

struct ParseFailure {
    std::string message;
};

bool IsIdStart(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool IsIdChar(char c) { return IsIdStart(c) || IsDigit(c); }

std::string Snippet(std::string_view s, std::size_t pos) {
    // src.slice(pos, pos + 12) counts UTF-16 units; close enough for a message.
    return std::string(s.substr(pos, 12));
}

std::vector<Token> Tokenize(std::string_view src) {
    std::vector<Token> out;
    std::size_t pos = 0;
    while (pos < src.size()) {
        std::size_t p = pos;
        for (std::size_t n = SpaceAt(src, p); n != 0; n = SpaceAt(src, p)) p += n;
        if (p >= src.size()) break;  // only whitespace left
        const char c = src[p];
        Token t;
        if (IsDigit(c)) {
            std::size_t e = p;
            while (e < src.size() && IsDigit(src[e])) ++e;
            t.kind = Token::Kind::Num;
            const std::string digits(src.substr(p, e - p));
            t.number = std::strtod(digits.c_str(), nullptr);
            pos = e;
        } else if (IsIdStart(c)) {
            std::size_t e = p;
            while (e < src.size() && IsIdChar(src[e])) ++e;
            t.kind = Token::Kind::Id;
            t.text = std::string(src.substr(p, e - p));
            pos = e;
        } else if (c == '\'' && src.find('\'', p + 1) != std::string_view::npos) {
            const std::size_t e = src.find('\'', p + 1);
            t.kind = Token::Kind::Str;
            t.text = std::string(src.substr(p + 1, e - p - 1));
            pos = e + 1;
        } else {
            t.kind = Token::Kind::Op;
            const std::string_view two = src.substr(p, 2);
            if (two == "==" || two == "!=" || two == ">=" || two == "<=" || two == "&&" || two == "||") {
                t.text = std::string(two);
                pos = p + 2;
            } else if (std::string_view("-+*/%()?:<>,.!").find(c) != std::string_view::npos) {
                t.text = std::string(1, c);
                pos = p + 1;
            } else {
                throw ParseFailure{"unexpected \"" + Snippet(src, pos) + "\""};
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

// Removes the quotes Excel left around a cell: .replace(/^\s*"|"\s*$/g, '').
std::string StripQuotes(std::string_view s) {
    std::size_t start = 0;
    {
        std::size_t p = 0;
        for (std::size_t n = SpaceAt(s, p); n != 0; n = SpaceAt(s, p)) p += n;
        if (p < s.size() && s[p] == '"') start = p + 1;
    }
    std::string out(s.substr(0, 0));
    // The trailing match: the first '"' at or after `start` that only whitespace follows.
    std::size_t end = s.size();
    for (std::size_t q = s.find('"', start); q != std::string_view::npos; q = s.find('"', q + 1)) {
        std::size_t p = q + 1;
        for (std::size_t n = SpaceAt(s, p); n != 0; n = SpaceAt(s, p)) p += n;
        if (p == s.size()) {
            end = q;
            break;
        }
    }
    return std::string(s.substr(start, end - start));
}

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    std::unique_ptr<Expr> ParseAll() {
        std::unique_ptr<Expr> ast = Ternary();
        if (i_ < toks_.size()) throw ParseFailure{"unexpected \"" + TokenText(toks_[i_]) + "\""};
        return ast;
    }

private:
    static std::string TokenText(const Token& t) {
        return t.kind == Token::Kind::Num ? js::NumberToString(t.number) : t.text;
    }

    bool IsOp(const char* v) const {
        return i_ < toks_.size() && toks_[i_].kind == Token::Kind::Op && toks_[i_].text == v;
    }

    void Eat(const char* v) {
        if (!IsOp(v)) throw ParseFailure{std::string("expected \"") + v + "\""};
        ++i_;
    }

    struct DepthGuard {
        explicit DepthGuard(int& d) : depth(d) {
            if (++depth > 400) throw ParseFailure{"too deeply nested"};
        }
        ~DepthGuard() { --depth; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
        int& depth;
    };

    static std::unique_ptr<Expr> Make(Op op) {
        auto e = std::make_unique<Expr>();
        e->op = op;
        return e;
    }

    std::unique_ptr<Expr> Ternary() {
        DepthGuard guard(depth_);
        std::unique_ptr<Expr> cond = Binary(0);
        if (!IsOp("?")) return cond;
        ++i_;
        std::unique_ptr<Expr> a = Ternary();
        Eat(":");
        std::unique_ptr<Expr> b = Ternary();
        // A sign on the condition can't change which branch is taken, so it's meant for the result.
        if (cond->op == Op::Neg || cond->op == Op::Pos) {
            auto t = Make(Op::Ternary);
            t->c = std::move(cond->a);
            t->a = std::move(a);
            t->b = std::move(b);
            cond->a = std::move(t);
            return cond;
        }
        auto t = Make(Op::Ternary);
        t->c = std::move(cond);
        t->a = std::move(a);
        t->b = std::move(b);
        return t;
    }

    static int LevelOf(const std::string& op) {
        if (op == "||") return 0;
        if (op == "&&") return 1;
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") return 2;
        if (op == "+" || op == "-") return 3;
        if (op == "*" || op == "/" || op == "%") return 4;
        return -1;
    }

    static Op BinaryOp(const std::string& op) {
        if (op == "||") return Op::Or;
        if (op == "&&") return Op::And;
        if (op == "==") return Op::Eq;
        if (op == "!=") return Op::Ne;
        if (op == "<") return Op::Lt;
        if (op == ">") return Op::Gt;
        if (op == "<=") return Op::Le;
        if (op == ">=") return Op::Ge;
        if (op == "+") return Op::Add;
        if (op == "-") return Op::Sub;
        if (op == "*") return Op::Mul;
        if (op == "/") return Op::Div;
        return Op::Mod;
    }

    std::unique_ptr<Expr> Binary(int level) {
        if (level == 5) return Unary();
        std::unique_ptr<Expr> left = Binary(level + 1);
        while (i_ < toks_.size() && toks_[i_].kind == Token::Kind::Op && LevelOf(toks_[i_].text) == level) {
            const Op op = BinaryOp(toks_[i_++].text);
            auto node = Make(op);
            node->a = std::move(left);
            node->b = Binary(level + 1);
            left = std::move(node);
        }
        return left;
    }

    std::unique_ptr<Expr> Unary() {
        DepthGuard guard(depth_);
        if (IsOp("-") || IsOp("+") || IsOp("!")) {
            const std::string op = toks_[i_++].text;
            auto node = Make(op == "-" ? Op::Neg : op == "+" ? Op::Pos : Op::Not);
            node->a = Unary();
            return node;
        }
        return Primary();
    }

    std::unique_ptr<Expr> Primary() {
        if (i_ >= toks_.size()) {
            ++i_;
            throw ParseFailure{"unexpected end"};
        }
        const Token& t = toks_[i_++];
        if (t.kind == Token::Kind::Num) {
            auto n = Make(Op::Number);
            n->value = t.number;
            return n;
        }
        if (t.kind == Token::Kind::Op && t.text == "(") {
            std::unique_ptr<Expr> e = Ternary();
            Eat(")");
            return e;
        }
        if (t.kind != Token::Kind::Id) throw ParseFailure{"unexpected \"" + TokenText(t) + "\""};
        const std::string name = js::ToLower(t.text);
        if (!IsOp("(")) {
            auto n = Make(Op::Ident);
            n->name = name;
            return n;
        }
        ++i_;
        // Lookup: skill('Fire Ball'.blvl), stat('x'.accr), miss('m'.rang), sklvl('S'.lvl.edmn)
        if (i_ < toks_.size() && toks_[i_].kind == Token::Kind::Str) {
            auto ref = Make(Op::Ref);
            ref->func = name;
            ref->name = js::ToLower(toks_[i_++].text);
            while (IsOp(".")) {
                ++i_;
                if (i_ >= toks_.size() ||
                    (toks_[i_].kind != Token::Kind::Id && toks_[i_].kind != Token::Kind::Num)) {
                    ++i_;
                    throw ParseFailure{"bad lookup path"};
                }
                const Token& p = toks_[i_++];
                calc::PathPart part;
                if (p.kind == Token::Kind::Num) {
                    part.isNumber = true;
                    part.number = p.number;
                    part.name = js::NumberToString(p.number);
                } else {
                    part.name = js::ToLower(p.text);
                }
                ref->path.push_back(std::move(part));
            }
            Eat(")");
            return ref;
        }
        auto call = Make(Op::Call);
        call->func = name;
        if (!IsOp(")")) {
            call->args.push_back(Ternary());
            while (IsOp(",")) {
                ++i_;
                call->args.push_back(Ternary());
            }
        }
        Eat(")");
        return call;
    }

    std::vector<Token> toks_;
    std::size_t i_ = 0;
    int depth_ = 0;
};

}  // namespace

std::shared_ptr<const calc::Expr> Compile(const std::string& formula) {
    try {
        Parser parser(Tokenize(StripQuotes(formula)));
        return std::shared_ptr<const Expr>(parser.ParseAll());
    } catch (const ParseFailure& failure) {
        auto e = std::make_shared<Expr>();
        e->op = Op::Error;
        e->message = failure.message;
        return e;
    }
}

const calc::Expr* Compiled(const Formula& formula) {
    if (formula.kind != Formula::Kind::Text || formula.text.empty()) return nullptr;
    if (!formula.compiled) formula.compiled = Compile(formula.text);
    return formula.compiled.get();
}

// ---------------------------------------------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------------------------------------------

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
// Damage per level: [base, levels 2-8, 9-16, 17-22, 23-28, 29+]; length: [base, 2-8, 9-16, 17+].
constexpr double kDamageSteps[5][2] = {{2, 8}, {9, 16}, {17, 22}, {23, 28}, {29, kInf}};
constexpr double kLengthSteps[3][2] = {{2, 8}, {9, 16}, {17, kInf}};

double At(const std::vector<double>& arr, std::size_t i) { return i < arr.size() ? arr[i] : 0; }

template <std::size_t N>
double Levelled(const std::vector<double>& arr, std::size_t off, const double (&steps)[N][2], double lvl) {
    double v = At(arr, off);
    for (std::size_t s = 0; s < N; ++s) {
        const double lo = steps[s][0];
        const double hi = steps[s][1];
        if (lvl >= lo) v += At(arr, off + s + 1) * (std::min(lvl, hi) - lo + 1);
    }
    return v;
}

double Div(double a, double b) { return Truthy(b) ? std::trunc(a / b) : 0; }

// rec.p[i - 1] || 0
double Par(const Record& rec, double i) {
    if (!(i >= 1) || i > static_cast<double>(rec.p.size())) return 0;
    const double v = rec.p[static_cast<std::size_t>(i) - 1];
    return Truthy(v) ? v : 0;
}

// The digits after `prefix` when name is prefix + one or more digits (as a JavaScript +m[1]); -1 when it is not.
double DigitsAfter(std::string_view name, std::string_view prefix) {
    if (name.size() <= prefix.size() || name.substr(0, prefix.size()) != prefix) return -1;
    double v = 0;
    for (std::size_t i = prefix.size(); i < name.size(); ++i) {
        if (!IsDigit(name[i])) return -1;
        v = v * 10 + (name[i] - '0');
    }
    return v;
}

const Formula* FormulaAt(const std::vector<Formula>& list, double index1) {
    if (!(index1 >= 1) || index1 > static_cast<double>(list.size())) return nullptr;
    return &list[static_cast<std::size_t>(index1) - 1];
}

double Pow2(double e) {
    if (e == std::floor(e) && std::fabs(e) < 2000) return std::ldexp(1.0, static_cast<int>(e));
    return std::pow(2.0, e);
}

std::string MissileToken(char a, char b) {
    // MISSILE_TOKENS = { en: 'enma', ex: 'exma', eo: 'enms', ey: 'exms', nm: 'pnma', xm: 'pxma' }
    if (a == 'e' && b == 'n') return "enma";
    if (a == 'e' && b == 'x') return "exma";
    if (a == 'e' && b == 'o') return "enms";
    if (a == 'e' && b == 'y') return "exms";
    if (a == 'n' && b == 'm') return "pnma";
    if (a == 'x' && b == 'm') return "pxma";
    return std::string();
}

}  // namespace

std::size_t Calculator::MemoHash::operator()(const MemoKey& k) const {
    std::size_t h = std::hash<const void*>()(k.rec);
    const auto mix = [&h](std::size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    mix(std::hash<std::string>()(k.key));
    mix(std::hash<double>()(k.lvl + 0.0));
    mix(std::hash<const void*>()(k.self));
    mix(std::hash<double>()(k.selfLvl + 0.0));
    return h;
}

Calculator::Calculator(const Data& data, CalcOptions options) : data_(data), options_(std::move(options)) {
    if (options_.characterLevel == 0) options_.characterLevel = 1;  // opts.ulvl || 1
}

void Calculator::Warn(const char* kind, const std::string& detail) const {
    if (options_.warn) options_.warn(kind, detail);
}

double Calculator::Run(const Record& rec, const Formula& formula, double lvl) {
    if (formula.kind == Formula::Kind::None) return 0;
    if (formula.kind == Formula::Kind::Number) return formula.number;
    if (formula.text.empty()) return 0;
    return RunExpr(rec, Compiled(formula), formula.text, lvl);
}

double Calculator::Run(const Record& rec, const calc::Expr& expr, double lvl) { return RunExpr(rec, &expr, std::string(), lvl); }

double Calculator::RunExpr(const Record& rec, const calc::Expr* expr, const std::string& source, double lvl) {
    if (expr == nullptr) return 0;
    if (expr->op == Op::Error) {
        Warn("parse", source + " (" + expr->message + ")");
        return 0;
    }
    if (++depth_ > 60) {
        --depth_;
        Warn("recursion", rec.k + ": " + source);
        return 0;
    }
    // The skill being described: other skills' formulas that read it back get its level.
    if (depth_ == 1) {
        self_ = &rec;
        selfLvl_ = lvl;
    }
    const double v = Node(*expr, rec, lvl);
    --depth_;
    return v;
}

double Calculator::Cached(const Record& rec, const std::string& key, const Formula* formula, double lvl) {
    if (formula == nullptr || formula->Empty()) return 0;
    MemoKey id{&rec, key, lvl + 0.0, nullptr, 0};
    // Another skill's value may read the described skill's level.
    if (self_ != nullptr && self_->k != rec.k) {
        id.self = self_;
        id.selfLvl = selfLvl_ + 0.0;
    }
    const auto found = memo_.find(id);
    if (found != memo_.end()) return found->second;
    memo_.emplace(id, 0.0);  // breaks self-reference loops
    const double v = Run(rec, *formula, lvl);
    memo_[id] = v;
    return v;
}

double Calculator::Node(const calc::Expr& n, const Record& rec, double lvl) {
    switch (n.op) {
        case Op::Number: return n.value;
        case Op::Ident: return Token(rec, n.name, lvl);
        case Op::Ref: return Ref(rec, n, lvl);
        case Op::Call: return Fn(rec, n, lvl);
        case Op::Ternary: return Truthy(Node(*n.c, rec, lvl)) ? Node(*n.a, rec, lvl) : Node(*n.b, rec, lvl);
        case Op::Neg: return -Node(*n.a, rec, lvl);
        case Op::Pos: return Node(*n.a, rec, lvl);
        case Op::Not: return Truthy(Node(*n.a, rec, lvl)) ? 0 : 1;
        case Op::Error: return 0;
        default: break;
    }
    const double a = Node(*n.a, rec, lvl);
    const double b = Node(*n.b, rec, lvl);
    switch (n.op) {
        case Op::Add: return a + b;
        case Op::Sub: return a - b;
        case Op::Mul: return a * b;
        case Op::Div: return Div(a, b);
        case Op::Mod: return Truthy(b) ? std::fmod(a, b) : 0;
        case Op::Eq: return a == b ? 1 : 0;
        case Op::Ne: return a != b ? 1 : 0;
        case Op::Lt: return a < b ? 1 : 0;
        case Op::Gt: return a > b ? 1 : 0;
        case Op::Le: return a <= b ? 1 : 0;
        case Op::Ge: return a >= b ? 1 : 0;
        case Op::And: return Truthy(a) && Truthy(b) ? 1 : 0;
        case Op::Or: return Truthy(a) || Truthy(b) ? 1 : 0;
        default: break;
    }
    Warn("operator", "?");
    return 0;
}

double Calculator::Fn(const Record& rec, const calc::Expr& n, double lvl) {
    std::vector<double> vals;
    vals.reserve(n.args.size());
    for (const auto& arg : n.args) vals.push_back(Node(*arg, rec, lvl));
    const bool isMin = n.func == "min";
    if (isMin || n.func == "max") {
        if (vals.empty()) return 0;
        double r = vals[0];
        for (std::size_t i = 1; i < vals.size(); ++i) {
            const double v = vals[i];
            if (std::isnan(r) || std::isnan(v)) {
                r = std::numeric_limits<double>::quiet_NaN();
            } else if (isMin ? (v < r || (v == r && v == 0 && std::signbit(v)))
                             : (v > r || (v == r && v == 0 && !std::signbit(v)))) {
                r = v;
            }
        }
        return r;
    }
    Warn("function", n.func);
    return !vals.empty() && Truthy(vals[0]) ? vals[0] : 0;
}

double Calculator::Token(const Record& rec, std::string_view nameIn, double lvl) {
    std::string_view name = nameIn;
    // Aliases fix typos in the data ("me3o" for "m3eo"); a number pins a token to a value.
    if (!data_.db.aliases.empty()) {
        const auto alias = data_.db.aliases.find(std::string(name));
        if (alias != data_.db.aliases.end()) {
            if (alias->second.isNumber) return alias->second.number;
            name = alias->second.name;
        }
    }
    if (name == "lvl") return lvl;
    if (name == "blvl") {
        double hard = rec.mx != 0 && !std::isnan(rec.mx) ? std::min(lvl, rec.mx) : lvl;
        if (options_.hardPoints && self_ != nullptr && rec.k == self_->k) {
            hard = std::min(hard, static_cast<double>(*options_.hardPoints));
        }
        return hard;
    }
    if (name == "ulvl") return options_.characterLevel;
    if (name == "len") return Cached(rec, "len", &rec.len, lvl);
    if (name == "rng") return Cached(rec, "rng", &rec.rng, lvl);
    if (name == "pets") return Cached(rec, "pet", &rec.pet, lvl);
    if (name == "usmc") return ManaCost(rec, lvl);
    if (name == "toht") return rec.th.empty() ? 0 : At(rec.th, 0) + At(rec.th, 1) * (lvl - 1);
    if (name == "edmn" || name == "edmx" || name == "edns" || name == "edxs" || name == "enma" || name == "exma" ||
        name == "enms" || name == "exms" || name == "edln" || name == "pnma" || name == "pxma") {
        return Damage(rec, rec, name, lvl);
    }
    double idx = DigitsAfter(name, "par");
    if (idx >= 0) return Par(rec, idx);
    idx = DigitsAfter(name, "clc");
    if (idx >= 0) return Cached(rec, std::string(name), FormulaAt(rec.c, idx), lvl);
    idx = DigitsAfter(name, "ast");
    if (idx >= 0) return Cached(rec, std::string(name), FormulaAt(rec.as, idx), lvl);
    // passive calcs: pst1-pst9, then ps10-ps14 (the game's tokens are four characters)
    idx = DigitsAfter(name, "pst");
    if (idx < 0 && name.size() == 4 && name[0] == 'p' && name[1] == 's' && name[2] == '1' && IsDigit(name[3])) {
        idx = DigitsAfter(name, "ps");
    }
    if (idx >= 0) return Cached(rec, std::string(name), FormulaAt(rec.ps, idx), lvl);
    if (name.size() == 4 && name[0] == 'l' && name[1] == 'n' && IsDigit(name[2]) && IsDigit(name[3])) {
        return Par(rec, name[2] - '0') + (lvl - 1) * Par(rec, name[3] - '0');
    }
    if (name.size() == 4 && name[0] == 'd' && name[1] == 'm' && IsDigit(name[2]) && IsDigit(name[3])) {
        const double lo = Par(rec, name[2] - '0');
        const double hi = Par(rec, name[3] - '0');
        return lo + Div(110 * lvl * (hi - lo), 100 * (lvl + 6));
    }
    if (name.size() == 4 && name[0] == 'm' && name[1] >= '1' && name[1] <= '3') {
        const std::string which = MissileToken(name[2], name[3]);
        if (!which.empty()) {
            const std::size_t slot = static_cast<std::size_t>(name[1] - '1');
            const Record* ms = slot < rec.ms.size() ? data_.db.Missile(rec.ms[slot]) : nullptr;
            if (ms == nullptr) {
                Warn("missile", rec.k + " " + std::string(name));
                return 0;
            }
            return Damage(*ms, rec, which, lvl);
        }
    }
    Warn("token", std::string(name));
    return 0;
}

// Mana cost in 256ths of a point, as skilldesc's usmc expects.
double Calculator::ManaCost(const Record& rec, double lvl) const {
    if (rec.mn.empty()) return 0;
    const double shift = rec.mn.size() > 2 ? rec.mn[2] : 8;
    const double cost = (At(rec.mn, 0) + At(rec.mn, 1) * (lvl - 1)) * Pow2(shift);
    const double floorCost = At(rec.mn, 3) * 256;
    double m = cost;
    if (std::isnan(cost) || std::isnan(floorCost)) m = std::numeric_limits<double>::quiet_NaN();
    else if (floorCost > cost) m = floorCost;
    return std::trunc(m);
}

// Damage tokens. src is the skill or missile row that holds the damage columns; rec is the skill whose synergy
// formulas apply. Suffix ..ns/..xs/..ms is in 256ths of a point, ..mn/..mx/..ma in points. ed.. is before
// synergies, en.. after.
double Calculator::Damage(const Record& src, const Record& rec, std::string_view which, double lvl) {
    const double scale = Pow2(src.hasHs ? src.hs : 8);
    const bool physical = which == "pnma" || which == "pxma";
    const bool max = which == "edmx" || which == "edxs" || which == "exma" || which == "exms" || which == "pxma";
    if (which == "edln") {
        const double len = Levelled(src.el, 0, kLengthSteps, lvl);
        return Div(len * (100 + Run(rec, src.lsym, lvl)), 100);
    }
    double raw = Levelled(physical ? src.pd : src.ed, max ? 6 : 0, kDamageSteps, lvl) * scale;
    const bool synergy = (which.size() < 2 || which[1] != 'd') || physical;  // en.., ex.., pnma, pxma
    if (synergy) raw = Div(raw * (100 + Run(rec, physical ? src.psym : src.esym, lvl)), 100);
    return (!which.empty() && which.back() == 's') ? raw : Div(raw, 256);
}

double Calculator::Ref(const Record& rec, const calc::Expr& n, double lvl) {
    static const std::string kUndefined = "undefined";
    const std::string& prop = n.path.empty() ? kUndefined : n.path.back().name;
    if (n.func == "stat") {
        const auto it = data_.db.stats.find(n.name);
        return it != data_.db.stats.end() && Truthy(it->second) ? it->second : 0;
    }
    if (n.func == "sksrc") return 0;
    if (n.func == "skill") {
        if (n.name == rec.k) return Token(rec, prop, lvl);
        const auto set = options_.levels.find(n.name);
        if (set != options_.levels.end()) {
            if (prop == "blvl") return set->second.blvl;
            if (prop == "lvl") return set->second.lvl;
            const Record* target = data_.db.Skill(n.name);
            return target != nullptr ? Token(*target, prop, set->second.lvl) : 0;
        }
        // Read back from another skill's formula: the described skill at its own level.
        if (self_ != nullptr && n.name == self_->k) {
            const Record* target = data_.db.Skill(n.name);
            return target != nullptr ? Token(*target, prop, selfLvl_) : 0;
        }
        // A character without the skill gets 0, except for the basic skills everyone has.
        if (data_.db.base.find(n.name) == data_.db.base.end()) return 0;
        const Record* target = data_.db.Skill(n.name);
        if (target == nullptr) {
            Warn("skill", n.name);
            return 0;
        }
        return Token(*target, prop, 1);
    }
    if (n.func == "sklvl") {
        // sklvl('Skill'.lvl.edmn): another skill's value at a level computed here.
        const Record* target = data_.db.Skill(n.name);
        if (target == nullptr) {
            Warn("skill", n.name);
            return 0;
        }
        double at = lvl;
        if (n.path.size() > 1) at = n.path[0].isNumber ? n.path[0].number : Token(rec, n.path[0].name, lvl);
        return Token(*target, prop, at);
    }
    if (n.func == "miss") {
        const Record* ms = data_.db.Missile(n.name);
        if (ms == nullptr) {
            Warn("missile", n.name);
            return 0;
        }
        if (prop == "rang") return Truthy(ms->r) ? ms->r : 0;
        if (prop == "rad") return Truthy(ms->rad) ? ms->rad : 0;
        const double idx = DigitsAfter(prop, "par");
        if (idx >= 0) return Par(*ms, idx);
        return Damage(*ms, rec, prop, lvl);
    }
    Warn("function", n.func + "('" + n.name + "')");
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Description lines
// ---------------------------------------------------------------------------------------------------------------

namespace {

// text.split('\n').reverse().join('\n')
std::string ReverseLines(const std::string& text) {
    if (text.find('\n') == std::string::npos) return text;
    std::vector<std::string_view> parts;
    std::string_view rest(text);
    for (;;) {
        const std::size_t nl = rest.find('\n');
        if (nl == std::string_view::npos) {
            parts.push_back(rest);
            break;
        }
        parts.push_back(rest.substr(0, nl));
        rest.remove_prefix(nl + 1);
    }
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = parts.size(); i-- > 0;) {
        out.append(parts[i]);
        if (i != 0) out += '\n';
    }
    return out;
}

bool IsPercentWord(const std::string& s, std::size_t i) {
    static const char kWord[] = "percent";
    if (i + 7 > s.size()) return false;
    for (std::size_t k = 0; k < 7; ++k) {
        char c = s[i + k];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (c != kWord[k]) return false;
    }
    return true;
}

std::string Tidy(const std::string& in) {
    // .replace(/:(?=[+\-]?\d)/g, ': ')
    std::string a;
    a.reserve(in.size() + 8);
    for (std::size_t i = 0; i < in.size(); ++i) {
        a += in[i];
        if (in[i] == ':') {
            const char n1 = i + 1 < in.size() ? in[i + 1] : '\0';
            const char n2 = i + 2 < in.size() ? in[i + 2] : '\0';
            if (IsDigit(n1) || ((n1 == '+' || n1 == '-') && IsDigit(n2))) a += ' ';
        }
    }
    // .replace(/(\d) ?percent ?/gi, '$1% ')
    std::string b;
    b.reserve(a.size());
    for (std::size_t i = 0; i < a.size();) {
        if (IsDigit(a[i])) {
            std::size_t w = std::string::npos;
            if (i + 1 < a.size() && a[i + 1] == ' ' && IsPercentWord(a, i + 2)) w = i + 2;
            else if (IsPercentWord(a, i + 1)) w = i + 1;
            if (w != std::string::npos) {
                std::size_t end = w + 7;
                if (end < a.size() && a[end] == ' ') ++end;
                b += a[i];
                b += "% ";
                i = end;
                continue;
            }
        }
        b += a[i++];
    }
    // .replace(/[ \t]+/g, ' ')
    std::string c;
    c.reserve(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (b[i] == ' ' || b[i] == '\t') {
            c += ' ';
            while (i + 1 < b.size() && (b[i + 1] == ' ' || b[i + 1] == '\t')) ++i;
        } else {
            c += b[i];
        }
    }
    // .trim()
    return std::string(TrimJs(c));
}

// /(^|[^\d.])[+\-]?0%/.test(text)
bool HasZeroPercent(const std::string& text) {
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != '0' || text[i + 1] != '%') continue;
        if (i == 0 || (!IsDigit(text[i - 1]) && text[i - 1] != '.')) return true;
    }
    return false;
}

}  // namespace

std::optional<std::string> Calculator::FormatLine(const Record& rec, const Line& line, double lvl) {
    if (line.t == 0) return std::nullopt;
    const double a = Run(rec, line.x, lvl);
    const double b = line.y.Empty() ? 0 : Run(rec, line.y, lvl);
    const double c = line.z.Empty() ? 0 : Run(rec, line.z, lvl);
    const std::string& la = line.a;
    const std::string& lb = line.b;
    std::string text;
    switch (line.t) {
        case 3: text = la + Num(a) + lb; break;
        case 4: text = la + Signed(a) + lb; break;
        case 5: text = la + Num(a); break;
        case 6: text = Signed(a) + lb + la; break;
        case 13: text = js::Sprintf(la, {NumArg(a)}); break;
        case 18: text = la; break;
        case 25: text = la + lb; break;
        case 36: {
            // a/b as a decimal; textA when that is exactly 1 (singular), else textB.
            const double v = Truthy(b) ? a / b : a;
            text = js::Sprintf(v == 1 || lb.empty() ? la : lb, {NumArg(v)});
            break;
        }
        case 40: text = js::Sprintf(la, {StrArg(lb)}); break;
        case 63: text = la + ": " + Signed(a) + "% " + lb; break;
        case 67: text = la + ": " + Signed(a) + " " + lb; break;
        case 74: text = js::Sprintf(la, {NumArg(a), NumArg(b), NumArg(c)}); break;
        case 75: text = js::Sprintf(la, {NumArg(a), NumArg(b)}); break;
        case 76: text = js::Sprintf(la, {StrArg(lb), NumArg(a), NumArg(b)}); break;
        case 78: text = js::Sprintf(la, {StrArg(lb)}); break;
        case 79: text = js::Sprintf(la, {StrArg(lb), NumArg(Truthy(b) ? a / b : a)}); break;
        default: text = js::Sprintf(la, {NumArg(a), NumArg(b), NumArg(c)}); break;
    }
    // The game fills the placeholders in stored order, then draws the lines bottom up.
    text = Tidy(ReverseLines(text));
    // A percentage that comes out as 0 ("Resist Magic: 0%") says nothing; hide the line.
    if (HasZeroPercent(text)) return std::nullopt;
    return text;
}

// ---------------------------------------------------------------------------------------------------------------
// Contexts, cards
// ---------------------------------------------------------------------------------------------------------------

CalcOptions OptionsFor(const Data& data, const Context& ctx) {
    CalcOptions options;
    options.characterLevel = ctx.characterLevel;
    if (ctx.hardPoints) options.hardPoints = std::clamp(*ctx.hardPoints, 0, data.levels.synergy);
    if (ctx.skill != nullptr && !ctx.synergyPoints.empty()) {
        const int cap = data.levels.synergy;
        const auto add = [&data, &options, &ctx, cap](const std::vector<std::string>& ids, bool levelLink) {
            for (const std::string& id : ids) {
                const auto it = ctx.synergyPoints.find(id);
                if (it == ctx.synergyPoints.end() || it->second <= 0) continue;
                const Skill* target = data.FindById(id);
                if (target == nullptr) continue;
                const int v = std::min(it->second, levelLink ? data.levels.max : cap);
                SkillLevel level;
                level.lvl = v;
                level.blvl = levelLink ? std::min(v, cap) : v;
                options.levels[target->key] = level;
            }
        };
        add(ctx.skill->synergies, false);
        add(ctx.skill->levelLinks, true);
    }
    return options;
}

double Evaluate(const Data& data, const calc::Expr& expr, const Context& ctx) {
    if (ctx.skill == nullptr || ctx.skill->record == nullptr) return 0;
    Calculator calculator(data, OptionsFor(data, ctx));
    return calculator.Run(*ctx.skill->record, expr, ctx.skillLevel);
}

std::optional<std::string> FormatLine(const Data& data, const Line& line, const Context& ctx) {
    if (ctx.skill == nullptr || ctx.skill->record == nullptr) return std::nullopt;
    Calculator calculator(data, OptionsFor(data, ctx));
    return calculator.FormatLine(*ctx.skill->record, line, ctx.skillLevel);
}

Calculator& Engine::CalculatorFor(const Context& ctx) {
    CalcOptions options = OptionsFor(data_, ctx);
    bool same = calc_ != nullptr && clvl_ == options.characterLevel && hp_ == options.hardPoints &&
                levels_.size() == options.levels.size();
    if (same) {
        for (const auto& [key, level] : options.levels) {
            const auto it = levels_.find(key);
            if (it == levels_.end() || it->second.blvl != level.blvl || it->second.lvl != level.lvl) {
                same = false;
                break;
            }
        }
    }
    if (!same) {
        clvl_ = options.characterLevel;
        hp_ = options.hardPoints;
        levels_ = options.levels;
        calc_ = std::make_unique<Calculator>(data_, std::move(options));
    }
    return *calc_;
}

std::optional<std::string> Engine::FormatLine(const Context& ctx, const Line& line) {
    if (ctx.skill == nullptr || ctx.skill->record == nullptr) return std::nullopt;
    return CalculatorFor(ctx).FormatLine(*ctx.skill->record, line, ctx.skillLevel);
}

CardLines Engine::Card(const Context& ctx) {
    CardLines out;
    if (ctx.skill == nullptr || ctx.skill->record == nullptr) return out;
    Calculator& calculator = CalculatorFor(ctx);
    const auto fill = [&calculator, &ctx](const std::vector<Line>& lines, std::vector<CardLine>& list) {
        for (const Line& line : lines) {
            std::optional<std::string> text = calculator.FormatLine(*ctx.skill->record, line, ctx.skillLevel);
            if (!text || text->empty()) continue;
            CardLine cl;
            cl.text = std::move(*text);
            cl.header = line.t == 40;
            list.push_back(std::move(cl));
        }
    };
    fill(ctx.skill->general, out.general);
    fill(ctx.skill->levelLines, out.current);
    fill(ctx.skill->synergyLines, out.synergies);
    return out;
}

bool UsesCalc(const Line& line) {
    return (!line.x.Empty() || !line.y.Empty() || !line.z.Empty()) && line.t != 18 && line.t != 25 && line.t != 40 &&
           line.t != 78;
}

bool ScalesWithCharacter(const Data& data, const Skill& skill) {
    if (skill.record == nullptr) return false;
    CalcOptions lowOptions;
    lowOptions.characterLevel = 1;
    CalcOptions highOptions;
    highOptions.characterLevel = data.levels.max;
    Calculator low(data, std::move(lowOptions));
    Calculator high(data, std::move(highOptions));
    for (const std::vector<Line>* group : {&skill.general, &skill.levelLines, &skill.synergyLines}) {
        for (const Line& line : *group) {
            if (!UsesCalc(line)) continue;
            for (const int lv : {1, 20, data.levels.max}) {
                if (low.FormatLine(*skill.record, line, lv) != high.FormatLine(*skill.record, line, lv)) return true;
            }
        }
    }
    return false;
}

int HardPointsAt(const Data& data, int skillLevel, std::optional<int> hardPoints) {
    return hardPoints ? std::min(*hardPoints, skillLevel) : std::min(skillLevel, data.levels.synergy);
}

}  // namespace esrwiki::skills
