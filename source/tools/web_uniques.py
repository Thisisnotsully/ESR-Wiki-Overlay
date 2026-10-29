#!/usr/bin/env python3
"""Unique-item cards built from the four unique pages of the ESR website.

    python web_uniques.py <folder with the four pages> <wiki-data.txt> [web-unique-art.txt]

Reads unique_others.htm, unique_weapons.htm, unique_armors.htm and unique_mythicals.htm from the folder and
replaces the "uniques" page of wiki-data.txt (the mod-built one, when the extractor wrote it) with the page
built from the website. The page is written in the same card format as the mod-built "uniques" page. Art, tier and weapon
family are not on the website: they come from web-unique-art.txt (next to wiki-data.txt unless given), which
the extractor writes from the mod's own tables. Nothing else of the mod-built page is used.
Standard library only, like build_cube.py. Anything that goes wrong leaves wiki-data.txt untouched and exits 1.

Website wording is kept as it is. Only these changes are made, each a constant below:
  * "(10 to 20)" is written "10-20", the way the other cards show ranges (RANGE_DASH)
  * "Tinkering Points" lines are left out, as on the mod-built page (HIDDEN_LINES)
"""
import html
import re
import sys
from pathlib import Path

PAGE_ID = "uniques"
PAGE_TITLE = "Uniques"
PAGE_FILES = ("unique_others.htm", "unique_weapons.htm", "unique_armors.htm", "unique_mythicals.htm")
SITE = "https://easternsunresurrected.com/"

RANGE_DASH = True
HIDDEN_LINES = re.compile(r"^Tinkering Points\b", re.I)

# Colour of a <font color=...> on the website -> Diablo II colour code (blue is the page's own colour).
COLORS = {"4850B8": "3", "ORANGE": "8", "WHITE": "0", "RED": "1"}
DEFAULT_COLOR = "3"

# Table heading on the website -> card group. Groups are listed in page order with their entry titles.
GROUPS = (("Mythical", "Mythical Uniques"), ("Weapon", "Weapons"), ("Helm", "Helms"), ("Torso", "Torso"),
          ("Shield", "Shields"), ("Belt", "Belts"), ("Boots", "Boots"), ("Gloves", "Gloves"), ("Ring", "Rings"),
          ("Amulet", "Amulets"), ("Jewel", "Jewels"), ("Charm", "Charms"), ("Quiver", "Quivers"))
# Groups whose cards are filtered by the website's table headings (the others have a single heading).
FAMILY_GROUPS = ("Weapon", "Helm", "Torso", "Shield")
HEADING_GROUP = {
    "Large Charm": "Charm", "Grand Charm": "Charm", "Odd Charm": "Charm", "Small Charm": "Charm",
    "Jewel": "Jewel", "Bow Quiver": "Quiver", "Crossbow Quiver": "Quiver",
    "Helm": "Helm", "Pelt": "Helm", "Primal Helm": "Helm", "Circlet": "Helm", "Spirit Crown": "Helm",
    "Body Armor": "Torso", "Cloak": "Torso", "Robe": "Torso",
    "Shield": "Shield", "Auric Shields": "Shield", "Voodoo Heads": "Shield", "Grimoire": "Shield",
    "Gloves": "Gloves", "Boots": "Boots", "Belt": "Belt",
}


def heading_group(page, heading):
    if page == "unique_mythicals.htm":
        return "Mythical"
    if heading in HEADING_GROUP:
        return HEADING_GROUP[heading]
    if heading.endswith("Ring"):
        return "Ring"
    if heading.endswith("Amulet"):
        return "Amulet"
    if page == "unique_weapons.htm":
        return "Weapon"
    return None


def clean(text):
    return " ".join(html.unescape(text).replace("\xa0", " ").split())


def pieces(cell):
    return [clean(p) for p in re.split(r"<[^>]+>", cell) if clean(p)]


def dash_ranges(text):
    return re.sub(r"\((-?\d+(?:\.\d+)?) to (-?\d+(?:\.\d+)?)\)", r"\1-\2", text)


def property_lines(cell):
    """[(colour code, text)] from the Properties cell: <br> ends a line, <font color> sets the colour."""
    lines, stack, current = [], [], []

    def color():
        for value in reversed(stack):
            if value:
                return value
        return DEFAULT_COLOR

    def flush():
        text = clean("".join(current))
        current.clear()
        if text:
            lines.append((color_of_line[0], text))

    color_of_line = [DEFAULT_COLOR]
    for token in re.split(r"(<[^>]*>)", cell):
        if not token:
            continue
        if token.startswith("<"):
            name = re.match(r"</?\s*([a-zA-Z0-9]+)", token)
            name = name.group(1).lower() if name else ""
            if name == "br":
                flush()
            elif name == "font":
                if token.startswith("</"):
                    if stack:
                        stack.pop()
                else:
                    value = re.search(r'color\s*=\s*"?#?([0-9A-Za-z]+)', token, re.I)
                    stack.append(COLORS.get(value.group(1).upper(), DEFAULT_COLOR) if value else None)
            continue
        if clean(token) and not clean("".join(current)):
            color_of_line[0] = color()
        current.append(token)
    flush()
    out = []
    for code, text in lines:
        if RANGE_DASH:
            text = dash_ranges(text)
        if HIDDEN_LINES.match(text):
            continue
        out.append((code, text))
    return out


def parse_page(name, text):
    """Records of one website page, in page order."""
    records, heading = [], ""
    for row in re.findall(r"<tr[^>]*>(.*?)</tr>", text, re.S | re.I):
        cells = re.findall(r"<td[^>]*>(.*?)</td>", row, re.S | re.I)
        if len(cells) == 1 and re.search(r"colspan", row, re.I):
            heading = clean(re.sub(r"<[^>]+>", " ", cells[0]))
            continue
        if len(cells) != 4 or "Item Level" not in cells[1]:
            continue
        head = re.split(r'<font face="arial,helvetica" size="-1" color', cells[0], maxsplit=1, flags=re.I)[0]
        parts = pieces(head)
        gamble = None
        if "Gamble Item:" in parts:
            at = parts.index("Gamble Item:")
            gamble = parts[at + 1] if at + 1 < len(parts) else None
            parts = parts[:at]
        if not parts:
            continue
        title = parts[0]
        if title.endswith("(") and len(parts) >= 4:          # "Tathamet's Awakening ( Fire Variant )"
            title = "%s%s)" % (title, parts[1])
            base = parts[3]
        else:
            base = parts[1] if len(parts) > 1 else ""
        code = ""
        match = re.match(r"^(.*?)\s*\(([0-9A-Za-z]{3})\)$", base)
        if match:
            base, code = match.group(1).strip(), match.group(2)
        picture = re.search(r'<img[^>]+src="[^"]*?([^/"]+)\.png"', cells[0], re.I)
        levels = dict(re.findall(r"(Item Level|Required Level):\s*(\d+)", clean(re.sub(r"<[^>]+>", " ", cells[1]))))
        group = heading_group(name, heading)
        records.append({
            "page": name, "heading": heading, "group": group, "title": title, "base": base, "code": code,
            "coupon": "Ancient Coupon Unique" in cells[0], "gamble": gamble,
            "ilvl": levels.get("Item Level", ""), "level": levels.get("Required Level", ""),
            "lines": property_lines(cells[2]), "note": clean(re.sub(r"<[^>]+>", " ", cells[3])),
            "picture": picture.group(1) if picture else "",
        })
    return records


def parse_pages(folder):
    records = []
    for name in PAGE_FILES:
        path = Path(folder) / name
        if not path.exists():
            raise SystemExit("missing website page: %s" % path)
        text = path.read_text(encoding="utf-8", errors="replace")
        if not re.search(r"</html", text, re.I):
            raise SystemExit("incomplete website page: %s" % path)
        found = parse_page(name, text)
        if not found:
            raise SystemExit("no items found in %s" % path)
        records += found
    return records


def art_table(text):
    """(UNIQUE rows by name, BASE rows by code) of web-unique-art.txt: each is (IMG line or "", tier, family)."""
    unique, base = {}, {}
    for line in text.split("\n"):
        parts = line.split("\t")
        if len(parts) < 6 or parts[0] not in ("UNIQUE", "BASE"):
            continue
        img = ("IMG\t" + parts[2] + ("\t" + parts[3] if parts[3] else "")) if parts[2] else ""
        (unique if parts[0] == "UNIQUE" else base)[parts[1]] = (img, parts[4], parts[5])
    return unique, base


def norm(text):
    return re.sub(r"[^a-z0-9]", "", text.lower())


def build_page(records, art_text):
    """The page's lines in wiki-data.txt format, and a list of cards that got no art."""
    unique, base = art_table(art_text)
    unique_norm = {norm(k): v for k, v in unique.items()}
    entries = {group: [] for group, _ in GROUPS}
    no_art, unplaced = [], []
    for rec in records:
        if rec["group"] not in entries:
            unplaced.append(rec["title"])
            continue
        plain = re.sub(r"\s*\([^)]*Variant\)$", "", rec["title"])
        # The unique's own art first, else the art of its base item (by the code the website shows).
        own = unique.get(rec["title"]) or unique_norm.get(norm(rec["title"])) or unique.get(plain)
        by_code = base.get(rec["code"])
        img = (own[0] if own and own[0] else "") or (by_code[0] if by_code else "")
        if not img:
            no_art.append(rec["title"])
        mythical = rec["group"] == "Mythical"
        # Tier and weapon family belong to the base item.
        tier, family = ("", "") if mythical else (by_code[1], by_code[2]) if by_code else (own[1], own[2]) if own else ("", "")
        if rec["group"] in FAMILY_GROUPS:
            family = rec["heading"]                # the website's own categories (weapon types, helm and shield kinds)
        block = ["BLOCK\t1\t%s\t%s\t\t%s\t%s\t%s" % (
            rec["title"], rec["base"] if mythical else "", tier, family,
            0 if mythical else (rec["level"] or 0))]
        if rec["group"] == "Charm":
            pass                                  # charms cannot be gambled and are not coupon items
        elif rec["coupon"]:
            block.append("TAG\t(Coupon only)")
        elif rec["gamble"] and rec["gamble"].upper() != "N/A":
            base_name = re.sub(r"\s*\([0-9A-Za-z]{3}\)$", "", rec["gamble"])
            block.append("TAG\tGamble Item:\\n%s - ilvl: %s" % (base_name, rec["ilvl"]))
        if not mythical and rec["base"]:
            block.append("FIELD\tBase\t%s" % rec["base"])
        if rec["level"]:
            block.append("FIELD\tRequired Level\t%s" % rec["level"])
        if img:
            block.append(img)
        for code, text in rec["lines"]:
            block.append("LINE\tS\t\u00ffc%s%s" % (code, text))
        if rec["note"]:
            block.append("NOTE\t%s" % rec["note"])
        entries[rec["group"]].append(block)
    out = ["PAGE\t%s\t%s\tWiki" % (PAGE_ID, PAGE_TITLE)]
    for group, title in GROUPS:
        if not entries[group]:
            continue
        flag = "wide" if group == "Mythical" else "sortable"
        out.append("ENTRY\t%s\t%d items\t%s" % (title, len(entries[group]), flag))
        for block in entries[group]:
            out += block
    return out, no_art, unplaced


def splice(lines, page_lines):
    """lines with the page whose id is PAGE_ID replaced; a file without one gets it before 'set-items' (else at the end)."""
    starts = [i for i, l in enumerate(lines) if l.startswith("PAGE\t")]
    spans = {}
    for n, i in enumerate(starts):
        spans[lines[i].split("\t")[1]] = (i, starts[n + 1] if n + 1 < len(starts) else len(lines))
    if PAGE_ID in spans:
        a, b = spans[PAGE_ID]
        return lines[:a] + page_lines + lines[b:]
    at = spans["set-items"][0] if "set-items" in spans else len(lines)
    return lines[:at] + page_lines + lines[at:]


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__)
        return 2
    folder, target = Path(argv[1]), Path(argv[2])
    try:
        records = parse_pages(folder)
        text = target.read_text(encoding="utf-8")
        art_path = Path(argv[3]) if len(argv) > 3 else target.parent / "web-unique-art.txt"
        art_text = art_path.read_text(encoding="utf-8") if art_path.exists() else ""
        lines = text.split("\n")
        trailing = lines and lines[-1] == ""
        if trailing:
            lines = lines[:-1]
        page, no_art, unplaced = build_page(records, art_text)
        if unplaced:
            raise SystemExit("items under an unknown table heading: %s" % ", ".join(unplaced[:5]))
        if len(records) < 900:
            raise SystemExit("only %d items found; the website pages look broken" % len(records))
        merged = splice(lines, page)
    except SystemExit as stop:
        print("Website uniques: %s" % stop)
        return 1
    tmp = target.with_suffix(target.suffix + ".new")
    tmp.write_text("\n".join(merged) + ("\n" if trailing else ""), encoding="utf-8", newline="\n")
    tmp.replace(target)
    print("Website uniques: %d cards, %d without art" % (len(records), len(no_art)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
