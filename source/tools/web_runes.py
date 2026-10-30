#!/usr/bin/env python3
"""Rune cards built from the gems page of the ESR website.

    python web_runes.py <gems.htm> <wiki-data.txt> [cube-wiki-data.txt]

Reads the runes from gems.htm (https://easternsunresurrected.com/gems.htm) and replaces the "runes" page of
wiki-data.txt (the mod-built one, when the extractor wrote it) with a page built only from the website. Nothing of
the mod-built page is used. The gems on the same website page are not read.
Standard library only, like build_cube.py and web_uniques.py. Anything that goes wrong leaves wiki-data.txt
untouched and exits 1.

How the page is read (the C++ twin is src/web_runes_live.cpp; change both together):
  * each rune is a table row with one wide cell "<name> Rune / Req Lvl: N", then a row of the three item-kind
    labels, then a row of three property cells (one per label), lines split by <br>
  * runes are grouped by the colour of the name: none = LoD Runes, a #rrggbb colour = Elemental Runes, any
    named colour = Kanji Runes. The groups are shown in that page order: Kanji, LoD, Elemental
  * property cells that are the same are shown once: all three = "All Items", else the website's own labels
    joined with " / ", e.g. "Helms / Boots / Armor / Shields / Belts"
  * a rune name used twice (Ko is a Kanji and a LoD rune) gives its LoD card the art "<name>_lod_rune"
  * "(10 to 20)" is written "10-20", as on the other cards
  * the "How to Acquire Elemental Runes" note comes last. Its text (HOW_TO_ACQUIRE) is fixed; its table lists the
    recipes of the cube recipes page (cube-wiki-data.txt, next to wiki-data.txt unless given, which the cube tools write
    from the website) that make one of the elemental runes of the gems page
"""
import re
import sys
from pathlib import Path

from web_uniques import clean, dash_ranges, property_lines

PAGE_ID = "runes"
PAGE_TITLE = "Runes"
PAGE_FLAG = "compact"
GROUPS = (("Kanji", "Kanji Runes"), ("LoD", "LoD Runes"), ("Elemental", "Elemental Runes"))
MIN_RUNES = 80
ALL_ITEMS = "All Items"
# The note after the runes. Its text is fixed (it is not on the website page); the table under it is built from the
# elemental rune recipes of the cube recipes page (see elemental_recipes). U+001F separates table cells.
HOW_TO_ACQUIRE = (
    "ENTRY\tHow to Acquire Elemental Runes\t",
    "BLOCK\t0\t\t\t\t\t\t0",
    "LINE\tT\tElemental Runes cannot be found as monster drops - they must be crafted using the Horadric Cube.",
    "LINE\tT\t1. Cube a Wo Rune together with a Full Rejuvenation Potion to create a Null Rune.",
    "LINE\tT\t2. Hell difficulty Treasure Goblins drop Crystal Shards. Cube 5 matching Crystal Shards to upgrade them into a Flawed Crystal, then cube 5 matching Flawed Crystals to upgrade into a named Catalyst Stone.",
    "LINE\tT\t3. Cube the Catalyst Stone together with a Null Rune to create the matching Elemental Rune.",
    "LINE\tT\tHeaven Rune and God Rune instead require four different Catalyst Stones together with a Null Rune.",
    "LINE\tC\tIngredients\u001fResult",
)


def rune_group(color):
    if not color:
        return "LoD"
    return "Elemental" if color.startswith("#") else "Kanji"


def parse_runes(text):
    """[{name, level, group, labels, cells}] for each rune of the page, in page order."""
    runes, current = [], None
    labels = None
    for row in re.findall(r"<tr[^>]*>(.*?)</tr>", text, re.S | re.I):
        cells = re.findall(r"<td[^>]*>(.*?)</td>", row, re.S | re.I)
        if len(cells) == 1 and re.search(r"Req\s*Lvl", cells[0], re.I):
            plain = clean(re.sub(r"<[^>]+>", " ", cells[0]))
            name = re.split(r"\s*Req\s*Lvl", plain, maxsplit=1, flags=re.I)[0].strip()
            level = re.search(r"Req\s*Lvl:?\s*(\d+)", plain, re.I)
            color = re.search(r"<b>\s*<font[^>]*color\s*=\s*\"?([#0-9A-Za-z]+)", cells[0], re.I)
            current = None
            if name.endswith(" Rune") and level:
                current = {"name": name, "level": level.group(1), "color": color.group(1) if color else ""}
                labels = None
            continue
        if current is None or len(cells) != 3:
            continue
        if re.search(r"height\s*=", row, re.I):
            current["cells"] = [[(code, dash_ranges(t)) for code, t in property_lines(c)] for c in cells]
            current["labels"] = labels or ["Weapons / Gloves", "Helms / Boots", "Armor / Shields / Belts"]
            current["group"] = rune_group(current.pop("color"))
            runes.append(current)
            current = None
        else:
            labels = [clean(re.sub(r"<[^>]+>", " ", c)) for c in cells]
    return runes


def image_key(rune, taken):
    base = "items_misc_rune_" + re.sub(r"[^a-z0-9]+", "_", rune["name"].lower()).strip("_")
    if rune["group"] == "LoD" and base in taken:
        base = base[: -len("_rune")] + "_lod_rune"
    return base


def sections(rune):
    """[(label, [(code, text)])]: equal neighbouring cells are shown once under their joined labels."""
    cells, labels = rune["cells"], rune["labels"]
    out = []
    for cell, label in zip(cells, labels):
        if not cell:
            continue
        if out and out[-1][1] == cell:
            out[-1] = (out[-1][0] + " / " + label, cell)
        else:
            out.append((label, cell))
    if len(out) == 1 and all(c == cells[0] for c in cells):
        out = [(ALL_ITEMS, out[0][1])]
    return out


def elemental_recipes(cube_text, names):
    """Table rows, as on the Cube Recipes window, for the cube recipes whose result is one of `names`, in the cube page's order."""
    rows = []
    for line in cube_text.split("\n"):
        parts = line.split("\t", 2)
        if len(parts) < 3 or parts[0] != "LINE" or parts[1] != "R":
            continue
        cells = parts[2].split("\x1f")
        if len(cells) != 2:
            continue
        result = re.sub("^\u00ffc.", "", cells[1])
        if result not in names:
            continue
        # The cube page's own cells: one ingredient per line, the result in blue.
        row = "LINE\tR\t%s\x1f%s" % (cells[0], cells[1])
        if row not in rows:
            rows.append(row)
    return rows


def build_page(runes, cube_text):
    taken, entries = set(), {key: [] for key, _ in GROUPS}
    for rune in runes:
        key = image_key(rune, taken)
        taken.add(key)
        block = ["BLOCK\t1\t%s\t\t\t\t\t0" % rune["name"], "FIELD\tRequired Level\t%s" % rune["level"], "IMG\t%s" % key]
        for label, lines in sections(rune):
            block.append("LINE\tS\tÿc0%s:" % label)
            for code, text in lines:
                block.append("LINE\tS\tÿc%s%s" % (code, text))
        entries[rune["group"]].append(block)
    out = ["PAGE\t%s\t%s\tWiki\t%s" % (PAGE_ID, PAGE_TITLE, PAGE_FLAG)]
    for key, title in GROUPS:
        if not entries[key]:
            continue
        out.append("ENTRY\t%s\t" % title)
        for block in entries[key]:
            out += block
    recipes = elemental_recipes(cube_text, {r["name"] for r in runes if r["group"] == "Elemental"})
    if not recipes:
        raise SystemExit("the cube recipes have no elemental rune recipes")
    out += list(HOW_TO_ACQUIRE) + recipes
    return out


def splice(lines, page_lines):
    """lines with the page whose id is PAGE_ID replaced; a file without one gets it after 'ascendancy' (else first)."""
    starts = [i for i, l in enumerate(lines) if l.startswith("PAGE\t")]
    spans = {}
    for n, i in enumerate(starts):
        spans[lines[i].split("\t")[1]] = (i, starts[n + 1] if n + 1 < len(starts) else len(lines))
    if PAGE_ID in spans:
        a, b = spans[PAGE_ID]
        return lines[:a] + page_lines + lines[b:]
    at = spans["ascendancy"][1] if "ascendancy" in spans else (starts[0] if starts else len(lines))
    return lines[:at] + page_lines + lines[at:]


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__)
        return 2
    source, target = Path(argv[1]), Path(argv[2])
    try:
        page = source.read_text(encoding="utf-8", errors="replace")
        if "</html" not in page.lower():
            raise SystemExit("%s is incomplete" % source.name)
        runes = parse_runes(page)
        if len(runes) < MIN_RUNES:
            raise SystemExit("only %d runes found; the website page looks broken" % len(runes))
        if {r["group"] for r in runes} != {k for k, _ in GROUPS}:
            raise SystemExit("the website runes do not fall into the three groups")
        cube_path = Path(argv[3]) if len(argv) > 3 else target.parent / "cube-wiki-data.txt"
        cube_text = cube_path.read_text(encoding="utf-8") if cube_path.exists() else ""
        text = target.read_text(encoding="utf-8")
        lines = text.split("\n")
        trailing = lines and lines[-1] == ""
        if trailing:
            lines = lines[:-1]
        merged = splice(lines, build_page(runes, cube_text))
    except SystemExit as stop:
        print("Website runes: %s" % stop)
        return 1
    tmp = target.with_suffix(target.suffix + ".new")
    tmp.write_text("\n".join(merged) + ("\n" if trailing else ""), encoding="utf-8", newline="\n")
    tmp.replace(target)
    print("Website runes: %d runes" % len(runes))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
