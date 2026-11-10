// Reads the Specializations page of the ESR website. See spec_data.hpp.
#include "spec_data.hpp"

#include <cstdlib>

namespace esrwiki::spec {
namespace {

// ---- small helpers (no regular expressions, no dependencies) -------------------------------------------------------------

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void AppendUtf8(std::string& out, unsigned long code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

// The common HTML entities and numeric ones.
std::string Unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const std::size_t end = s.find(';', i);
        if (end == std::string::npos || end - i > 10) {
            out += s[i];
            continue;
        }
        const std::string name = s.substr(i + 1, end - i - 1);
        bool done = true;
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name == "nbsp") AppendUtf8(out, 0xA0);
        else if (name == "ndash") AppendUtf8(out, 0x2013);
        else if (name == "mdash") AppendUtf8(out, 0x2014);
        else if (name == "hellip") AppendUtf8(out, 0x2026);
        else if (name == "rsquo") AppendUtf8(out, 0x2019);
        else if (name == "lsquo") AppendUtf8(out, 0x2018);
        else if (name == "ldquo") AppendUtf8(out, 0x201C);
        else if (name == "rdquo") AppendUtf8(out, 0x201D);
        else if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            char* stop = nullptr;
            const unsigned long code = std::strtoul(name.c_str() + (hex ? 2 : 1), &stop, hex ? 16 : 10);
            if (stop != nullptr && *stop == '\0' && code > 0) AppendUtf8(out, code);
            else done = false;
        } else {
            done = false;
        }
        if (done) i = end;
        else out += s[i];
    }
    return out;
}

// Whitespace runs (and a no-break space) become one space; the ends are trimmed.
std::string Squash(const std::string& s) {
    std::string out;
    bool pendingSpace = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const bool nbsp = c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0;
        if (IsSpace(s[i]) || nbsp) {
            if (nbsp) ++i;
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) out += ' ';
        pendingSpace = false;
        out += s[i];
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

// Position of the tag `<name` (case-insensitive) at or after `from`, that is not a longer tag name (<td> is not <tdx>).
std::size_t FindTag(const std::string& html, const std::string& lowerHtml, const std::string& name, std::size_t from) {
    const std::string needle = "<" + name;
    for (std::size_t at = lowerHtml.find(needle, from); at != std::string::npos; at = lowerHtml.find(needle, at + 1)) {
        const std::size_t next = at + needle.size();
        if (next >= html.size()) return at;
        const char c = html[next];
        if (c == '>' || c == '/' || IsSpace(c)) return at;
    }
    return std::string::npos;
}

// The value of attribute `name` in the tag that starts at `tagStart` ("" when it has none).
std::string Attribute(const std::string& html, std::size_t tagStart, const std::string& name) {
    const std::size_t end = html.find('>', tagStart);
    if (end == std::string::npos) return std::string();
    const std::string tag = Lower(html.substr(tagStart, end - tagStart));
    const std::string key = Lower(name);
    for (std::size_t at = tag.find(key); at != std::string::npos; at = tag.find(key, at + 1)) {
        if (at == 0 || !(IsSpace(tag[at - 1]))) continue;
        std::size_t i = at + key.size();
        while (i < tag.size() && IsSpace(tag[i])) ++i;
        if (i >= tag.size() || tag[i] != '=') continue;
        ++i;
        while (i < tag.size() && IsSpace(tag[i])) ++i;
        if (i >= tag.size()) return std::string();
        const std::size_t valueStart = at;  // use the original-case text from the same offsets
        (void)valueStart;
        const char quote = tag[i];
        if (quote == '"' || quote == '\'') {
            const std::size_t close = tag.find(quote, i + 1);
            if (close == std::string::npos) return std::string();
            return html.substr(tagStart + i + 1, close - i - 1);
        }
        std::size_t close = i;
        while (close < tag.size() && !IsSpace(tag[close])) ++close;
        return html.substr(tagStart + i, close - i);
    }
    return std::string();
}

// The CSS value "<number>px" of `property` inside a style attribute or rule body ("left:32px;" -> 32). `ok` says whether it was found.
double PxOf(const std::string& style, const std::string& property, bool& ok) {
    const std::string lower = Lower(style);
    ok = false;
    for (std::size_t at = lower.find(property); at != std::string::npos; at = lower.find(property, at + 1)) {
        if (at > 0 && (lower[at - 1] != ';' && lower[at - 1] != '{' && !IsSpace(lower[at - 1]))) continue;
        std::size_t i = at + property.size();
        while (i < lower.size() && IsSpace(lower[i])) ++i;
        if (i >= lower.size() || lower[i] != ':') continue;
        ++i;
        while (i < lower.size() && IsSpace(lower[i])) ++i;
        char* stop = nullptr;
        const double value = std::strtod(lower.c_str() + i, &stop);
        if (stop == lower.c_str() + i) continue;
        ok = true;
        return value;
    }
    return 0;
}

double PxOr(const std::string& style, const std::string& property, double fallback) {
    bool ok = false;
    const double value = PxOf(style, property, ok);
    return ok ? value : fallback;
}

// The inner HTML of the element whose opening tag starts at `openAt` and is <div ...>: up to its matching </div>.
std::string DivInner(const std::string& html, const std::string& lowerHtml, std::size_t openAt) {
    const std::size_t contentStart = html.find('>', openAt);
    if (contentStart == std::string::npos) return std::string();
    int depth = 1;
    std::size_t i = contentStart + 1;
    while (i < html.size()) {
        const std::size_t open = FindTag(html, lowerHtml, "div", i);
        const std::size_t close = lowerHtml.find("</div", i);
        if (close == std::string::npos) return html.substr(contentStart + 1);
        if (open != std::string::npos && open < close) {
            ++depth;
            i = open + 4;
            continue;
        }
        if (--depth == 0) return html.substr(contentStart + 1, close - contentStart - 1);
        i = close + 5;
    }
    return html.substr(contentStart + 1);
}

// Text of a piece of HTML: tags dropped, <br> as a line break (\n), entities decoded, each line's spaces squashed, empty
// lines kept out.
std::string LinesOf(const std::string& inner) {
    std::string text;
    for (std::size_t i = 0; i < inner.size();) {
        if (inner[i] == '<') {
            const std::size_t end = inner.find('>', i);
            if (end == std::string::npos) break;
            const std::string tag = Lower(inner.substr(i + 1, end - i - 1));
            if (tag.compare(0, 2, "br") == 0 && (tag.size() == 2 || tag[2] == '/' || IsSpace(tag[2]))) text += '\n';
            i = end + 1;
            continue;
        }
        text += inner[i++];
    }
    text = Unescape(text);
    std::string out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t stop = text.find('\n', start);
        if (stop == std::string::npos) stop = text.size();
        const std::string line = Squash(text.substr(start, stop - start));
        if (!line.empty()) {
            if (!out.empty()) out += '\n';
            out += line;
        }
        start = stop + 1;
    }
    return out;
}

// The text of the first <div class="NAME"> inside `html` (an element's inner HTML), "" when there is none.
std::string DivText(const std::string& html, const std::string& lowerHtml, const std::string& className, std::size_t from = 0,
                    std::size_t* after = nullptr) {
    for (std::size_t at = FindTag(html, lowerHtml, "div", from); at != std::string::npos; at = FindTag(html, lowerHtml, "div", at + 1)) {
        const std::string classes = " " + Attribute(html, at, "class") + " ";
        if (classes.find(" " + className + " ") == std::string::npos) continue;
        const std::string inner = DivInner(html, lowerHtml, at);
        if (after != nullptr) *after = html.find('>', at) + 1 + inner.size();
        return LinesOf(inner);
    }
    return std::string();
}

// "spw-icon-alchemist" -> "alchemist"
std::string SheetOf(const std::string& classes) {
    const std::string prefix = "spw-icon-";
    const std::size_t at = classes.find(prefix);
    if (at == std::string::npos) return std::string();
    std::size_t end = at + prefix.size();
    while (end < classes.size() && !IsSpace(classes[end])) ++end;
    return classes.substr(at + prefix.size(), end - at - prefix.size());
}

// The page's explanation above the board: the text of the table just before the board, one line per <br>, with the
// <font color="orange"> names as D2 orange (ÿcO ... ÿcN) like the rest of the wiki's text.
std::vector<std::string> IntroLines(const std::string& html, const std::string& lowerHtml, std::size_t boardAt) {
    std::vector<std::string> lines;
    std::size_t table = std::string::npos;
    for (std::size_t at = FindTag(html, lowerHtml, "table", 0); at != std::string::npos && at < boardAt;
         at = FindTag(html, lowerHtml, "table", at + 1))
        table = at;
    if (table == std::string::npos) return lines;
    const std::size_t end = lowerHtml.find("</table", table);
    if (end == std::string::npos || end > boardAt) return lines;
    std::string inner = html.substr(table, end - table);
    // colour the <font color=...>name</font> pieces
    std::string marked;
    for (std::size_t i = 0; i < inner.size();) {
        if (inner[i] != '<') {
            marked += inner[i++];
            continue;
        }
        const std::size_t close = inner.find('>', i);
        if (close == std::string::npos) break;
        const std::string tag = Lower(inner.substr(i + 1, close - i - 1));
        if (tag.compare(0, 4, "font") == 0 && tag.find("color") != std::string::npos) {
            const std::string color = Lower(Attribute(inner, i, "color"));
            if (color == "orange") marked += "\xC3\xBF" "cO";
            else if (color == "red") marked += "\xC3\xBF" "c1";
            else if (color == "green" || color == "lime") marked += "\xC3\xBF" "c2";
            else if (color == "yellow") marked += "\xC3\xBF" "c9";
            else marked += "\xC3\xBF" "cH";
        } else if (tag == "/font") {
            marked += "\xC3\xBF" "cN";
        } else if (tag.compare(0, 2, "br") == 0 && (tag.size() == 2 || tag[2] == '/' || IsSpace(tag[2]))) {
            marked += "<br>";
        }
        i = close + 1;
    }
    const std::string text = LinesOf(marked);
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t stop = text.find('\n', start);
        if (stop == std::string::npos) stop = text.size();
        lines.push_back(text.substr(start, stop - start));
        start = stop + 1;
    }
    // The font tags of the page wrap every line, so a colour reset opens the text and shows in the first line: drop the
    // codes that have no visible text after them.
    for (std::string& line : lines) {
        const std::string reset = "\xC3\xBF" "cN";
        while (line.compare(0, reset.size(), reset) == 0) line.erase(0, reset.size());
    }
    // Lines that hold nothing but colour codes are dropped.
    std::vector<std::string> kept;
    for (const std::string& line : lines) {
        std::string visible;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line.compare(i, 2, "\xC3\xBF") == 0 && i + 2 < line.size()) {
                i += 3;
                continue;
            }
            visible += line[i];
        }
        if (!Squash(visible).empty()) kept.push_back(line);
    }
    return kept;
}

}  // namespace

bool Parse(const std::string& html, Data& out, std::string& error) {
    const std::string lowerHtml = Lower(html);
    std::size_t board = lowerHtml.find("class=\"spw-wrap");
    if (board == std::string::npos) board = lowerHtml.find("spw-wrap");
    if (board == std::string::npos) {
        error = "the page has no specializations board (spw-wrap)";
        return false;
    }
    Data data;

    // The board's measures from the page's style, when it has them.
    {
        const std::size_t style = lowerHtml.find("<style");
        const std::size_t styleEnd = style == std::string::npos ? std::string::npos : lowerHtml.find("</style", style);
        if (style != std::string::npos && styleEnd != std::string::npos) {
            const std::string css = html.substr(style, styleEnd - style);
            const auto rule = [&](const std::string& selector) {
                const std::size_t at = css.find(selector + " {");
                if (at == std::string::npos) return std::string();
                const std::size_t end = css.find('}', at);
                return css.substr(at, end == std::string::npos ? std::string::npos : end - at);
            };
            const std::string spw = rule(".spw");
            data.width = PxOr(spw, "width", data.width);
            data.height = PxOr(spw, "height", data.height);
            const std::string perk = rule(".spw-perk");
            data.perkWidth = PxOr(perk, "width", data.perkWidth);
            data.perkHeight = PxOr(perk, "height", data.perkHeight);
            const std::string tab = rule(".spw-tab");
            data.tabTop = PxOr(tab, "top", data.tabTop);
            data.tabWidth = PxOr(tab, "width", data.tabWidth);
            data.tabHeight = PxOr(tab, "height", data.tabHeight);
            const std::string title = rule(".spw-title");
            data.titleLeft = PxOr(title, "left", data.titleLeft);
            data.titleTop = PxOr(title, "top", data.titleTop);
            data.titleWidth = PxOr(title, "width", data.titleWidth);
            data.titleHeight = PxOr(title, "height", data.titleHeight);
            const std::string points = rule(".spw-points");
            data.hintLeft = PxOr(points, "left", data.hintLeft);
            data.hintTop = PxOr(points, "top", data.hintTop);
            data.hintWidth = PxOr(points, "width", data.hintWidth);
            data.hintHeight = PxOr(points, "height", data.hintHeight);
        }
    }

    data.intro = IntroLines(html, lowerHtml, board);
    data.title = DivText(html, lowerHtml, "spw-title", board);
    data.hint = DivText(html, lowerHtml, "spw-points", board);

    // The tabs: <label for="spw-tabN" class="spw-tab spw-tabN" style="left:32px;">Alchemist</label>
    std::vector<std::string> radios;  // ids of the radio inputs in order, to find the checked one
    for (std::size_t at = FindTag(html, lowerHtml, "input", board); at != std::string::npos; at = FindTag(html, lowerHtml, "input", at + 1)) {
        if (Lower(Attribute(html, at, "type")) != "radio") continue;
        const std::size_t end = html.find('>', at);
        const std::string tag = Lower(html.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (tag.find("checked") != std::string::npos) data.defaultTab = static_cast<int>(radios.size());
        radios.push_back(Attribute(html, at, "id"));
        if (radios.size() > 12) break;
    }
    for (std::size_t at = FindTag(html, lowerHtml, "label", board); at != std::string::npos; at = FindTag(html, lowerHtml, "label", at + 1)) {
        const std::string classes = " " + Attribute(html, at, "class") + " ";
        if (classes.find(" spw-tab ") == std::string::npos) continue;
        const std::size_t open = html.find('>', at);
        const std::size_t close = lowerHtml.find("</label", at);
        if (open == std::string::npos || close == std::string::npos) break;
        Tab tab;
        tab.label = LinesOf(html.substr(open + 1, close - open - 1));
        tab.left = PxOr(Attribute(html, at, "style"), "left", 0);
        data.tabs.push_back(tab);
    }
    if (data.tabs.empty()) {
        error = "the page has no tabs";
        return false;
    }

    // The pages: <div class="spw-page spw-pageN"> holds that tab's perks.
    std::size_t perksTotal = 0;
    for (std::size_t pageAt = FindTag(html, lowerHtml, "div", board); pageAt != std::string::npos; pageAt = FindTag(html, lowerHtml, "div", pageAt + 1)) {
        const std::string classes = " " + Attribute(html, pageAt, "class") + " ";
        if (classes.find(" spw-page ") == std::string::npos) continue;
        // which page: the digits of "spw-pageN"
        int number = 0;
        const std::size_t key = classes.find(" spw-page");
        for (std::size_t k = classes.find("spw-page", key + 1); k != std::string::npos; k = classes.find("spw-page", k + 1)) {
            const std::size_t d = k + 8;
            if (d < classes.size() && classes[d] >= '0' && classes[d] <= '9') {
                number = std::atoi(classes.c_str() + d);
                break;
            }
        }
        if (number < 1 || static_cast<std::size_t>(number) > data.tabs.size()) continue;
        Tab& tab = data.tabs[static_cast<std::size_t>(number) - 1];
        const std::string page = DivInner(html, lowerHtml, pageAt);
        const std::string lowerPage = Lower(page);
        for (std::size_t perkAt = FindTag(page, lowerPage, "div", 0); perkAt != std::string::npos; perkAt = FindTag(page, lowerPage, "div", perkAt + 1)) {
            const std::string perkClasses = " " + Attribute(page, perkAt, "class") + " ";
            if (perkClasses.find(" spw-perk ") == std::string::npos) continue;
            const std::string inner = DivInner(page, lowerPage, perkAt);
            const std::string lowerInner = Lower(inner);
            Perk perk;
            const std::string style = Attribute(page, perkAt, "style");
            perk.x = PxOr(style, "left", 0);
            perk.y = PxOr(style, "top", 0);
            // the icon: spw-icon-<sheet> and background-position:-<n>px
            for (std::size_t iconAt = FindTag(inner, lowerInner, "div", 0); iconAt != std::string::npos; iconAt = FindTag(inner, lowerInner, "div", iconAt + 1)) {
                const std::string iconClasses = Attribute(inner, iconAt, "class");
                if (iconClasses.find("spw-icon ") == std::string::npos && iconClasses.find("spw-icon-") == std::string::npos) continue;
                const std::string sheet = SheetOf(iconClasses);
                if (!sheet.empty() && tab.sheet.empty()) tab.sheet = sheet;
                const double offset = PxOr(Attribute(inner, iconAt, "style"), "background-position", 0);
                perk.icon = static_cast<int>(-offset / data.perkWidth + 0.5);
                if (perk.icon < 0) perk.icon = 0;
                break;
            }
            perk.name = DivText(inner, lowerInner, "spw-tip-title");
            perk.text = DivText(inner, lowerInner, "spw-tip-text");
            perk.hint = DivText(inner, lowerInner, "spw-tip-hint");
            if (perk.name.empty() && perk.text.empty()) continue;
            tab.perks.push_back(perk);
            ++perksTotal;
        }
    }
    if (perksTotal == 0) {
        error = "the page has no perks";
        return false;
    }
    if (data.defaultTab < 0 || static_cast<std::size_t>(data.defaultTab) >= data.tabs.size()) data.defaultTab = 0;
    out = data;
    return true;
}

}  // namespace esrwiki::spec
