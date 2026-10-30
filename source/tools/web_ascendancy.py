#!/usr/bin/env python3
"""Ascendancy page of wiki-data.txt, rebuilt from the saved page of the ESR website.

    python web_ascendancy.py <ascendancies.htm> <wiki-data.txt>

The page keeps the layout it has: an "Ascendancies" entry (the introduction, then the Orb of Creation table, which the
website does not have and which is kept as it is), a "Higher Tier Challenges" entry, and one entry per ascendancy in
alphabetical order with its picture, the gems that unlock it, and its five tiers (tier headers gold, stats blue,
footnote asterisks and footnotes red). Everything the website says comes from the saved page: the introduction, the
challenge texts, the gem recipes, the tier stats and the footnotes. Standard library only. Anything that goes wrong
leaves wiki-data.txt untouched and exits 1.
"""
import re
import sys
from pathlib import Path

from web_uniques import clean

PAGE_ID = "ascendancy"
ORB_TITLE = "Orb of Creation / Material Locations"
KEY_PREFIX = "d2rloader_esr-wiki_wiki_"
RED, BLUE, GOLD, WHITE = "ÿc1", "ÿc3", "ÿc4", "ÿc0"
SPACER = "​"
SEPARATOR = "\x1f"  # between table cells
BREAK = "\x1e"  # between the lines of one cell
SENTENCE_ENDS = re.compile(r"special encounters\.|endgame content\.|any order\.|etc\.\)\.")
EXPECTED_CLASSES = 15


def plain(fragment):
    """Text of an HTML fragment: line breaks become spaces, tags are dropped, entities are decoded, spaces collapsed,
    and a space before . , ; : or ) goes."""
    text = clean(re.sub(r"<[^>]+>", "", re.sub(r"<br\s*/?>", " ", fragment, flags=re.I)))
    return re.sub(r" ([.,;:)])", r"\1", text)


def lines_of(fragment):
    """The lines of an HTML fragment, split at <br>, tags removed, empty ones dropped."""
    out = []
    for part in re.split(r"<br\s*/?>", fragment, flags=re.I):
        text = plain(part)
        if text:
            out.append(text)
    return out


def red_stars(text, base):
    return re.sub(r"\*+", lambda m: RED + m.group(0) + base, text)


def parse(html):
    if "</html" not in html.lower():
        raise SystemExit("the saved page is incomplete")
    # the introduction
    marker = html.find("are powerful specializations")
    cell_open = html.rfind("<td", 0, marker) if marker >= 0 else -1
    intro_start = html.find(">", cell_open) + 1 if cell_open >= 0 else -1
    intro_end = html.find("</td>", marker) if marker >= 0 else -1
    if intro_start <= 0 or intro_end < 0:
        raise SystemExit("the introduction is missing")
    intro = plain(html[intro_start:intro_end])

    # tier 1: the gems that unlock each ascendancy
    recipes = {}
    tier1 = html.find("Tier 1 Challenge")
    tier2 = html.find("Tier 2 Challenge")
    if tier1 < 0 or tier2 < tier1:
        raise SystemExit("the Tier 1 challenge is missing")
    for row in re.findall(r"<tr>(.*?)</tr>", html[tier1:tier2], flags=re.S):
        cells = re.findall(r"<td[^>]*>(.*?)</td>", row, flags=re.S)
        if len(cells) != 2:
            continue
        match = re.match(r"(.+?) Ascendancy Stone$", plain(cells[1]))
        if match:
            # what goes in: each line, and whether the website writes it in orange (the orb); what comes out
            inputs = []
            for piece in re.split(r"<br\s*/?>", cells[0], flags=re.I):
                text = plain(piece)
                if text:
                    inputs.append((text, 'color="orange"' in piece))
            recipes[match.group(1)] = {"input": inputs, "output": plain(cells[1])}
    if len(recipes) != EXPECTED_CLASSES:
        raise SystemExit("expected %d ascendancy recipes, found %d" % (EXPECTED_CLASSES, len(recipes)))

    # tiers 2 to 5
    challenges = []
    for number in range(2, 6):
        start = html.find("Tier %d Challenge" % number)
        if start < 0:
            raise SystemExit("the Tier %d challenge is missing" % number)
        cell = re.search(r"<td[^>]*>(.*?)</td>", html[start:], flags=re.S)
        challenges.append(("Tier %d:" % number, plain(cell.group(1))))

    # the ascendancies
    section = html.find('name="ascendancies"')
    if section < 0:
        raise SystemExit("the ascendancy list is missing")
    parts = re.split(r'<div style="display: flex; justify-content: space-around; width: 1025px">', html[section:])[1:]
    classes = {}
    for part in parts:
        name = re.search(r'<font color="orange" size="4">(.*?) Ascendancy</font>', part, flags=re.S)
        image = re.search(r'<img src="[^"]*?([A-Za-z0-9_]+)\.png"', part)
        body = re.search(r'<font face="arial,helvetica" size="-1" style="width: 100%">(.*?)</font>\s*</div>', part, flags=re.S)
        if not (name and image and body):
            raise SystemExit("an ascendancy block could not be read")
        title = plain(name.group(1))
        tiers = {}
        pieces = re.split(r'<font color="orange">(Tier \d\*?)</font>', body.group(1))
        for k in range(1, len(pieces), 2):
            header = plain(pieces[k])
            stats = lines_of(pieces[k + 1])
            tiers[int(header[5])] = (header, stats)
        if sorted(tiers) != [1, 2, 3, 4, 5]:
            raise SystemExit("%s does not have five tiers" % title)
        notes = [plain(n) for n in re.findall(r'<font size="-2"[^>]*>(.*?)</font>', part, flags=re.S) if plain(n)]
        if title in classes or title not in recipes:
            raise SystemExit("unexpected ascendancy %s" % title)
        # the website puts the tiers on one side and the name with the picture on the other, alternating
        picture_first = part.find('<font color="orange" size="4">') < part.find('style="width: 100%"')
        classes[title] = {"image": image.group(1), "tiers": tiers, "notes": notes, "recipe": recipes[title],
                          "picture_first": picture_first}
    if set(classes) != set(recipes):
        raise SystemExit("the ascendancy list and the Tier 1 recipes disagree")
    return intro, challenges, classes


def intro_lines(text):
    out = []
    pos = 0
    for match in SENTENCE_ENDS.finditer(text):
        out.append(text[pos:match.end()].strip())
        out.append(SPACER)
        pos = match.end()
    if text[pos:].strip():
        out.append(text[pos:].strip())
    return out


def text_line(text):
    return "LINE\tT\t" + text


def stat_line(text):
    """A line of the tier columns: the overlay draws these beside the picture, centred."""
    return "LINE\tS\t" + text


def class_entry(title, info):
    recipe = info["recipe"]
    inputs = BREAK.join((GOLD if orange else WHITE) + text + WHITE for text, orange in recipe["input"])
    out = ["ENTRY\t%s\t\t%s" % (title, "splitflip" if info["picture_first"] else "split"), "BLOCK\t0\t\t\t\t\t\t0",
           "IMG\t%s%s" % (KEY_PREFIX, info["image"])]
    for number in range(1, 6):
        header, stats = info["tiers"][number]
        if number > 1:
            out.append(stat_line(SPACER))
        stars = re.search(r"\*+$", header)
        label = "Tier %d" % number
        out.append(stat_line("%s%s%s%s:%s" % (GOLD, label, (RED + stars.group(0) + GOLD) if stars else "", "", WHITE)))
        for stat in stats:
            out.append(stat_line(BLUE + red_stars(stat, BLUE) + WHITE))
    if info["notes"]:
        out.append(text_line(SPACER))
        for note in info["notes"]:
            out.append(text_line(RED + note))
    # what goes in and what comes out, at the bottom
    out.append(text_line(SPACER))
    out.append("LINE\tC\tInput" + SEPARATOR + "Output")
    out.append("LINE\tR\t" + inputs + SEPARATOR + GOLD + recipe["output"] + WHITE)
    return out


def page_lines(old, intro, challenges, classes):
    """old: the lines of the existing page (PAGE line first)."""
    # the Orb of Creation table and what follows it are kept from the existing page
    orb_at = next((i for i, l in enumerate(old) if l == "LINE\tT\t" + ORB_TITLE), None)
    if orb_at is None:
        raise SystemExit("the existing page has no Orb of Creation table to keep")
    next_entry = next(i for i in range(orb_at, len(old)) if old[i].startswith("ENTRY\t"))
    kept = old[orb_at:next_entry]
    out = [old[0], "ENTRY\tAscendancies\t", "BLOCK\t0\t\t\t\t\t\t0"]
    out += [text_line(t) for t in intro_lines(intro)]
    out += kept
    out += ["ENTRY\tHigher Tier Challenges\t", "BLOCK\t0\t\t\t\t\t\t0"]
    for header, text in challenges:
        out += [text_line(header), text_line(text), text_line(SPACER)]
    for title in sorted(classes, key=str.lower):
        out += class_entry(title, classes[title])
    return out


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    html_path, target = Path(argv[1]), Path(argv[2])
    try:
        intro, challenges, classes = parse(html_path.read_text(encoding="utf-8", errors="replace"))
        lines = target.read_text(encoding="utf-8").split("\n")
        trailing = bool(lines) and lines[-1] == ""
        if trailing:
            lines = lines[:-1]
        starts = [i for i, l in enumerate(lines) if l.startswith("PAGE\t")]
        found = next((n for n, i in enumerate(starts) if lines[i].split("\t")[1] == PAGE_ID), None)
        if found is None:
            raise SystemExit("wiki-data.txt has no %s page" % PAGE_ID)
        begin = starts[found]
        end = starts[found + 1] if found + 1 < len(starts) else len(lines)
        merged = lines[:begin] + page_lines(lines[begin:end], intro, challenges, classes) + lines[end:]
    except SystemExit as stop:
        print("Website ascendancies: %s" % stop)
        return 1
    tmp = target.with_suffix(target.suffix + ".new")
    tmp.write_text("\n".join(merged) + ("\n" if trailing else ""), encoding="utf-8", newline="\n")
    tmp.replace(target)
    print("Website ascendancies: %d ascendancies" % len(classes))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
