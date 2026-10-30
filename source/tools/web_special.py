#!/usr/bin/env python3
"""Special Craft page of wiki-data.txt, from the Special Crafting Outcomes page of the ESR website
(special_crafting_outcomes.htm).

    python web_special.py <special_crafting_outcomes.htm> <wiki-data.txt>

The website gives four sections (Corruption, Anointment, Sanctification and Standard of Heroes Outcomes), each with an
intro paragraph and a table of inputs and their possible outcomes. Each input (item type or item name) becomes a block
under its section, and each outcome row becomes one "Same Item: ..." line. Standard library only (web_uniques.clean).
Anything that goes wrong leaves wiki-data.txt untouched and exits 1.
"""
import re
import sys
from pathlib import Path

from web_uniques import clean

PAGE_ID = "corruption"
PAGE_TITLE = "Special Craft"
PAGE_GROUP = "Endgame"
SECTIONS = {  # the website's heading -> the number of outcome rows the section must have at least
    "Corruption Outcomes": 100,
    "Anointment Outcomes": 10,
    "Sanctification Outcomes": 50,
    "Standard of Heroes Outcomes": 6,
}
OUTCOME_COLOR = "ÿc3"  # the overlay's colour marker and the colour the outcome lines have always had
SAME_ITEM = "Same Item"
TABLE = re.compile(r"<table[^>]*>(.*?)</table>", re.S | re.I)
ROW = re.compile(r"<tr[^>]*>(.*?)</tr>", re.S | re.I)
CELL = re.compile(r"<td[^>]*>(.*?)</td>", re.S | re.I)
BREAK = re.compile(r"<br\s*/?>", re.I)
SLASH_NAME = re.compile(r"^(?:[A-Za-z]+/)+[A-Za-z]+ (.+)$")


def lines_of(fragment):
    """The <br>-separated lines of a cell, cleaned, without empty ones."""
    out = []
    for part in BREAK.split(fragment):
        text = clean(re.sub(r"<[^>]+>", "", part))
        if text:
            out.append(text)
    return out


def group_name(first):
    """The block name of an input: "Unique/Set/Crafted/Rare/Magic/Normal Weapon" is Weapon, "Unique Charm" is Charm,
    "Hellfire Torch" and "Any Jewel" are themselves."""
    match = SLASH_NAME.match(first)
    if match:
        return match.group(1)
    if first.startswith("Unique "):
        return first[len("Unique "):]
    return first


def outcome_line(cell):
    parts = lines_of(cell)
    if parts and parts[0] == SAME_ITEM:
        parts = parts[1:]
    if not parts:
        return None
    return "LINE\tR\t" + OUTCOME_COLOR + SAME_ITEM + ": " + "; ".join(parts)


def parse(html):
    if "</html" not in html.lower():
        raise SystemExit("the saved page is incomplete")
    sections = []
    intro = []
    current = None
    for table in TABLE.findall(html):
        rows = ROW.findall(table)
        if not rows:
            continue
        cells = CELL.findall(rows[0])
        if len(rows) == 1 and len(cells) == 1:
            text = " ".join(lines_of(cells[0]))
            if text in SECTIONS:
                current = {"title": text, "intro": intro, "groups": {}, "rows": 0}
                sections.append(current)
                intro = []
            else:
                intro = lines_of(cells[0])
        elif current is not None and "Input(s)" in " ".join(lines_of(rows[0])):
            for row in rows[1:]:
                row_cells = CELL.findall(row)
                if len(row_cells) != 2:
                    raise SystemExit("an outcome row does not have an input and an outcome")
                inputs = lines_of(row_cells[0])
                if not inputs:
                    raise SystemExit("an outcome row has no input")
                line = outcome_line(row_cells[1])
                if line is None:
                    raise SystemExit("an outcome row has no outcome")
                name = group_name(inputs[0])
                current["groups"].setdefault(name, []).append(line)
                current["rows"] += 1
    if [s["title"] for s in sections] != list(SECTIONS):
        raise SystemExit("the four outcome sections are missing")
    for section in sections:
        if not section["intro"]:
            raise SystemExit("%s has no intro" % section["title"])
        if section["rows"] < SECTIONS[section["title"]]:
            raise SystemExit("%s has %d outcomes, expected at least %d" % (section["title"], section["rows"],
                                                                         SECTIONS[section["title"]]))
    return sections


def build_page(sections):
    out = ["PAGE\t%s\t%s\t%s" % (PAGE_ID, PAGE_TITLE, PAGE_GROUP)]
    for section in sections:
        out += ["ENTRY\t%s\t" % section["title"], "BLOCK\t0\t\t\t\t\t\t0"]
        out += ["LINE\tT\t" + line for line in section["intro"]]
        many = len(section["groups"]) > 1
        for name, rows in section["groups"].items():
            family = name if many else ""
            out += ["BLOCK\t0\t\t\t%s\t\t%s\t0" % (name, family), "LINE\tC\tPossible Outcome(s)"]
            out += rows
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
        sections = parse(page_path.read_text(encoding="utf-8", errors="replace"))
        current = target.read_text(encoding="utf-8")
        merged = splice(current, build_page(sections))
    except SystemExit as stop:
        print("Website special crafting: %s" % stop)
        return 1
    tmp = target.with_suffix(target.suffix + ".new")
    tmp.write_text(merged, encoding="utf-8", newline="\n")
    tmp.replace(target)
    rows = sum(s["rows"] for s in sections)
    print("Website special crafting: %d sections, %d outcomes" % (len(sections), rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
