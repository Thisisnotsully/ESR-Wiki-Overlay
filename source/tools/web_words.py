#!/usr/bin/env python3
"""Runeword and gemword cards built from runewords.htm and gemwords.htm of the ESR website.

    python web_words.py runewords <runewords.htm> <gems.htm> <wiki-data.txt> [web-word-art.txt]
    python web_words.py gemwords  <gemwords.htm>             <wiki-data.txt> [web-word-art.txt]

Replaces the "runewords" or "gemwords" page of wiki-data.txt (the mod-built one, when the extractor wrote it) with a
page built only from the website. Nothing of the mod-built page is used, except the pictures: the website has none,
so web-word-art.txt (next to wiki-data.txt unless given; the extractor writes it from the mod's own art) says which
picture goes with which runeword, and which icon goes with a rune or gem. Standard library only, like web_uniques.py.
Anything that goes wrong leaves wiki-data.txt untouched and exits 1. The C++ twin is src/web_words_live.cpp; change
both together (tests/web_words_live_test.cpp checks that they agree).

How the pages are read (two layouts):
  * since November 2026 each recipe is a table row (class recipeRow) of five cells: the name with its sockets and
    required level ("Req Lvl: 2"), the runes or gems, the allowed items, the word's own bonuses and its socketable
    bonuses. The socketable bonuses are plain lines, or groups (div class="rws-group") of a label (div class="rws-label",
    the item kinds they apply to) and its lines. Every card shows the bonuses under the page's own "Bonuses" heading,
    then the socketable bonuses under its "Socketable Bonuses" heading, each group's label as a LINE L (a small dim
    sub-heading) over its lines. The required level is the website's; without one it is worked out as below
  * the older layout (still read, for saved pages): each recipe is a table row (class recipeRow) of six cells: the name with its sockets, the runes or gems, the allowed
    items, and three columns of stat lines (Weapons / Gloves, Helms / Boots, Armor / Shields / Belts)
  * a blank line in a stat column starts a new group of lines. Group by group, columns that read the same are shown
    once, otherwise each column's group is shown under the website's own label (equal neighbours share a label)
  * runewords: the last group of a column is the rune bonuses, shown under "Rune Bonuses:". The page has no required
    level, so it is the highest level of the runes and gems used, as the gems page gives it. A rune name written
    without a colour is a LoD rune (Ko exists as a Kanji and a LoD rune)
  * a recipe is listed under every item category its allowed items belong to (gemwords: under the first)
  * gemwords are filtered by the gemword's name; their picture is the first gem
"""
import re
import sys
from pathlib import Path

from web_uniques import GAP, clean, property_lines

OUTER_COLOR = "#908858"
RUNEWORD_MIN = 300
GEMWORD_MIN = 500
LABELS = ("Weapons / Gloves", "Helms / Boots", "Armor / Shields / Belts")
NEW_LABELS = ("Bonuses", "Socketable Bonuses")  # the five-cell layout's headings, when the page has none
GROUP_MARK = 'class="rws-group"'
LABEL_MARK = 'class="rws-label"'

CATEGORIES = ("Weapons", "Torso", "Boots", "Gloves", "Helms", "Shields", "Belts", "Charms", "Other")
ARMOR_TOKENS = {"Body Armor": ("Torso",), "Helm": ("Helms",), "Pelt": ("Helms",), "Any Shield": ("Shields",),
                "Paladin Item": ("Shields",), "Gloves": ("Gloves",), "Boots": ("Boots",), "Belt": ("Belts",),
                "Charm": ("Charms",), "Any Armor": ("Torso", "Boots", "Gloves", "Helms", "Shields", "Belts")}
WEAPON_TOKENS = {"Weapon", "Missile", "Melee Weapon", "Missile Weapon", "Two-Handed Melee Weapon", "Hand to Hand",
                 "Assassin 2H Katana", "Axe", "Blunt", "Club", "Crossbow", "Hammer", "Knife", "Mace", "Orb",
                 "Paladin Sword", "Polearm", "Scepter", "Shuriken", "Sorceress Mana Blade", "Spear", "Staff", "Sword",
                 "Wand"}


# The type of item whose picture stands for a category when the runeword has no picture of its own in it.
CATEGORY_TYPE = {"Torso": "Body Armor", "Boots": "Boots", "Gloves": "Gloves", "Helms": "Helm", "Shields": "Any Shield",
                 "Belts": "Belt", "Charms": "Charm"}
IMAGE_PREFIX = (("items_armor_armor", "Torso"), ("items_armor_boot", "Boots"), ("items_armor_glove", "Gloves"),
                ("items_armor_helmet", "Helms"), ("items_armor_shield", "Shields"), ("items_armor_belt", "Belts"),
                ("items_misc_charm", "Charms"), ("items_weapon", "Weapons"))


def image_category(key):
    """The category a picture belongs to, by its name ("items_armor_helmet_..." = Helms); "" when not known."""
    return next((cat for prefix, cat in IMAGE_PREFIX if key.startswith(prefix)), "")


def runeword_image(row, cat, word_image, art):
    """The picture of a runeword card in one category: the mod's own picture when it is of that kind of item, else the
    picture of the kind of item of the category (the first weapon type the runeword allows, for weapons)."""
    if word_image and image_category(word_image) == cat:
        return word_image
    if cat == "Weapons":
        kinds = [t for t in row["en_allowed"] if t in WEAPON_TOKENS] + ["Weapon"]
    else:
        kinds = [CATEGORY_TYPE.get(cat, "")]
    return next((art["type"][k] for k in kinds if k in art["type"]), word_image if image_category(word_image or "") == "" else None)


def norm(text):
    return re.sub(r"[^a-z0-9]", "", text.lower())


def slug(text):
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


def plural(sockets):
    return "%s Socket%s" % (sockets, "" if sockets == "1" else "s")


# ---- art table -------------------------------------------------------------------------------------------------------

CAP_CATEGORIES = {"Torso": "tors", "Boots": "boot", "Gloves": "glov", "Helms": "helm", "Shields": "shld", "Belts": "belt",
                  "Weapons": "weap"}


def socket_caps(excel_dir):
    """MAXSOCKETS (by item type name) and MAXCAT (by category) rows for web-word-art.txt: the most sockets a base item of
    the type can have, from the mod's itemtypes.txt, armor.txt and weapons.txt (a base's own sockets, held to its type's
    highest maximum). Charms are left out: the mod gives them runewords of their own."""
    import csv

    def rd(name):
        with open(Path(excel_dir) / name, encoding="latin-1", newline="") as f:
            rows = list(csv.reader(f, delimiter="\t"))
        return [dict(zip(rows[0], r)) for r in rows[1:]]

    types = {r["Code"]: r for r in rd("itemtypes.txt") if r.get("Code")}

    def chain(code, seen=()):
        out = [code]
        for key in ("Equiv1", "Equiv2"):
            parent = types.get(code, {}).get(key, "")
            if parent in types and parent not in seen:
                out += chain(parent, seen + (code,))
        return out

    by_type, by_cat = {}, {}
    for name in ("armor.txt", "weapons.txt"):
        for r in rd(name):
            code = r.get("type", "")
            if code not in types:
                continue
            cap = min(int(r.get("gemsockets") or 0), int(types[code].get("MaxSockets3") or 0))
            for parent in chain(code):
                by_type[parent] = max(by_type.get(parent, 0), cap)
    lines = ["MAXSOCKETS\t%s\t%d" % (types[c]["ItemType"], n) for c, n in sorted(by_type.items()) if types[c].get("ItemType")]
    lines += ["MAXCAT\t%s\t%d" % (cat, by_type.get(code, 0)) for cat, code in CAP_CATEGORIES.items()]
    return "\n".join(lines) + "\n"


def read_art(text):
    """{'word': {(name, ingredients): key}, 'type': {token: key}, 'icon': {word: key}, 'alias': {from: to}}"""
    art = {"word": {}, "type": {}, "icon": {}, "alias": {}, "maxtype": {}, "maxcat": {}}
    for line in text.split("\n"):
        parts = line.split("\t")
        if parts[0] == "RUNEWORD" and len(parts) >= 4:
            art["word"][(norm(parts[1]), parts[2])] = parts[3]
        elif parts[0] == "TYPE" and len(parts) >= 3:
            art["type"][parts[1]] = parts[2]
        elif parts[0] == "ICON" and len(parts) >= 3:
            art["icon"][parts[1].lower()] = parts[2]
        elif parts[0] == "MAXSOCKETS" and len(parts) >= 3:
            art["maxtype"][parts[1]] = max(art["maxtype"].get(parts[1], 0), int(parts[2]))
        elif parts[0] == "MAXCAT" and len(parts) >= 3:
            art["maxcat"][parts[1]] = int(parts[2])
        elif parts[0] == "ALIAS" and len(parts) >= 3:
            art["alias"][parts[1].lower()] = parts[2]
    return art


def ingredient_key(names):
    """The art table's key for a list of ingredient names: any jewel is just "jewel"."""
    return "+".join("jewel" if "jewel" in n.lower() else norm(n) for n in names)


# ---- the art table, written by the extractor ---------------------------------------------------------------------------

GEM_ALIASES = (("sapphire", "saphire"), ("obsidian", "onyx"))  # the mod's own file names
JEWEL_ICON = "items_misc_gem_celestial_shard"


def art_table(mod_text):
    """web-word-art.txt from the mod-built wiki data: the picture of each runeword (by name and runes), a fallback
    picture for each kind of item, and the icon names the website's gems and jewels need."""
    lines = mod_text.split("\n")
    start = next((i for i, l in enumerate(lines) if l.startswith("PAGE\trunewords\t")), None)
    if start is None:
        return ""
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("PAGE\t")), len(lines))
    words, types, block = {}, {}, None
    for line in lines[start:end]:
        parts = line.split("\t")
        if parts[0] == "BLOCK":
            block = {"name": parts[2], "base": "", "img": None, "runes": None}
        elif block is None:
            continue
        elif parts[0] == "FIELD" and parts[1] == "Base types":
            block["base"] = parts[2].split(", ")[0]
        elif parts[0] == "IMG" and block["img"] is None:
            block["img"] = parts[1]
        elif parts[0] == "LINE" and len(parts) > 2 and parts[2].startswith("\u00ffc7") and block["runes"] is None:
            block["runes"] = parts[2][3:].split(" + ")
        if block["img"] and block["runes"] is not None and block.get("done") is None:
            block["done"] = True
            words.setdefault((block["name"], ingredient_key(block["runes"])), block["img"])
            if block["base"]:
                types.setdefault(block["base"], block["img"])
    out = ["RUNEWORD\t%s\t%s\t%s" % (name, key, img) for (name, key), img in words.items()]
    if "Armor" in types:
        types.setdefault("Body Armor", types["Armor"])
    out += ["TYPE\t%s\t%s" % (token, img) for token, img in types.items()]
    out += ["ICON\tjewel\t%s" % JEWEL_ICON]
    out += ["ALIAS\t%s\t%s" % pair for pair in GEM_ALIASES]
    return "\n".join(out) + "\n"


# ---- the gems page (levels) -------------------------------------------------------------------------------------------

def read_levels(text):
    """{(name, uncoloured): level} for every rune and gem of gems.htm."""
    levels = {}
    for row in re.findall(r"<tr[^>]*>(.*?)</tr>", text, re.S | re.I):
        cells = re.findall(r"<td[^>]*>(.*?)</td>", row, re.S | re.I)
        if len(cells) != 1:
            continue
        plain = clean(re.sub(r"<[^>]+>", " ", cells[0]))
        found = re.search(r"Req\s*Lvl:?\s*(\d+)", plain, re.I)
        if not found:
            continue
        name = re.split(r"\s*Req\s*Lvl", plain, maxsplit=1, flags=re.I)[0].strip()
        coloured = re.search(r"<b>\s*<font[^>]*color\s*=", cells[0], re.I) is not None
        levels.setdefault((name, not coloured), int(found.group(1)))
    return levels


def level_of(levels, name, uncoloured):
    for key in ((name, uncoloured), (name, not uncoloured)):
        if key in levels:
            return levels[key]
    return None


# ---- the recipe pages --------------------------------------------------------------------------------------------------

def joined_text(html_text):
    """The text of a piece of HTML with the tags removed (not replaced by spaces: "<span>Orb</span>, " is "Orb, ")."""
    return clean(re.sub(r"<[^>]+>", "", html_text))


def socketable_groups(cell):
    """[(label, [(code, text)])] of the five-cell layout's last cell: one group per div class="rws-group" (its label
    div, then its lines), or a single group with an empty label when the cell has plain lines."""
    starts = [m.start() for m in re.finditer(re.escape(GROUP_MARK), cell, re.I | re.A)]
    if not starts:
        lines = [(c, t) for c, t in property_lines(cell, dash=False) if c is not GAP]
        return [("", lines)] if lines else []
    groups = []
    for n, start in enumerate(starts):
        open_end = cell.find(">", start)
        if open_end == -1:
            break
        end = len(cell)
        if n + 1 < len(starts):                    # the next group starts at the "<" of its div
            end = cell.rfind("<", 0, starts[n + 1])
            if end == -1:
                end = starts[n + 1]
        body = cell[open_end + 1:end]
        label = ""
        mark = re.search(re.escape(LABEL_MARK), body, re.I | re.A)
        if mark:
            label_open = body.find(">", mark.start())
            close = re.compile(r"</div>", re.I | re.A).search(body, label_open + 1) if label_open != -1 else None
            label_close = close.start() if close else -1
            if label_close != -1:
                label = joined_text(body[label_open + 1:label_close])
                body = body[label_close + 6:]
        lines = [(c, t) for c, t in property_lines(body, dash=False) if c is not GAP]
        if label or lines:
            groups.append((label, lines))
    return groups


def parse_rows(text):
    """[{name, sockets, level, ingredients: [(text, uncoloured)], allowed: [..], excluded: [..], columns (six-cell layout)
    or bonuses and socketables (five-cell layout)}], labels (the stat column headings of the page's layout)"""
    labels = None
    head = re.search(r"<tr[^>]*>((?:\s*<th[^>]*>.*?</th>){5,6})\s*</tr>", text, re.S | re.I)
    if head:
        cells = [clean(re.sub(r"<[^>]+>", " ", c)) for c in re.findall(r"<th[^>]*>(.*?)</th>", head.group(1), re.S | re.I)]
        if all(cells[3:]):
            labels = cells[3:]
    rows = []
    for attrs, row in re.findall(r"<tr([^>]*)>(.*?)</tr>", text, re.S | re.I):
        if "recipeRow" not in attrs:
            continue
        cells = re.findall(r"<td[^>]*>(.*?)</td>", row, re.S | re.I)
        if len(cells) not in (5, 6):
            continue
        bold = re.search(r"<b>(.*?)</b>", cells[0], re.S | re.I)
        name = clean(re.sub(r"<[^>]+>", " ", bold.group(1))) if bold else ""
        sockets = re.search(r"\((\d+(?:-\d+)?)\s+[^()]+\)", clean(re.sub(r"<[^>]+>", " ", cells[0])))
        if not name or not sockets:
            continue
        found = re.search(r"Req\s*Lvl:?\s*(\d+)", clean(re.sub(r"<[^>]+>", " ", cells[0])), re.I)
        level = int(found.group(1)) if found else None
        ingredients = []
        for part in re.split(r"<br\s*/?>", cells[1], flags=re.I):
            label = clean(re.sub(r"<[^>]+>", " ", part))
            if not label:
                continue
            colors = [c for c in re.findall(r"<font[^>]*color\s*=\s*\"?([#0-9A-Za-z]+)", part, re.I)
                      if c.lower() != OUTER_COLOR]
            ingredients.append((label, not colors))
        allowed, excluded, in_excluded = [], [], False
        for part in re.split(r"<br\s*/?>", cells[2], flags=re.I):
            label = clean(re.sub(r"<[^>]+>", " ", part))
            if not label:
                continue
            if label.lower().startswith("excluded"):
                in_excluded = True
            elif in_excluded:
                excluded.append(label)
            else:
                allowed.append(label)
        if len(cells) == 5:
            bonuses = [(c, t) for c, t in property_lines(cells[3], dash=False, gaps=True) if c is not GAP]
            rows.append({"name": name, "sockets": sockets.group(1), "level": level, "ingredients": ingredients,
                         "allowed": allowed, "excluded": excluded, "columns": None, "bonuses": bonuses,
                         "socketables": socketable_groups(cells[4]),
                         "en_name": name, "en_allowed": allowed, "en_ingredients": ingredients})
            continue
        columns = []
        for cell in cells[3:]:
            groups, current = [], []
            for code, line in property_lines(cell, dash=False, gaps=True):
                if code is GAP:
                    if current:
                        groups.append(current)
                    current = []
                else:
                    current.append((code, line))
            if current:
                groups.append(current)
            columns.append(groups)
        rows.append({"name": name, "sockets": sockets.group(1), "level": level, "ingredients": ingredients,
                     "allowed": allowed, "excluded": excluded, "columns": columns,
                     # identity (art, categories, levels): the English text when a translated page is read
                     "en_name": name, "en_allowed": allowed, "en_ingredients": ingredients})
    if labels is None or len(labels) != (2 if rows and rows[0]["columns"] is None else 3):
        labels = list(NEW_LABELS if rows and rows[0]["columns"] is None else LABELS)
    return rows, labels


def allowed_text(row):
    text = ", ".join(row["allowed"])
    if row["excluded"]:
        text += " (Excluded: %s)" % ", ".join(row["excluded"])
    return text


def can_hold(row, token, cat, art):
    """False when the item type is known to take fewer sockets than the card needs (the lowest of a range)."""
    if art is None:
        return True
    if token == "Missile":  # the website's name for missile weapons (the item type of that name is arrows and bolts)
        token = "Missile Weapon"
    cap = art["maxcat"].get(cat) if token == "Any Armor" else art["maxtype"].get(token, art["maxcat"].get(cat))
    return cap is None or int(row["sockets"].split("-")[0]) <= cap


def categories_of(row, first_only, art=None):
    found = []
    for token in row["en_allowed"]:
        if token in ARMOR_TOKENS:
            cats = ARMOR_TOKENS[token]
        elif token in WEAPON_TOKENS:
            cats = ("Weapons",)
        else:
            cats = ("Other",)
        for cat in cats:
            if can_hold(row, token, cat, art) and cat not in found:
                found.append(cat)
        if first_only and found:
            break
    if not found and art is not None:
        return []                                  # no allowed item can hold that many sockets: no card
    return (found[:1] if first_only else found) or ["Other"]


def label_kind(cat):
    """The item-kind word the website's column labels use for a category ("Torso" is "Armor"); "" for charms."""
    return {"Torso": "Armor"}.get(cat, cat) if cat in ("Weapons", "Torso", "Gloves", "Helms", "Boots", "Shields", "Belts") else ""


def stat_lines(row, labels, heading, cat=None):
    """The card's stat lines, group by group, from the website column that names the category's kind of item (no labels).
    With a heading (runewords) the last group is the rune bonuses and gets the heading. Charms have no column of their
    own: they get only the groups every filled column agrees on, and
    no rune bonuses."""
    columns = row["columns"]
    count = max((len(c) for c in columns), default=0)
    out = []
    for g in range(count):
        cells = [c[g] if g < len(c) else [] for c in columns]
        if not any(cells):
            continue
        if heading and g == count - 1 and count >= 2:
            kind = label_kind(cat)
            mine = [c for c, label in zip(cells, labels) if c and kind and kind in label.split(" / ")]
            if mine:
                out.append("LINE\tS\tÿc4%s" % heading)
                out += ["LINE\tS\tÿc%s%s" % (code, text) for code, text in mine[0]]
            continue
        kind = label_kind(cat)
        if kind:                                   # only the column that names the category's kind of item
            cell = next((c for c, label in zip(cells, labels) if kind in label.split(" / ")), [])
        else:                                      # charms: what every filled column agrees on (empty columns are kinds the row does not allow)
            used = [c for c in cells if c]
            cell = used[0] if used and all(c == used[0] for c in used) else []
        out += ["LINE\tS\tÿc%s%s" % (code, text) for code, text in cell]
    return out


def word_lines(row, labels):
    """The five-cell layout's stat lines: the bonuses under the page's first heading, then the socketable bonuses under
    its second, each group's label (when it has one) as a small dim sub-heading (LINE L) over its lines."""
    out = []
    if row["bonuses"]:
        out.append("LINE\tS\t\u00ffc4%s" % labels[0])
        out += ["LINE\tS\t\u00ffc%s%s" % (code, text) for code, text in row["bonuses"]]
    if row["socketables"]:
        out.append("LINE\tS\t\u00ffc4%s" % labels[1])
        for label, lines in row["socketables"]:
            if label:
                out.append("LINE\tL\t%s" % label)
            out += ["LINE\tS\t\u00ffc%s%s" % (code, text) for code, text in lines]
    return out


def gem_key(name, art):
    words = [art["alias"].get(w, w) for w in slug(name).split("_")]
    return "items_misc_gem_" + "_".join(words)


def icon_of(label, uncoloured, art):
    """(icon key, label, lod flag) of a rune or gem, or None."""
    for word, key in art["icon"].items():
        if word in label.lower():
            return key, label, False
    if label.endswith(" Rune"):
        return "items_misc_rune_" + slug(label), label[: -len(" Rune")], uncoloured
    return gem_key(label, art), label, False


def build_page(kind, rows, labels, art, levels):
    runewords = kind == "runewords"
    entries = {cat: [] for cat in CATEGORIES}
    seen = {cat: set() for cat in CATEGORIES}
    for row in rows:
        names = [n for n, _ in row["en_ingredients"]]
        icons = [icon_of(n, u, art) for n, u in row["en_ingredients"]]
        level = 0
        if row.get("level") is not None:          # the website's own required level (five-cell layout)
            level = row["level"]
        elif runewords:
            known = [level_of(levels, n, u) for n, u in row["en_ingredients"]]
            level = max([k for k in known if k is not None], default=0)
        word_image = art["word"].get((norm(row["en_name"]), ingredient_key(names))) if runewords else None
        head = ["BLOCK\t1\t%s\t\t%s\t\t%s\t%d" % (row["name"], plural(row["sockets"]), "" if runewords else row["name"], level),
                "FIELD\tSockets\t%s" % row["sockets"], "FIELD\tBase types\t%s" % allowed_text(row)]
        tail = []
        if runewords:
            for key, label, lod in icons:
                tail.append("RUNE\t%s\t%s%s" % (key, label, "\tlod" if lod else ""))
        tail.append("LINE\tS\tÿc4%s" % row["name"])
        tail.append("LINE\tS\tÿc7%s" % " + ".join(names))
        for cat in categories_of(row, first_only=False, art=art):
            img = runeword_image(row, cat, word_image, art) if runewords else (icons[0][0] if icons else None)
            if row["columns"] is None:
                tail2 = tail + word_lines(row, labels)
            else:
                tail2 = tail + stat_lines(row, labels, "Rune Bonuses:" if runewords else None, cat)
            if level:
                tail2.append("LINE\tS\tÿc5Required Level: %d" % level)
            tail2.append("LINE\tS\tÿc0%s" % allowed_text(row))
            card = head + (["IMG\t%s" % img] if img else []) + tail2
            # the website lists a few recipes twice, word for word; a translated card also has to match in English
            key = (tuple(card), row["en_name"], tuple(row["en_allowed"]), tuple(names))
            if key not in seen[cat]:
                seen[cat].add(key)
                entries[cat].append(card)
    title = "Runewords" if runewords else "Gemwords"
    out = ["PAGE\t%s\t%s\tWiki" % (kind, title)]
    for cat in CATEGORIES:
        if not entries[cat]:
            continue
        flag = "sortable" if runewords else "compact\tsocketsort"  # gemwords are sorted by socket count
        out.append("ENTRY\t%s\t%d items\t%s" % (cat, len(entries[cat]), flag))
        for block in entries[cat]:
            out += block
    return out


def splice(lines, page_lines, page_id, after):
    """lines with the page whose id is page_id replaced; a file without one gets it after the page `after` (else last)."""
    starts = [i for i, l in enumerate(lines) if l.startswith("PAGE\t")]
    spans = {}
    for n, i in enumerate(starts):
        spans[lines[i].split("\t")[1]] = (i, starts[n + 1] if n + 1 < len(starts) else len(lines))
    if page_id in spans:
        a, b = spans[page_id]
        return lines[:a] + page_lines + lines[b:]
    at = spans[after][1] if after in spans else len(lines)
    return lines[:at] + page_lines + lines[at:]


FILLER_WORDS = {"rune", "une", "pas", "de", "jewel", "jewels", "joyau", "joyaux", "bijou"}


LEVEL_TEXT = r"(?:Req\s*Lvl|Niv\.?\s*requis)"


def gem_names(text):
    """The rune and gem names of a gems page, in page order (each row that names a required level)."""
    names = []
    for row in re.findall(r"<tr[^>]*>(.*?)</tr>", text, re.S | re.I):
        cells = re.findall(r"<td[^>]*>(.*?)</td>", row, re.S | re.I)
        if len(cells) != 1:
            continue
        plain = clean(re.sub(r"<[^>]+>", " ", cells[0]))
        if re.search(LEVEL_TEXT, plain, re.I):
            names.append(re.split(r"\s*" + LEVEL_TEXT, plain, maxsplit=1, flags=re.I)[0].strip())
    return names


def squash(name):
    return " ".join(name.lower().split())


def recipe_key(row, to_english=None):
    """Language-independent identity of a recipe: its socket count and its rune tokens ("Rune I" and "I Rune" are both "i").
    to_english maps translated rune and gem names to their English names; a name that two English names share becomes ("*",)."""
    to_english = to_english or {}
    tokens = []
    for name, _ in row["ingredients"]:
        names = to_english.get(squash(name))
        if names is not None and len(names) > 1:
            tokens.append(("*",))
            continue
        text = next(iter(names)) if names else name
        if text.lower() == "une rune":                       # the French name of the A rune
            tokens.append(("a",))
            continue
        tokens.append(tuple(w for w in re.findall(r"[^\W_]+|\(\d+-\d+\)", text.lower()) if w not in FILLER_WORDS))
    return (row["sockets"], tuple(sorted(tokens)))


def match_score(mine, theirs):
    """How many of the translated recipe's rune tokens find a partner in the English recipe; ("*",) matches any."""
    left = list(theirs)
    score = 0
    for token in sorted(mine, key=lambda t: t == ("*",)):
        if token == ("*",):
            if left:
                left.pop()
                score += 1
        elif token in left:
            left.remove(token)
            score += 1
    return score


def pair_recipes(rows, english, to_english):
    """(translated row, English row) pairs. Every pair with the same socket count is scored by its shared rune
    tokens (an identical recipe scores best); the best pairs are taken first, so one recipe cannot steal another's partner."""
    keys = [recipe_key(row, to_english) for row in rows]
    en_keys = [recipe_key(ref) for ref in english]
    scored = []
    for i, row in enumerate(rows):
        for j, ref in enumerate(english):
            if ref["sockets"] != row["sockets"]:
                continue
            exact = keys[i] == en_keys[j]
            score = match_score(keys[i][1], en_keys[j][1]) + (100 if exact else 0)
            scored.append((score, -i, -j, i, j))
    scored.sort(reverse=True)
    taken_row, taken_ref, pairs = set(), set(), {}
    for score, _, _, i, j in scored:
        if i in taken_row or j in taken_ref:
            continue
        if score < 100 and score < len(keys[i][1]) - 1:
            continue
        taken_row.add(i)
        taken_ref.add(j)
        pairs[i] = j
    for i, row in enumerate(rows):
        if i not in pairs:
            raise SystemExit("no English recipe matches %s (%s)" % (row["name"], row["sockets"]))
    return [(row, english[pairs[i]]) for i, row in enumerate(rows)]


def main(argv):
    reference = None
    for arg in [a for a in argv if a.startswith("--reference=")]:
        reference = Path(arg[len("--reference="):])
        argv = [a for a in argv if not a.startswith("--reference=")]
    gem_pairs = None                      # --gem-pairs=<English gems page>,<translated gems page>
    for arg in [a for a in argv if a.startswith("--gem-pairs=")]:
        gem_pairs = [Path(p) for p in arg[len("--gem-pairs="):].split(",")]
        argv = [a for a in argv if not a.startswith("--gem-pairs=")]
    kind = argv[1] if len(argv) > 1 else ""
    if kind == "runewords" and len(argv) in (5, 6):
        page_path, gems_path, target = Path(argv[2]), Path(argv[3]), Path(argv[4])
        art_arg = argv[5] if len(argv) > 5 else None
    elif kind == "gemwords" and len(argv) in (4, 5):
        page_path, gems_path, target = Path(argv[2]), None, Path(argv[3])
        art_arg = argv[4] if len(argv) > 4 else None
    else:
        print(__doc__)
        return 2
    try:
        page = page_path.read_text(encoding="utf-8", errors="replace")
        if "</html" not in page.lower():
            raise SystemExit("%s is incomplete" % page_path.name)
        rows, labels = parse_rows(page)
        if reference is not None:
            english, _ = parse_rows(reference.read_text(encoding="utf-8", errors="replace"))
            if len(english) != len(rows):
                raise SystemExit("the English page has %d recipes, this one %d" % (len(english), len(rows)))
            to_english = {}
            if gem_pairs is not None:
                en_gems = gem_names(gem_pairs[0].read_text(encoding="utf-8", errors="replace"))
                fr_gems = gem_names(gem_pairs[1].read_text(encoding="utf-8", errors="replace"))
                if len(en_gems) != len(fr_gems):
                    raise SystemExit("the gems pages list %d and %d names" % (len(en_gems), len(fr_gems)))
                for fr_name, en_name in zip(fr_gems, en_gems):
                    to_english.setdefault(squash(fr_name), set()).add(en_name)
            # the translated page does not list the recipes in the English order: pair them by socket count and rune names
            for row, ref in pair_recipes(rows, english, to_english):
                row["en_name"], row["en_allowed"], row["en_ingredients"] = ref["name"], ref["allowed"], ref["ingredients"]
                if row.get("level") is None:
                    row["level"] = ref.get("level")
        if len(rows) < (RUNEWORD_MIN if kind == "runewords" else GEMWORD_MIN):
            raise SystemExit("only %d recipes found; the website page looks broken" % len(rows))
        levels = {}
        if gems_path is not None:
            levels = read_levels(gems_path.read_text(encoding="utf-8", errors="replace"))
            if len(levels) < 100:
                raise SystemExit("the gems page gives only %d levels" % len(levels))
        art_path = Path(art_arg) if art_arg else target.parent / "web-word-art.txt"
        art = read_art(art_path.read_text(encoding="utf-8") if art_path.exists() else "")
        text = target.read_text(encoding="utf-8")
        lines = text.split("\n")
        trailing = lines and lines[-1] == ""
        if trailing:
            lines = lines[:-1]
        merged = splice(lines, build_page(kind, rows, labels, art, levels), kind, "runes" if kind == "runewords" else "runewords")
    except SystemExit as stop:
        print("Website %s: %s" % (kind, stop))
        return 1
    tmp = target.with_suffix(target.suffix + ".new")
    tmp.write_text("\n".join(merged) + ("\n" if trailing else ""), encoding="utf-8", newline="\n")
    tmp.replace(target)
    print("Website %s: %d recipes" % (kind, len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
