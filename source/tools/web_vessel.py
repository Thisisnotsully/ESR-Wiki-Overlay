#!/usr/bin/env python3
"""Vessel of Souls page of wiki-data.txt, from the Vessel of Souls page of the ESR website (vessel_of_souls.htm).

    python web_vessel.py <vessel_of_souls.htm> <wiki-data.txt>

The website gives the overview, four tier tables (organs, effect, and soul cost where it is stated) and the completion
table (blemished ingredients, mythical base, bonus). Each table becomes one entry with its intro and its rows. The
website's colours become the overlay's colour letters. A row with no outcome cell is skipped and reported.
Standard library only (web_uniques.clean). Anything that goes wrong leaves wiki-data.txt untouched and exits 1.
"""
import html as htmllib
import re
import sys
from pathlib import Path

from web_uniques import clean

PAGE_ID = "vessel-of-souls"
PAGE_TITLE = "Vessel of Souls"
PAGE_GROUP = "Endgame"
OVERVIEW = "Overview"
TIERS = ["Tier 1", "Tier 2", "Tier 3", "Tier 4"]
COMPLETION = "Completion"
SECTIONS = ["Tier 1", "Tier 2", "Tier 3", "Tier 4", COMPLETION]
MIN_ROWS = {"Tier 1": 100, "Tier 2": 20, "Tier 3": 15, "Tier 4": 60, COMPLETION: 10}
MIN_OVERVIEW = 5
VESSEL = "Vessel of Souls"
SAME_ITEM = "Same Item"
TIER_HEADINGS = "Organs\x1fEffect\x1fSoul cost"
COMPLETION_HEADINGS = "Blemished ingredients\x1fMythical base\x1fBonus"
SEPARATOR = "\x1f"
BLOCK_LINE = "BLOCK\t0\t\t\t\t\t\t0"
# the website's colour names and hex values, as the overlay's colour letters
COLORS = {"red": "R", "orange": "O", "gold": "K", "yellow": "9", "lightgray": "N", "green": "G", "blue": "L",
          "purple": "P", "white": "W", "gray": "Y", "#bdb76b": "K"}
TOKEN = re.compile(r"<br\s*/?>|<font\b([^>]*)>|</font\s*>|<[^>]+>|([^<]+)", re.I)
COLOR_ATTR = re.compile(r'color="([^"]*)"', re.I)
TABLE = re.compile(r"<table[^>]*>(.*?)</table>", re.S | re.I)
ROW = re.compile(r"<tr[^>]*>(.*?)</tr>", re.S | re.I)
CELL = re.compile(r"<t[dh][^>]*>(.*?)</t[dh]>", re.S | re.I)
COST = re.compile(r"consumes (\d+) souls", re.I)


def runs_of(fragment):
    """The <br>-separated lines of a cell, each a list of runs (colour code or None, text)."""
    lines, current, stack = [], [], []
    for match in TOKEN.finditer(fragment):
        token = match.group(0).lower()
        if token.startswith("<br"):
            lines.append(current)
            current = []
        elif token.startswith("<font"):
            color = COLOR_ATTR.search(match.group(1) or "")
            stack.append(color.group(1).lower() if color else None)
        elif token.startswith("</font"):
            if stack:
                stack.pop()
        elif match.group(2) is not None:
            colored = [c for c in stack if c is not None]
            code = COLORS.get(colored[-1]) if colored else None
            current.append((code, htmllib.unescape(match.group(2))))
    lines.append(current)
    return lines


def plain(runs):
    return clean("".join(text for _, text in runs))


def rendered(runs):
    """A line as LINE text: colour runs wrapped in the overlay's colour markers."""
    raw = "".join(("ÿc" + code + text + "ÿcN") if code else text for code, text in runs)
    return clean(raw)


def cell_lines(fragment):
    """The non-empty lines of a cell, as runs."""
    return [runs for runs in runs_of(fragment) if plain(runs)]


def ingredients(cell):
    """Organs or ingredients: the vessel itself is dropped. A line in brackets belongs to the item beside it: it
    follows the item before it, or comes before the next item when there is none."""
    parts = []
    waiting = []  # bracketed lines with no item before them yet
    for runs in cell_lines(cell):
        text = plain(runs)
        if not parts and not waiting and text == VESSEL:
            continue
        if text.startswith("("):
            if parts:
                parts[-1] = parts[-1] + " " + rendered(runs)
            else:
                waiting.append(rendered(runs))
        else:
            item = rendered(runs)
            if waiting:
                item = item + " " + " ".join(waiting)
                waiting = []
            parts.append(item)
    return "\x1e".join(parts)  # one organ per line, as the cube recipes show their inputs


def outcome(cell):
    """(first outcome line rendered, the notes after it joined) of an outcome cell, or None when it has none."""
    lines = cell_lines(cell)
    if lines and plain(lines[0]) == SAME_ITEM:
        lines = lines[1:]
    if not lines:
        return None
    effect = rendered(lines[0])
    notes = " ".join(rendered(runs) for runs in lines[1:])
    return effect, notes


def parse(html):
    if "</html" not in html.lower():
        raise SystemExit("the saved page is incomplete")
    overview = []
    sections = {}  # title -> {"intro": [...], "rows": [...], "default": cost}
    order = []
    skipped = 0
    tables = TABLE.findall(html)
    if not tables:
        raise SystemExit("the vessel tables are missing")
    overview = [rendered(runs) for runs in cell_lines(CELL.findall(ROW.findall(tables[0])[0])[0])]
    for table in tables[1:]:
        rows = ROW.findall(table)
        if not rows:
            continue
        first = CELL.findall(rows[0])
        if len(first) != 1:
            continue
        heading_lines = cell_lines(first[0])
        if not heading_lines or plain(heading_lines[0]) not in SECTIONS:
            continue
        title = plain(heading_lines[0])
        intro_text = " ".join(plain(runs) for runs in heading_lines[1:])
        match = COST.search(intro_text)
        default = ("%s Souls" % match.group(1)) if match else ""  # the soul cost each recipe states in the intro
        section = {"intro": [rendered(runs) for runs in heading_lines[1:]], "rows": [], "default": default}
        for row in rows[1:]:
            cells = CELL.findall(row)
            if len(cells) == 1:
                if not section["rows"]:
                    section["intro"] += [rendered(runs) for runs in cell_lines(cells[0])]
                else:
                    skipped += 1  # an input with no outcome
                continue
            if len(cells) != 2:
                raise SystemExit("a %s row does not have an input and an outcome" % title)
            lines = cell_lines(cells[1])
            if title == COMPLETION:
                if not lines:
                    raise SystemExit("a completion row has no outcome")
                start = 1 if plain(lines[0]) == SAME_ITEM else 0
                if start >= len(lines):
                    raise SystemExit("a completion row has no outcome")
                base = rendered(lines[start])
                bonus = " ".join(rendered(runs) for runs in lines[start + 1:])
                section["rows"].append(ingredients(cells[0]) + SEPARATOR + base + SEPARATOR + bonus)
            else:
                result = outcome(cells[1])
                if result is None:
                    raise SystemExit("a %s row has no outcome" % title)
                effect, notes = result
                cost = notes if notes else default
                section["rows"].append(ingredients(cells[0]) + SEPARATOR + effect + SEPARATOR + cost)
        sections[title] = section
        order.append(title)
    if order != SECTIONS:
        raise SystemExit("the vessel sections are missing")
    if len(overview) < MIN_OVERVIEW:
        raise SystemExit("the overview is missing")
    for title in SECTIONS:
        if not sections[title]["intro"]:
            raise SystemExit("%s has no intro" % title)
        if len(sections[title]["rows"]) < MIN_ROWS[title]:
            raise SystemExit("%s has %d rows, expected at least %d" % (title, len(sections[title]["rows"]), MIN_ROWS[title]))
    return overview, sections, skipped


def build_page(overview, sections):
    out = ["PAGE\t%s\t%s\t%s" % (PAGE_ID, PAGE_TITLE, PAGE_GROUP), "ENTRY\t%s\t" % OVERVIEW, BLOCK_LINE]
    out += ["LINE\tT\t" + line for line in overview]
    for title in SECTIONS:
        section = sections[title]
        out += ["ENTRY\t%s\t" % title, BLOCK_LINE]
        out += ["LINE\tT\t" + line for line in section["intro"]]
        out.append("LINE\tC\t" + (COMPLETION_HEADINGS if title == COMPLETION else TIER_HEADINGS))
        out += ["LINE\tR\t" + row for row in section["rows"]]
    return out


def splice(current, page):
    lines = current.split("\n")
    trailing = bool(lines) and lines[-1] == ""
    if trailing:
        lines = lines[:-1]
    starts = [i for i, line in enumerate(lines) if line.startswith("PAGE\t")]
    for n, i in enumerate(starts):
        if lines[i].split("\t")[1] == PAGE_ID:
            end = starts[n + 1] if n + 1 < len(starts) else len(lines)
            merged = lines[:i] + page + lines[end:]
            return "\n".join(merged) + ("\n" if trailing else "")
    raise SystemExit("wiki-data.txt has no %s page" % PAGE_ID)


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    page_path, target = Path(argv[1]), Path(argv[2])
    try:
        overview, sections, skipped = parse(page_path.read_text(encoding="utf-8", errors="replace"))
        current = target.read_text(encoding="utf-8")
        merged = splice(current, build_page(overview, sections))
    except SystemExit as stop:
        print("Website vessel of souls: %s" % stop)
        return 1
    tmp = target.with_suffix(target.suffix + ".new")
    tmp.write_text(merged, encoding="utf-8", newline="\n")
    tmp.replace(target)
    rows = sum(len(sections[t]["rows"]) for t in SECTIONS)
    print("Website vessel of souls: %d rows, %d input rows without an outcome skipped" % (rows, skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
