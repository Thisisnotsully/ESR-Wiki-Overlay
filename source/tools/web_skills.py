#!/usr/bin/env python3
"""Mercenary/O-Skill page of wiki-data.txt, from the Skill Information page of the ESR website (skill_information.htm).

    python web_skills.py <skill_information.htm> <wiki-data.txt>

The website gives the intro, a table of mercenaries (hireling, subtype, skills) and a table of O-skills (skill,
description). Each table becomes one entry with its column headings and one row per line. Standard library only
(web_uniques.clean). Anything that goes wrong leaves wiki-data.txt untouched and exits 1.
"""
import re
import sys
from pathlib import Path

from web_uniques import clean

PAGE_ID = "merc-oskill"
PAGE_TITLE = "Mercenary/O-Skill"
PAGE_GROUP = "Endgame"
SECTIONS = {  # the website's heading -> (columns in each row, the rows it must have at least)
    "Mercenaries": (3, 10),
    "Oskills": (2, 20),
}
SEPARATOR = "\x1f"  # the unit separator the overlay splits table cells on
BLOCK_LINE = "BLOCK\t0\t\t\t\t\t\t0"
TABLE = re.compile(r"<table[^>]*>(.*?)</table>", re.S | re.I)
ROW = re.compile(r"<tr[^>]*>(.*?)</tr>", re.S | re.I)
CELL = re.compile(r"<td[^>]*>(.*?)</td>", re.S | re.I)
HEADING = re.compile(r"<th[^>]*>(.*?)</th>", re.S | re.I)
BREAK = re.compile(r"<br\s*/?>", re.I)


def lines_of(fragment):
    """The <br>-separated lines of a cell, cleaned, without empty ones."""
    out = []
    for part in BREAK.split(fragment):
        text = clean(re.sub(r"<[^>]+>", "", part))
        if text:
            out.append(text)
    return out


def cell_text(fragment):
    return " ".join(lines_of(fragment))


def parse(html):
    if "</html" not in html.lower():
        raise SystemExit("the saved page is incomplete")
    intro = []
    sections = []  # [title, headings, rows]
    for table in TABLE.findall(html):
        rows = ROW.findall(table)
        if not rows:
            continue
        cells = CELL.findall(rows[0])
        if len(cells) != 1:
            continue
        text = cell_text(cells[0])
        if text not in SECTIONS:
            if len(rows) == 1:
                intro = lines_of(cells[0])
            continue
        # A section table: its title row, then the column headings, then one row per entry.
        section = [text, [], []]
        sections.append(section)
        columns = SECTIONS[text][0]
        for row in rows[1:]:
            headings = HEADING.findall(row)
            if headings:
                section[1] = [cell_text(h) for h in headings]
                continue
            row_cells = CELL.findall(row)
            if len(row_cells) != columns:
                raise SystemExit("a %s row does not have %d columns" % (text, columns))
            values = [cell_text(c) for c in row_cells]
            if not values[0]:
                raise SystemExit("a %s row has no name" % text)
            section[2].append(SEPARATOR.join(values))
    if [s[0] for s in sections] != list(SECTIONS):
        raise SystemExit("the mercenary and oskill tables are missing")
    if not intro:
        raise SystemExit("the intro is missing")
    for title, headings, rows in sections:
        if len(headings) != SECTIONS[title][0]:
            raise SystemExit("%s has no column headings" % title)
        if len(rows) < SECTIONS[title][1]:
            raise SystemExit("%s has %d rows, expected at least %d" % (title, len(rows), SECTIONS[title][1]))
    return intro, sections


def build_page(intro, sections):
    out = ["PAGE\t%s\t%s\t%s" % (PAGE_ID, PAGE_TITLE, PAGE_GROUP)]
    for index, (title, headings, rows) in enumerate(sections):
        out += ["ENTRY\t%s\t" % title, BLOCK_LINE]
        if index == 0:
            out += ["LINE\tT\t" + line for line in intro]
        out.append("LINE\tC\t" + SEPARATOR.join(headings))
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
        intro, sections = parse(page_path.read_text(encoding="utf-8", errors="replace"))
        current = target.read_text(encoding="utf-8")
        merged = splice(current, build_page(intro, sections))
    except SystemExit as stop:
        print("Website skill information: %s" % stop)
        return 1
    tmp = target.with_suffix(target.suffix + ".new")
    tmp.write_text(merged, encoding="utf-8", newline="\n")
    tmp.replace(target)
    print("Website skill information: %d tables, %d rows" % (len(sections), sum(len(s[2]) for s in sections)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
