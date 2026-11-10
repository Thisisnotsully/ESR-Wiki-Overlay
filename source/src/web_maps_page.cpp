// Reader of the official Maps page. See web_maps_page.hpp.
#include "web_maps_page.hpp"

#include "web_html.hpp"

#include <cstddef>
#include <utility>

namespace esrwiki::mapspage {

namespace {

char LowerChar(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Is there a tag named `name` (case-insensitive) starting at `at` ('<' at at, or "</" for a closing tag)?
bool TagAt(const std::string& s, std::size_t at, const char* name, bool closing) {
    std::size_t i = at;
    if (i >= s.size() || s[i] != '<') return false;
    ++i;
    if (closing) {
        if (i >= s.size() || s[i] != '/') return false;
        ++i;
    }
    for (const char* p = name; *p; ++p, ++i)
        if (i >= s.size() || LowerChar(s[i]) != *p) return false;
    return i < s.size() && (s[i] == '>' || s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '/');
}

// Spans [open, close-end) of every element `name`, matched with nesting. `inner` is the content between the tags.
struct Element {
    std::size_t start = 0, innerStart = 0, innerEnd = 0, end = 0;
};

std::vector<Element> Elements(const std::string& s, const char* name, std::size_t from = 0,
                              std::size_t to = std::string::npos) {
    if (to > s.size()) to = s.size();
    std::vector<Element> out;
    std::vector<Element> stack;
    for (std::size_t i = from; i < to; ++i) {
        if (s[i] != '<') continue;
        if (TagAt(s, i, name, false)) {
            Element e;
            e.start = i;
            const std::size_t gt = s.find('>', i);
            if (gt == std::string::npos || gt >= to) break;
            e.innerStart = gt + 1;
            stack.push_back(e);
            i = gt;
        } else if (TagAt(s, i, name, true)) {
            const std::size_t gt = s.find('>', i);
            if (gt == std::string::npos || gt >= to) break;
            if (!stack.empty()) {
                Element e = stack.back();
                stack.pop_back();
                e.innerEnd = i;
                e.end = gt + 1;
                out.push_back(e);
            }
            i = gt;
        }
    }
    // in document order of their start
    for (std::size_t a = 1; a < out.size(); ++a)
        for (std::size_t b = a; b > 0 && out[b - 1].start > out[b].start; --b) std::swap(out[b - 1], out[b]);
    return out;
}

// The value of attribute `name` in the open tag starting at `at` (lowercase name), or empty.
std::string Attribute(const std::string& s, std::size_t at, const std::string& name) {
    const std::size_t gt = s.find('>', at);
    if (gt == std::string::npos) return std::string();
    std::string tag = s.substr(at, gt - at);
    for (char& c : tag) c = LowerChar(c);
    std::size_t p = 0;
    while ((p = tag.find(name, p)) != std::string::npos) {
        const bool boundary = p > 0 && (tag[p - 1] == ' ' || tag[p - 1] == '\t' || tag[p - 1] == '\n' || tag[p - 1] == '\r');
        std::size_t q = p + name.size();
        while (q < tag.size() && tag[q] == ' ') ++q;
        if (!boundary || q >= tag.size() || tag[q] != '=') {
            p += name.size();
            continue;
        }
        ++q;
        while (q < tag.size() && tag[q] == ' ') ++q;
        if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
            const char quote = tag[q];
            const std::size_t e = tag.find(quote, q + 1);
            return tag.substr(q + 1, e == std::string::npos ? std::string::npos : e - q - 1);
        }
        std::size_t e = q;
        while (e < tag.size() && tag[e] != ' ' && tag[e] != '\t' && tag[e] != '\n' && tag[e] != '/') ++e;
        return tag.substr(q, e - q);
    }
    return std::string();
}

// The direct <tr> rows of a table (rows of nested tables left out), each as its direct <td> cells.
std::vector<std::vector<Element>> Rows(const std::string& s, const Element& table) {
    std::vector<std::vector<Element>> rows;
    const auto nested = Elements(s, "table", table.innerStart, table.innerEnd);
    auto insideNested = [&](std::size_t pos) {
        for (const Element& n : nested)
            if (pos > n.start && pos < n.end) return true;
        return false;
    };
    for (const Element& tr : Elements(s, "tr", table.innerStart, table.innerEnd)) {
        if (insideNested(tr.start)) continue;
        std::vector<Element> cells;
        for (const Element& td : Elements(s, "td", tr.innerStart, tr.innerEnd))
            if (!insideNested(td.start)) {
                bool direct = true;  // not inside another td of this row
                for (const Element& other : cells)
                    if (td.start > other.start && td.start < other.end) direct = false;
                if (direct) cells.push_back(td);
            }
        rows.push_back(cells);
    }
    return rows;
}

std::string Inner(const std::string& s, const Element& e) { return s.substr(e.innerStart, e.innerEnd - e.innerStart); }

std::string Joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& p : parts) {
        if (!out.empty()) out += ' ';
        out += p;
    }
    return out;
}

}  // namespace

std::vector<std::string> TextNodes(const std::string& html) {
    std::vector<std::string> out;
    std::size_t i = 0;
    std::string pending;
    auto flush = [&]() {
        const std::string t = web::Clean(pending);
        if (!t.empty()) out.push_back(t);
        pending.clear();
    };
    while (i < html.size()) {
        if (html[i] == '<') {
            flush();
            if (html.compare(i, 4, "<!--") == 0) {
                const std::size_t e = html.find("-->", i + 4);
                i = e == std::string::npos ? html.size() : e + 3;
                continue;
            }
            const bool skip = TagAt(html, i, "script", false) || TagAt(html, i, "style", false);
            const std::size_t gt = html.find('>', i);
            if (gt == std::string::npos) break;
            if (skip) {
                const char* name = TagAt(html, i, "script", false) ? "script" : "style";
                std::size_t j = gt + 1;
                while (j < html.size() && !(html[j] == '<' && TagAt(html, j, name, true))) ++j;
                const std::size_t e = html.find('>', j);
                i = e == std::string::npos ? html.size() : e + 1;
                continue;
            }
            i = gt + 1;
            continue;
        }
        pending += html[i++];
    }
    flush();
    return out;
}

bool Parse(const std::string& html, Page& out, std::string& error) {
    Page page;
    const auto tables = Elements(html, "table");

    // Teleport Restrictions: the first innermost table whose every row has exactly two cells with text.
    for (const Element& t : tables) {
        if (!Elements(html, "table", t.innerStart, t.innerEnd).empty()) continue;  // not innermost
        const auto rows = Rows(html, t);
        if (rows.empty()) continue;
        bool ok = true;
        std::vector<TeleportRow> found;
        for (const auto& cells : rows) {
            if (cells.size() != 2) { ok = false; break; }
            TeleportRow r;
            r.area = Joined(TextNodes(Inner(html, cells[0])));
            r.text = Joined(TextNodes(Inner(html, cells[1])));
            if (r.area.empty() || r.text.empty()) { ok = false; break; }
            found.push_back(r);
        }
        if (!ok) continue;
        page.teleport = found;
        // the heading: the first cell of the enclosing table
        for (const Element& parent : tables) {
            if (parent.start < t.start && parent.end > t.end) {
                const auto prow = Rows(html, parent);
                if (!prow.empty() && !prow[0].empty()) page.teleportTitle = Joined(TextNodes(Inner(html, prow[0][0])));
            }
        }
        break;
    }

    // Special Dungeons/Areas: the <td height="120"> cells.
    std::size_t firstCard = std::string::npos;
    for (const Element& td : Elements(html, "td")) {
        if (Attribute(html, td.start, "height") != "120") continue;
        const std::string inner = Inner(html, td);
        SpecialCard c;
        c.lines = TextNodes(inner);
        if (c.lines.empty()) continue;
        bool bold = false;
        for (std::size_t i = 0; i + 2 < inner.size(); ++i)
            if (inner[i] == '<' && TagAt(inner, i, "b", false)) { bold = true; break; }
        if (bold) {
            c.name = c.lines[0];
            if (c.lines.size() > 1) c.size = c.lines[1];
            if (c.lines.size() > 2) {
                std::vector<std::string> rest(c.lines.begin() + 2, c.lines.end());
                c.location = Joined(rest);
            }
        } else {
            c.note = true;
        }
        if (firstCard == std::string::npos) firstCard = td.start;
        page.cards.push_back(c);
    }
    if (firstCard != std::string::npos) {
        // the heading: the first row's cell of the innermost table holding the cards
        const Element* best = nullptr;
        for (const Element& t : tables)
            if (t.start < firstCard && t.end > firstCard && (best == nullptr || t.start > best->start)) best = &t;
        if (best != nullptr) {
            const auto rows = Rows(html, *best);
            if (!rows.empty() && !rows[0].empty()) page.specialTitle = Joined(TextNodes(Inner(html, rows[0][0])));
        }
    }

    if (page.teleport.empty() && page.cards.empty()) {
        error = "maps page: no teleport rows and no special-area cards";
        return false;
    }
    out = std::move(page);
    return true;
}

}  // namespace esrwiki::mapspage
