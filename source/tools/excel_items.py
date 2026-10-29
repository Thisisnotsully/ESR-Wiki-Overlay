"""Item facts from the mod's excel tables (data/global/excel).

Used by extract_wiki.py to add the structured fields the wiki text leaves out:
base item, required level, runeword base types, and set piece counts.

Lookups are by display name. Runeword rows are keyed on '*Rune Name' (the
table's 'Name' column is an internal id such as 'Runeword580'), and each row
also lists its rune codes (r01 = I Rune), which tell apart runewords that
share a name across the LoD and ESR rune families.
"""
import csv
import json
import re
from collections import Counter, OrderedDict
from pathlib import Path

RUNE_COUNT = 6

# Base item types (armor.txt / weapons.txt "type") grouped for the uniques page.
UNIQUE_GROUP_BY_TYPE = {}
for _group, _types in {
    "Weapon": "swor axe bow pole spea staf knif xbow wand scep hamm jave mace orb club taxe abow "
              "h2h1 h2h2 npol boqm tkni aspe knuc ajav bjav asnx wswd",
    "Helm": "helm phlm circ pelt",   # druid pelts are helms
    "Torso": "tors",
    "Shield": "shie ashd grim head",  # warlock offhands (grim) and necromancer voodoo heads (head)
    "Belt": "belt",
    "Boots": "boot",
    "Gloves": "glov",
    "Amulet": "amul",
    "Ring": "ring srin",
    "Jewel": "jewl",
}.items():
    for _type in _types.split():
        UNIQUE_GROUP_BY_TYPE[_type] = _group


# Items that belong in Weapon, Helm or Torso although their base type is not in the lists above.
for _group, _types in {
    "Weapon": "dclb ndgr bedg mana srkn",   # druid clubs and wands, daggers, blades, shuriken/kagekiri
    "Helm": "scrn",                          # crowns
    "Torso": "clok robe",                    # cloaks and robes
}.items():
    for _type in _types.split():
        UNIQUE_GROUP_BY_TYPE[_type] = _group

# Weapon families for the sort bar, by base type. Anything else is "Other".
FAMILY_BY_TYPE = {}
for _family, _types in {
    "Axes": "axe taxe",
    "Swords": "swor wswd bedg asnx asns mana",
    "Daggers": "knif tkni ndgr srkn",
    "Bows": "bow abow",
    "Crossbows": "xbow boqm",
    "Spears": "spea aspe bjav jave ajav",
    "Polearms": "pole npol",
    "Maces and hammers": "mace club dclb hamm",
    "Staves": "staf",
    "Wands and scepters": "wand scep",
    "Claws": "h2h1 h2h2 knuc",
    "Orbs": "orb",
}.items():
    for _type in _types.split():
        FAMILY_BY_TYPE[_type] = _family


def norm(name):
    """Key for matching names: lower case, letters and digits only."""
    return re.sub(r"[^a-z0-9]", "", str(name).lower())


def read_table(path):
    with open(path, encoding="utf-8-sig", errors="replace", newline="") as handle:
        rows = list(csv.reader(handle, delimiter="\t"))
    if not rows:
        return []
    header = rows[0]
    return [dict(zip(header, row)) for row in rows[1:] if row and any(cell.strip() for cell in row)]


class Tables:
    def __init__(self, folder, item_names=None):
        folder = Path(folder)
        self.folder = folder
        self.item_types = self._item_type_names(read_table(folder / "itemtypes.txt"))

        # Rune item levels from misc.txt (code r01..r97 -> level). levelreq is empty for runes.
        self.rune_levels = {}
        if (folder / "misc.txt").exists():
            for row in read_table(folder / "misc.txt"):
                code = row.get("code", "").strip()
                level = row.get("level", "").strip()
                if re.fullmatch(r"r\d+", code) and level.isdigit():
                    self.rune_levels[code] = int(level)

        self.uniques = {}
        self._unique_rows = [r for r in read_table(folder / "uniqueitems.txt") if (r.get("index") or "").strip()]
        for row in read_table(folder / "uniqueitems.txt"):
            key = norm(row.get("index", ""))
            if key and key not in self.uniques:
                self.uniques[key] = row

        # Display names (what the wiki and the in-game tooltip show) differ from the
        # internal index names in uniqueitems.txt ("Biggin's Bonnet" is "War Bonnet").
        # item-names.json maps internal Key -> display enUS; use it as an alias.
        self.unique_alias = {}
        if item_names and Path(item_names).exists():
            for entry in json.loads(Path(item_names).read_text(encoding="utf-8")):
                row = self.uniques.get(norm(entry.get("Key", "")))
                display = norm(entry.get("enUS", ""))
                if row is None or not display or display in self.uniques:
                    continue
                self.unique_alias.setdefault(display, row)

        # Uniques that a Horadric Cube recipe makes from a Coupon item (cubemain.txt).
        self.coupon_uniques = set()
        if (folder / "cubemain.txt").exists():
            for row in read_table(folder / "cubemain.txt"):
                if row.get("description", "").strip() != "Coupon":
                    continue
                for cell in row.values():
                    key = norm(cell)
                    if key in self.uniques:
                        self.coupon_uniques.add(key)

        # Base item code -> base type (for example "swor"), from the base item tables.
        self.base_types = {}
        self.base_names = {}
        for name in ("armor.txt", "weapons.txt", "misc.txt"):
            if not (folder / name).exists():
                continue
            for row in read_table(folder / name):
                code = row.get("code", "").strip()
                if code and code not in self.base_types:
                    self.base_types[code] = row.get("type", "").strip()
                    self.base_names[code] = row.get("name", "").strip()

        # Base item code -> Normal, Exceptional or Elite, from the tier codes on each base row.
        self.code_tiers = {}
        for name in ("armor.txt", "weapons.txt"):
            if not (folder / name).exists():
                continue
            for row in read_table(folder / name):
                for column, tier_name in (("normcode", "Normal"), ("ubercode", "Exceptional"),
                                          ("ultracode", "Elite")):
                    code = row.get(column, "").strip()
                    if code:
                        self.code_tiers.setdefault(code, tier_name)

        # Gambling (gamble.txt lists Normal-tier base codes). A base of any tier counts through its
        # Normal base. The vendor rules for quality and tier upgrades live in the game, not in the files.
        self.gamble_codes = set()
        if (folder / "gamble.txt").exists():
            self.gamble_codes = {row.get("code", "").strip() for row in read_table(folder / "gamble.txt")
                                 if row.get("code", "").strip()}
        self.normal_codes = {}
        for name in ("armor.txt", "weapons.txt"):
            if (folder / name).exists():
                for row in read_table(folder / name):
                    norm_code = row.get("normcode", "").strip()
                    for column in ("normcode", "ubercode", "ultracode"):
                        code = row.get(column, "").strip()
                        if code and norm_code:
                            self.normal_codes.setdefault(code, norm_code)

        self.set_rows = {}
        self.set_table = []  # every setitems.txt row, in file order
        set_names = []
        for row in read_table(folder / "setitems.txt"):
            self.set_table.append(row)
            key = norm(row.get("index", ""))
            if key and key not in self.set_rows:
                self.set_rows[key] = row
            if row.get("set"):
                set_names.append(row["set"])
        self.set_sizes = Counter(set_names)

        # Set bonus rows, one per difficulty: "Kashya's Survival Kit-N", "-NM" and "-H".
        self.set_bonuses = {}
        if (folder / "sets.txt").exists():
            for row in read_table(folder / "sets.txt"):
                if row.get("name", "").strip():
                    self.set_bonuses[row["name"].strip()] = row

        self.runewords = {}
        for row in read_table(folder / "runes.txt"):
            if not row.get("Rune1"):
                continue
            key = norm(row.get("*Rune Name") or row.get("Name", ""))
            self.runewords.setdefault(key, []).append(row)
        self.runewords_by_id = {row["Name"]: row for rows in self.runewords.values() for row in rows}

    @staticmethod
    def _item_type_names(rows):
        names = {}
        # A row's own Code wins over another row's Equiv column (Bow lists "miss" as an equivalent).
        for column in ("Code", "Equiv1", "Equiv2"):
            for row in rows:
                name = row.get("ItemType", "")
                code = row.get(column, "")
                if name and code and code not in names:
                    names[code] = name
        return names

    def base_type(self, code):
        return self.item_types.get(code, code)

    def coupon_only(self, title):
        """True when a Horadric Cube recipe makes this unique from a Coupon item."""
        row = self.unique_row(title)
        return row is not None and norm(row.get("index", "")) in self.coupon_uniques

    def gamble_info(self, title):
        """(Normal base item name, item level) when this unique can come from gambling, else None.
        Not for Coupon uniques; a disabled, non-spawnable or skip-generation row never drops."""
        row = self.unique_row(title)
        if row is None or norm(row.get("index", "")) in self.coupon_uniques:
            return None
        if row.get("disabled", "").strip() not in ("", "0") or row.get("spawnable", "").strip() != "1":
            return None
        if row.get("*Skip Generation", "").strip() not in ("", "0"):
            return None
        code = row.get("code", "").strip()
        normal = self.normal_codes.get(code, code)
        if normal not in self.gamble_codes:
            return None
        return self.base_names.get(normal, ""), row.get("lvl", "").strip()

    def unique_fields(self, title):
        row = self.unique_row(title)
        if row is None:
            return None
        return [("Base", row.get("*ItemName", "")), ("Required Level", row.get("lvl req", ""))]

    def code(self, title):
        """Base item code of a unique, or ""."""
        row = self.unique_row(title)
        return row.get("code", "").strip() if row is not None else ""

    def tier(self, title):
        """Normal, Exceptional or Elite, from the base item the unique is made on."""
        row = self.unique_row(title)
        if row is None:
            return ""
        return self.code_tiers.get(row.get("code", "").strip(), "")

    def is_lod_rune(self, code):
        """True for the LoD runes (codes r51 to r83 in misc.txt), as opposed to the Kanji runes."""
        match = re.fullmatch(r"r(\d+)", code or "")
        return bool(match) and 51 <= int(match.group(1)) <= 83

    def rune_level(self, code):
        """Level of a rune item by its code (r19 is the Kanji Ko), or 0 when unknown."""
        return self.rune_levels.get(code, 0)

    def level(self, title):
        """Required level of a unique, for sorting (0 when unknown)."""
        row = self.unique_row(title)
        if row is None:
            return 0
        try:
            return int(row.get("lvl req", "0") or 0)
        except ValueError:
            return 0

    def code_tier(self, code):
        """Normal, Exceptional or Elite for a base item code, or ""."""
        return self.code_tiers.get(code, "")

    def code_family(self, code):
        """Weapon family for a base item code (for the Type filter), or ""."""
        base = self.base_types.get(code, "")
        return FAMILY_BY_TYPE.get(base, "Other") if base else ""

    def family(self, title):
        """Weapon family for the sort bar (Axes, Swords, ...); "Other" when not listed."""
        base = self.base_type(title)
        return FAMILY_BY_TYPE.get(base, "Other") if base else ""

    def base_name(self, title):
        """Name of the base item a unique is made from (for example "Ring"), or ""."""
        row = self.unique_row(title)
        if row is None:
            return ""
        return self.base_names.get(row.get("code", "").strip(), "")

    def unique_row(self, title):
        """uniqueitems.txt row for a unique, matched by index name or display name."""
        key = norm(title)
        return self.uniques.get(key) or self.unique_alias.get(key)

    def unique_index_name(self, title):
        """Internal index name of a unique (for example "War Bonnet"), or ""."""
        row = self.unique_row(title)
        return row.get("index", "").strip() if row else ""

    def base_type(self, title):
        """Base type code of a unique's base item (for example "axe"), or ""."""
        row = self.unique_row(title)
        if row is None:
            return ""
        return self.base_types.get(row.get("code", "").strip(), "")

    def unique_rows_in_order(self):
        return list(self._unique_rows)

    def coupon_only_row(self, row):
        return norm(row.get("index", "")) in self.coupon_uniques

    def gamble_info_row(self, row):
        """gamble_info for one uniqueitems.txt row (a name can have several rows)."""
        if norm(row.get("index", "")) in self.coupon_uniques:
            return None
        if row.get("disabled", "").strip() not in ("", "0") or row.get("spawnable", "").strip() != "1":
            return None
        if row.get("*Skip Generation", "").strip() not in ("", "0"):
            return None
        code = row.get("code", "").strip()
        normal = self.normal_codes.get(code, code)
        if normal not in self.gamble_codes:
            return None
        return self.base_names.get(normal, ""), row.get("lvl", "").strip()

    def class_of_code(self, code):
        """Class code ("nec", "pal"...) of a class-specific base item, from itemtypes.txt, or ""."""
        if not hasattr(self, "_type_class"):
            self._type_class = {}
            self._type_parents = getattr(self, "_type_parents", {})
            for row in read_table(self.folder / "itemtypes.txt"):
                key = (row.get("Code") or "").strip()
                if key:
                    self._type_class[key] = (row.get("Class") or "").strip()
                    self._type_parents[key] = [(row.get("Equiv1") or "").strip(), (row.get("Equiv2") or "").strip()]
        todo, seen = [self.base_types.get((code or "").strip(), "")], set()
        while todo:
            current = todo.pop(0)
            if not current or current in seen:
                continue
            seen.add(current)
            if self._type_class.get(current):
                return self._type_class[current]
            todo.extend(self._type_parents.get(current, []))
        return ""

    def group_of_code(self, code):
        code = (code or "").strip()
        if code.startswith("cm"):
            return "Charm"
        if code.startswith(("aq", "cq")):
            return "Quiver"
        item_type = self.base_types.get(code, "")
        if item_type in UNIQUE_GROUP_BY_TYPE:
            return UNIQUE_GROUP_BY_TYPE[item_type]
        # Class rings and amulets and the like are types of a parent type in itemtypes.txt.
        if not hasattr(self, "_type_parents"):
            self._type_parents = {}
            for row in read_table(self.folder / "itemtypes.txt"):
                key = (row.get("Code") or "").strip()
                if key:
                    self._type_parents[key] = [(row.get("Equiv1") or "").strip(), (row.get("Equiv2") or "").strip()]
        todo, seen = [item_type], set()
        while todo:
            current = todo.pop(0)
            if not current or current in seen:
                continue
            seen.add(current)
            if current in UNIQUE_GROUP_BY_TYPE:
                return UNIQUE_GROUP_BY_TYPE[current]
            todo.extend(self._type_parents.get(current, []))
        return "Other"

    def unique_group(self, title):
        """Display group for a unique, from its base item: Weapon, Helm, ... or Other."""
        row = self.unique_row(title)
        if row is None:
            return "Other"
        code = row.get("code", "").strip()
        if code.startswith("cm"):
            return "Charm"
        if code.startswith(("aq", "cq")):
            return "Quiver"
        return UNIQUE_GROUP_BY_TYPE.get(self.base_types.get(code, ""), "Other")

    def set_fields(self, title):
        row = self.set_rows.get(norm(title))
        if row is None:
            # Difficulty variants: "Rogue's Bow" is listed as "Rogue's Bow Normal" / "... Hell".
            key = norm(title)
            row = next((r for k, r in self.set_rows.items() if k.startswith(key)), None)
        if row is None:
            # Some set cards are titled with the set's name, not one piece's name.
            for set_name, count in self.set_sizes.items():
                if norm(set_name) == norm(title):
                    return [("Set", set_name), ("Set pieces", str(count))]
            return None
        return [
            ("Set", row.get("set", "")),
            ("Base", row.get("*ItemName", "")),
            ("Required Level", row.get("lvl req", "")),
            ("Set pieces", str(self.set_sizes.get(row.get("set", ""), 0))),
        ]

    def hireling_rows(self):
        """The mercenary-only set items: setitems.txt rows whose set ends in -N, -NM or -H. Returns
        (row, set name without the suffix, difficulty) in table order, skipping disabled rows."""
        names = {"N": "Normal", "NM": "Nightmare", "H": "Hell"}
        out = []
        for row in self.set_table:
            match = re.fullmatch(r"(.+)-(N|NM|H)", row.get("set", "").strip())
            if match and row.get("index", "").strip() and row.get("disabled", "").strip() in ("", "0"):
                out.append((row, match.group(1), names[match.group(2)]))
        return out

    def set_bonus(self, set_name, suffix):
        """Partial and full set bonus properties of one difficulty (suffix N, NM or H), as
        (code, param, value) tuples in table order, or None when sets.txt has no such row."""
        row = self.set_bonuses.get(f"{set_name}-{suffix}")
        if row is None:
            return None

        def read(code_column):
            code = row.get(code_column, "").strip()
            if not code:
                return None
            return (code, row.get(code_column.replace("Code", "Param"), "").strip(),
                    row.get(code_column.replace("Code", "Min"), "").strip())

        partial = [prop for prop in (read("PCode2b"), read("PCode2a")) if prop]
        full = [prop for prop in (read("FCode%d" % i) for i in range(1, 9)) if prop]
        return {"partial": partial, "full": full}

    def keyed_rows(self, title, key):
        """Runeword rows named title. A card's own row (its Runeword ID) is used when there is one,
        so cards that share runes (Rain) each keep their own base types."""
        if key and key in self.runewords_by_id:
            return [self.runewords_by_id[key]]
        return self.runewords.get(norm(title), [])

    def runeword_fields(self, title, rune_codes, key=None):
        rows = self.keyed_rows(title, key)
        wanted = list(rune_codes)
        match = [r for r in rows if [r.get("Rune%d" % i, "") for i in range(1, RUNE_COUNT + 1) if r.get("Rune%d" % i, "")] == wanted]
        if not match:
            match = [r for r in rows if sorted(r.get("Rune%d" % i, "") for i in range(1, RUNE_COUNT + 1) if r.get("Rune%d" % i, "")) == sorted(wanted)]
        if not match and rows:
            # The card's codes do not match any row exactly (a layout card can show a different
            # rune of the same name, such as the second Ko). Use the row sharing the most runes.
            def shared(row):
                return len(set(wanted) & {row.get("Rune%d" % i, "") for i in range(1, RUNE_COUNT + 1)})
            best = max(shared(r) for r in rows)
            match = [r for r in rows if shared(r) == best] if best else rows
        if not match:
            return None
        bases = OrderedDict()
        for row in match:
            for i in range(1, RUNE_COUNT + 1):
                code = row.get("itype%d" % i, "")
                if code:
                    # Item type names come from itemtypes.txt. base_type() is for uniques and
                    # would return "" here.
                    bases[self.item_types.get(code, code)] = True
        return [("Base types", ", ".join(bases))] if bases else []

    def runeword_row_types(self, title, rune_codes, key=None):
        """Item type names of the runeword row that matches the card's runes (for a card that lists
        more than one base, such as Spirit's sword and shield)."""
        rows = self.keyed_rows(title, key)
        if not rows:
            return []
        wanted = sorted(rune_codes)
        def runes(row):
            return sorted(row.get("Rune%d" % i, "") for i in range(1, RUNE_COUNT + 1) if row.get("Rune%d" % i, ""))
        row = next((r for r in rows if runes(r) == wanted), None)
        if row is None:
            def shared(r):
                return len(set(wanted) & {r.get("Rune%d" % i, "") for i in range(1, RUNE_COUNT + 1)})
            row = max(rows, key=shared)
        names = []
        for i in range(1, RUNE_COUNT + 1):
            code = row.get("itype%d" % i, "")
            if code:
                name = self.item_types.get(code, code)
                if name not in names:
                    names.append(name)
        return names

    def runeword_cells(self, title, rune_codes, key=None):
        """Socket codes of the card's row in order: rune codes (r..), jewel (jew, mjw) and gem codes."""
        rows = self.keyed_rows(title, key)
        if not rows:
            return []
        wanted = sorted(rune_codes)
        def runes(row):
            return sorted(row.get("Rune%d" % i, "") for i in range(1, RUNE_COUNT + 1) if row.get("Rune%d" % i, ""))
        row = next((r for r in rows if runes(r) == wanted), rows[0])
        return [row.get("Rune%d" % i, "") for i in range(1, RUNE_COUNT + 1) if row.get("Rune%d" % i, "")]

    def runeword_socket_count(self, title, rune_codes, key=None):
        """Number of runes the runeword takes, from runes.txt. The card art can show fewer icons
        when a rune repeats (Conquest uses Shi Shi O, so three sockets)."""
        rows = self.keyed_rows(title, key)
        wanted = sorted(rune_codes)
        def count(row):
            return sum(1 for i in range(1, RUNE_COUNT + 1) if row.get("Rune%d" % i, ""))
        for row in rows:
            if sorted(row.get("Rune%d" % i, "") for i in range(1, RUNE_COUNT + 1) if row.get("Rune%d" % i, "")) == wanted:
                return count(row)
        if not rows:
            return 0
        # No exact match: the recipe sharing the most runes with the card.
        def shared(row):
            return len(set(wanted) & {row.get("Rune%d" % i, "") for i in range(1, RUNE_COUNT + 1)})
        best = max(rows, key=shared)
        return count(best)
