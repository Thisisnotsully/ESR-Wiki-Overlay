#!/usr/bin/env python3
"""Endgame Maps page of wiki-data.txt, from the saved End Game Maps page of the ESR website (endgame_maps.htm).

    python web_endgame.py <endgame_maps.htm> <wiki-data.txt>

The website gives the overview paragraphs and, for each of the 18 map bosses, its name and zone, its description, its
abilities and its rewards. The bosses keep the pictures the existing page gives them (matched by title). Standard
library only. Anything that goes wrong leaves wiki-data.txt untouched and exits 1.
"""
import html as htmllib
import re
import sys
from pathlib import Path

from web_uniques import clean

PAGE_ID = "endgame-maps"
PAGE_TITLE = "Endgame Maps"
PAGE_GROUP = "Endgame"
OVERVIEW = "Overview"
BOSSES = 18
MIN_OVERVIEW = 5
BLOCK_LINE = "BLOCK\t0\t\t\t\t\t\t0"
HEADING = "H"            # the website's heading colour (#706850)
ABILITIES_LABEL = "Abilities"
# the website's colour names and hex values, as the overlay's colour letters
COLORS = {"red": "R", "green": "G", "purple": "P", "orange": "O", "#8b0000": "D", "#4850b8": "B", "#0000ff": "L",
          "#706850": "H", "#808080": "Y", "white": "W", "#f1e5ac": "K"}
TOKEN = re.compile(r"<br\s*/?>|<font\b([^>]*)>|</font\s*>|<i\s*>|</i\s*>|<[^>]+>|([^<]+)", re.I)
COLOR_ATTR = re.compile(r'color="([^"]*)"', re.I)
ROW = re.compile(r"<tr[^>]*>(.*?)</tr>", re.S | re.I)
CELL = re.compile(r"<td([^>]*)>(.*?)</td>", re.S | re.I)


def key_of(title):
    """Titles compare without case, spacing or punctuation."""
    return re.sub(r"[^a-z0-9]", "", title.lower())


def text_of(fragment):
    return clean(re.sub(r"<[^>]+>", "", fragment))


def runs_of(fragment):
    """The lines of a cell, each a list of runs (colour code or None, italic, text), split at <br>."""
    lines, current, stack, italic = [], [], [], 0
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
        elif token == "<i>":
            italic += 1
        elif token == "</i>":
            italic = max(0, italic - 1)
        elif match.group(2) is not None:
            colored = [c for c in stack if c is not None]
            code = COLORS.get(colored[-1]) if colored else None
            current.append((code, italic > 0, htmllib.unescape(match.group(2))))
    lines.append(current)
    return lines


def join(runs):
    return clean("".join(text for _, _, text in runs))


def boss_lines(cell):
    """The LINE lines of one boss's description and rewards cell."""
    out = []
    section = "other"
    abilities = False
    for runs in runs_of(cell):
        content = [r for r in runs if r[2].strip()]
        text = join(runs)
        if not content or not text:
            continue
        codes = {code for code, _, _ in content}
        if codes == {HEADING}:
            section = "rewards" if text.startswith("Rewards") else "other"
            out.append("LINE\tH\tÿcH" + text)
        elif codes == {None} and all(italic for _, italic, _ in content):
            out.append("LINE\tT\t" + text)
        elif content[0][0] not in (None, HEADING):
            first = content[0][0]
            start = next(i for i, run in enumerate(runs) if run[2].strip())
            i = start
            while i < len(runs) and runs[i][0] == first:
                i += 1
            name = join(runs[start:i])
            rest_raw = "".join(text for _, _, text in runs[i:])
            rest = clean(rest_raw)
            if section != "rewards" and not abilities:
                out.append("LINE\tH\tÿc" + HEADING + ABILITIES_LABEL)
                abilities = True
            if rest:
                lead = " " if rest_raw[:1].isspace() else ""
                out.append("LINE\tT\tÿc" + first + name + "ÿcN" + lead + rest)
            else:
                out.append("LINE\tT\tÿc" + first + name)
        else:
            out.append(("LINE\tT\tÿcN" if section == "rewards" else "LINE\tT\t") + text)
    return out


def parse(html):
    if "</html" not in html.lower():
        raise SystemExit("the saved page is incomplete")
    overview, bosses = [], []
    for row in ROW.findall(html):
        cells = CELL.findall(row)
        if len(cells) == 1 and "Warning:" in cells[0][1]:
            overview = [join(line) for line in runs_of(cells[0][1]) if join(line)]
        elif len(cells) == 2 and "map-bosses/" in cells[0][1]:
            title = text_of(cells[0][1])
            if not title:
                raise SystemExit("a map boss has no name")
            bosses.append((title, boss_lines(cells[1][1])))
    if len(overview) < MIN_OVERVIEW:
        raise SystemExit("the overview is missing")
    if len(bosses) != BOSSES:
        raise SystemExit("expected %d map bosses, found %d" % (BOSSES, len(bosses)))
    return overview, bosses


def existing_pictures(lines):
    """key of the title -> the IMG lines the existing page gives that entry (the first entry with any)."""
    entries = []
    for line in lines:
        parts = line.split("\t")
        if parts[0] == "ENTRY" and len(parts) > 1:
            entries.append((key_of(parts[1]), []))
        elif parts[0] == "IMG" and entries and len(parts) > 1:
            entries[-1][1].append("\t".join(parts[1:]))
    pictures = {}
    for key, images in entries:
        if images:
            pictures.setdefault(key, images)
    return pictures


def build_page(overview, bosses, pictures, report):
    out = ["PAGE\t%s\t%s\t%s" % (PAGE_ID, PAGE_TITLE, PAGE_GROUP), "ENTRY\t%s\t" % OVERVIEW, BLOCK_LINE]
    out += ["LINE\tT\t" + text for text in overview]
    for title, lines in bosses:
        images = pictures.get(key_of(title), [])
        if not images:
            report.append("no picture: %s" % title)
        out += ["ENTRY\t%s\t" % title, BLOCK_LINE]
        out += ["IMG\t" + image for image in images]
        out += lines
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


def page_lines(lines):
    starts = [i for i, line in enumerate(lines) if line.startswith("PAGE\t")]
    for n, i in enumerate(starts):
        if lines[i].split("\t")[1] == PAGE_ID:
            end = starts[n + 1] if n + 1 < len(starts) else len(lines)
            return lines[i:end]
    return []


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    page_path, target = Path(argv[1]), Path(argv[2])
    report = []
    try:
        overview, bosses = parse(page_path.read_text(encoding="utf-8", errors="replace"))
        current = target.read_text(encoding="utf-8")
        page = build_page(overview, bosses, existing_pictures(page_lines(current.split("\n"))), report)
        merged = splice(current, page)
    except SystemExit as stop:
        print("Website endgame maps: %s" % stop)
        return 1
    tmp = target.with_suffix(target.suffix + ".new")
    tmp.write_text(merged, encoding="utf-8", newline="\n")
    tmp.replace(target)
    print("Website endgame maps: %d bosses, %d without a picture" % (len(bosses), len(report)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
