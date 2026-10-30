#!/usr/bin/env python3
"""Kill Ledger page of wiki-data.txt, from the Kill Ledger page of the ESR website (kill_ledger.htm).

    python web_kill.py <kill_ledger.htm> <wiki-data.txt>

The website gives the overview, a table of kill bonuses (kills, gem, bonus) and a table of boss bonuses (ingredients,
boss, bonus). The website's colours become the overlay's colour letters. Standard library only (web_uniques.clean).
Anything that goes wrong leaves wiki-data.txt untouched and exits 1.
"""
import html as htmllib
import re
import sys
from pathlib import Path

from web_uniques import clean

PAGE_ID = "kill-ledger"
PAGE_TITLE = "Kill Ledger"
PAGE_GROUP = "Endgame"
OVERVIEW = "Overview"
KILLS = "Kill Bonuses"
BOSSES = "Boss Bonuses"
SECTIONS = [KILLS, BOSSES]
MIN_ROWS = {KILLS: 5, BOSSES: 10}
MIN_OVERVIEW = 5
LEDGER = "Kill Ledger"
JUST_KILLED = " just killed"
KILLS_HEADINGS = "Kills\x1fGem\x1fBonus"
BOSS_HEADINGS = "Ingredients\x1fBoss\x1fBonus"
SEPARATOR = "\x1f"
BLOCK_LINE = "BLOCK\t0\t\t\t\t\t\t0"
COUNT = re.compile(r"^\(At least (\d+) kills\)$", re.I)
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


def parse_input(cell):
    """(kills, gem, boss) of a kill bonus or boss bonus input cell. Kill rows have a count, boss rows a boss."""
    lines = cell_lines(cell)
    if lines and plain(lines[0]) == LEDGER:
        lines = lines[1:]
    return lines


def kill_row(inputs, outcome):
    lines = parse_input(inputs)
    if len(lines) != 2:
        raise SystemExit("a kill bonus row does not have a count and a gem")
    count = COUNT.match(plain(lines[0]))
    if not count:
        raise SystemExit("a kill bonus row has no kill count")
    bonus = bonus_of(outcome)
    return "%s kills" % count.group(1) + SEPARATOR + rendered(lines[1]) + SEPARATOR + bonus


def boss_row(inputs, outcome):
    lines = parse_input(inputs)
    if len(lines) < 2:
        raise SystemExit("a boss bonus row has no ingredients and a boss")
    last = plain(lines[-1])
    if not last.endswith(JUST_KILLED):
        raise SystemExit("a boss bonus row does not end with a boss that was just killed")
    boss = last[: -len(JUST_KILLED)]
    # one ingredient per line, as the Vessel of Souls organs and the cube recipes
    ingredients = "\x1e".join(rendered(runs) for runs in lines[:-1])
    return ingredients + SEPARATOR + boss + SEPARATOR + bonus_of(outcome)


def bonus_of(outcome):
    lines = cell_lines(outcome)
    if lines and plain(lines[0]) == LEDGER:
        lines = lines[1:]
    if not lines:
        raise SystemExit("a row has no bonus")
    return " ".join(rendered(runs) for runs in lines)


def parse(html):
    if "</html" not in html.lower():
        raise SystemExit("the saved page is incomplete")
    tables = TABLE.findall(html)
    if len(tables) < 3:
        raise SystemExit("the kill ledger tables are missing")
    overview_cell = CELL.findall(ROW.findall(tables[0])[0])[0]
    overview = [rendered(runs) for runs in cell_lines(overview_cell)]
    sections = []
    for table in tables[1:3]:
        rows = ROW.findall(table)
        head = CELL.findall(rows[0])
        if not head:
            raise SystemExit("a table has no heading")
        if len(head) == 1:
            heading = cell_lines(head[0])
            title = plain(heading[0]) if heading else ""
            intro = [rendered(runs) for runs in heading[1:]]
        else:
            title = plain(cell_lines(head[0])[0]) if cell_lines(head[0]) else ""
            intro = [rendered(runs) for runs in cell_lines(head[1])]
        if title not in SECTIONS:
            raise SystemExit("the table %r is not a kill ledger table" % title)
        rows_out = []
        for row in rows[1:]:
            cells = CELL.findall(row)
            if len(cells) != 2:
                raise SystemExit("a %s row does not have an input and an outcome" % title)
            if title == KILLS:
                rows_out.append(kill_row(cells[0], cells[1]))
            else:
                rows_out.append(boss_row(cells[0], cells[1]))
        sections.append((title, intro, rows_out))
    if [s[0] for s in sections] != SECTIONS:
        raise SystemExit("the kill bonus and boss bonus tables are missing")
    if len(overview) < MIN_OVERVIEW:
        raise SystemExit("the overview is missing")
    for title, intro, rows_out in sections:
        if len(rows_out) < MIN_ROWS[title]:
            raise SystemExit("%s has %d rows, expected at least %d" % (title, len(rows_out), MIN_ROWS[title]))
    return overview, sections


def build_page(overview, sections):
    out = ["PAGE\t%s\t%s\t%s" % (PAGE_ID, PAGE_TITLE, PAGE_GROUP), "ENTRY\t%s\t" % OVERVIEW, BLOCK_LINE]
    out += ["LINE\tT\t" + line for line in overview]
    for title, intro, rows_out in sections:
        out += ["ENTRY\t%s\t" % title, BLOCK_LINE]
        out += ["LINE\tT\t" + line for line in intro]
        out.append("LINE\tC\t" + (KILLS_HEADINGS if title == KILLS else BOSS_HEADINGS))
        out += ["LINE\tR\t" + row for row in rows_out]
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
        overview, sections = parse(page_path.read_text(encoding="utf-8", errors="replace"))
        current = target.read_text(encoding="utf-8")
        merged = splice(current, build_page(overview, sections))
    except SystemExit as stop:
        print("Website kill ledger: %s" % stop)
        return 1
    tmp = target.with_suffix(target.suffix + ".new")
    tmp.write_text(merged, encoding="utf-8", newline="\n")
    tmp.replace(target)
    print("Website kill ledger: %d kill bonuses and boss bonuses" % sum(len(s[2]) for s in sections))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
