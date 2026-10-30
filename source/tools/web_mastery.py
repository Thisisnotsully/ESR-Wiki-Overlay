#!/usr/bin/env python3
"""Weapon Mastery page of wiki-data.txt, from the Weapon Mastery page of the ESR website (weapon_mastery.htm).

    python web_mastery.py <weapon_mastery.htm> <wiki-data.txt>

The website gives the overview, the general recipes table (ingredients, result) and one table per specialization
variant (skill, effect). Each variant is an entry of its own, as the Weapon Mastery tab has always shown them. Tables
are laid out like the cube recipes and the Vessel of Souls: each ingredient on its own line. The website's colours
become the overlay's colour letters. Standard library only (web_uniques.clean). Anything that goes wrong leaves
wiki-data.txt untouched and exits 1.
"""
import html as htmllib
import re
import sys
from pathlib import Path

from web_uniques import clean

PAGE_ID = "weapon-mastery"
PAGE_TITLE = "Weapon Mastery"
PAGE_GROUP = "Endgame"
OVERVIEW = "Overview"
GENERAL = "General Recipes"
SPECIALIZATIONS = "Specializations"
RECIPE_HEADINGS = "Ingredients\x1fResult"
SKILL_HEADINGS = "Ingredients\x1fSkill\x1fEffect"
SEPARATOR = "\x1f"
LINE_BREAK = "\x1e"
BLOCK_LINE = "BLOCK\t0\t\t\t\t\t\t0"
MIN_OVERVIEW = 4
MIN_RECIPES = 2
MIN_VARIANTS = 10
# the website's colour names and hex values, as the overlay's colour letters
COLORS = {"red": "R", "orange": "O", "gold": "K", "yellow": "9", "lightgray": "N", "green": "G", "blue": "L",
          "purple": "P", "white": "W", "gray": "Y", "#bdb76b": "K", "#928a70": "N", "#706850": "H"}
TOKEN = re.compile(r"<br\s*/?>|<font\b([^>]*)>|</font\s*>|<[^>]+>|([^<]+)", re.I)
COLOR_ATTR = re.compile(r'color="([^"]*)"', re.I)
TABLE = re.compile(r"<table[^>]*>(.*?)</table>", re.S | re.I)
ROW = re.compile(r"<tr[^>]*>(.*?)</tr>", re.S | re.I)
CELL = re.compile(r"<t[dh][^>]*>(.*?)</t[dh]>", re.S | re.I)


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


def heading_and_rows(table, expected):
    """(title, rows of cells) of a table: its heading row names it, the rows after it hold two cells each."""
    rows = ROW.findall(table)
    head = CELL.findall(rows[0]) if rows else []
    if not head:
        raise SystemExit("a table has no heading")
    heading = cell_lines(head[0])
    title = plain(heading[0]) if heading else ""
    if title != expected:
        raise SystemExit("the table %r is not %s" % (title, expected))
    return [CELL.findall(row) for row in rows[1:]]


def skills_of(cell):
    """(skill, effect) pairs of a specialization's outcome cell. An all-orange line names a skill; the lines after it,
    up to the next skill, are its effect."""
    pairs = []
    for runs in cell_lines(cell):
        significant = [(code, text) for code, text in runs if text.strip()]
        if significant and all(code == "O" for code, _ in significant):
            pairs.append([rendered(runs), []])
        elif pairs:
            pairs[-1][1].append(rendered(runs))
        else:
            raise SystemExit("a specialization has an effect with no skill")
    return [(skill, " ".join(effect)) for skill, effect in pairs]


def parse(html):
    if "</html" not in html.lower():
        raise SystemExit("the saved page is incomplete")
    tables = TABLE.findall(html)
    if len(tables) < 3:
        raise SystemExit("the weapon mastery tables are missing")
    overview_cell = CELL.findall(ROW.findall(tables[0])[0])[0]
    overview = [rendered(runs) for runs in cell_lines(overview_cell)]
    if len(overview) < MIN_OVERVIEW:
        raise SystemExit("the overview is missing")
    recipes = []
    for cells in heading_and_rows(tables[1], GENERAL):
        if len(cells) != 2:
            raise SystemExit("a general recipe does not have an ingredients and a result")
        ingredients = LINE_BREAK.join(rendered(runs) for runs in cell_lines(cells[0]))
        result = LINE_BREAK.join(rendered(runs) for runs in cell_lines(cells[1]))
        if not ingredients or not result:
            raise SystemExit("a general recipe is missing its ingredients or its result")
        recipes.append(ingredients + SEPARATOR + result)
    variants = []
    for cells in heading_and_rows(tables[2], SPECIALIZATIONS):
        if len(cells) != 2:
            raise SystemExit("a specialization does not have a name and an outcome")
        lines = cell_lines(cells[0])
        if len(lines) < 2:
            raise SystemExit("a specialization has no name and ingredients")
        name = plain(lines[0])
        ingredients = LINE_BREAK.join(rendered(runs) for runs in lines[1:])
        rows = [ingredients + SEPARATOR + skill + SEPARATOR + effect for skill, effect in skills_of(cells[1])]
        if not rows:
            raise SystemExit("%s has no skills" % name)
        variants.append((name, rows))
    if len(recipes) < MIN_RECIPES:
        raise SystemExit("the general recipes have %d rows, expected at least %d" % (len(recipes), MIN_RECIPES))
    if len(variants) < MIN_VARIANTS:
        raise SystemExit("the specializations have %d variants, expected at least %d" % (len(variants), MIN_VARIANTS))
    return overview, recipes, variants


def build_page(overview, recipes, variants):
    out = ["PAGE\t%s\t%s\t%s" % (PAGE_ID, PAGE_TITLE, PAGE_GROUP), "ENTRY\t%s\t" % OVERVIEW, BLOCK_LINE]
    out += ["LINE\tT\t" + line for line in overview]
    out += ["ENTRY\t%s\t" % GENERAL, BLOCK_LINE, "LINE\tC\t" + RECIPE_HEADINGS]
    out += ["LINE\tR\t" + row for row in recipes]
    for name, rows in variants:
        out += ["ENTRY\t%s\t" % name, BLOCK_LINE, "LINE\tC\t" + SKILL_HEADINGS]
        out += ["LINE\tR\t" + row for row in rows]
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
        overview, recipes, variants = parse(page_path.read_text(encoding="utf-8", errors="replace"))
        current = target.read_text(encoding="utf-8")
        merged = splice(current, build_page(overview, recipes, variants))
    except SystemExit as stop:
        print("Website weapon mastery: %s" % stop)
        return 1
    tmp = target.with_suffix(target.suffix + ".new")
    tmp.write_text(merged, encoding="utf-8", newline="\n")
    tmp.replace(target)
    print("Website weapon mastery: %d specialization variants, %d general recipes" % (len(variants), len(recipes)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
