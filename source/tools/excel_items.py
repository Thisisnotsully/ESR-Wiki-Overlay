"""Item facts from the mod's excel tables (data/global/excel).

Used by extract_wiki.py to add the structured fields the wiki text leaves out:
base item, required level, runeword base types, and set piece counts.

Lookups are by display name. Runeword rows are keyed on '*Rune Name' (the
table's 'Name' column is an internal id such as 'Runeword580'), and each row
also lists its rune codes (r01 = I Rune), which tell apart runewords that
share a name across the LoD and ESR rune families.
"""
import csv
import re
from collections import Counter, OrderedDict
from pathlib import Path

RUNE_COUNT = 6

# Base item types (armor.txt / weapons.txt "type") grouped for the uniques page.
UNIQUE_GROUP_BY_TYPE = {}
for _group, _types in {
    "Weapon": "swor axe bow pole spea staf knif xbow wand scep hamm jave mace orb club taxe abow "
              "h2h1 h2h2 npol boqm tkni aspe knuc ajav bjav asnx wswd",
    "Helm": "helm phlm circ",
    "Torso": "tors pelt",
    "Shield": "shie ashd grim head",  # warlock offhands (grim) and necromancer voodoo heads (head)
    "Belt": "belt",
    "Boots": "boot",
    "Gloves": "glov",
    "Amulet": "amul",
    "Ring": "ring",
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
    def __init__(self, folder):
        folder = Path(folder)
        self.item_types = self._item_type_names(read_table(folder / "itemtypes.txt"))

        self.uniques = {}
        for row in read_table(folder / "uniqueitems.txt"):
            key = norm(row.get("index", ""))
            if key and key not in self.uniques:
                self.uniques[key] = row

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

        self.set_rows = {}
        set_names = []
        for row in read_table(folder / "setitems.txt"):
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

    @staticmethod
    def _item_type_names(rows):
        names = {}
        for row in rows:
            name = row.get("ItemType", "")
            if not name:
                continue
            for column in ("Code", "Equiv1", "Equiv2"):
                code = row.get(column, "")
                if code and code not in names:
                    names[code] = name
        return names

    def base_type(self, code):
        return self.item_types.get(code, code)

    def coupon_only(self, title):
        """True when a Horadric Cube recipe makes this unique from a Coupon item."""
        return norm(title) in self.coupon_uniques

    def unique_fields(self, title):
        row = self.uniques.get(norm(title))
        if row is None:
            return None
        return [("Base", row.get("*ItemName", "")), ("Required Level", row.get("lvl req", ""))]

    def code(self, title):
        """Base item code of a unique, or ""."""
        row = self.uniques.get(norm(title))
        return row.get("code", "").strip() if row is not None else ""

    def tier(self, title):
        """Normal, Exceptional or Elite, from the base item the unique is made on."""
        row = self.uniques.get(norm(title))
        if row is None:
            return ""
        return self.code_tiers.get(row.get("code", "").strip(), "")

    def level(self, title):
        """Required level of a unique, for sorting (0 when unknown)."""
        row = self.uniques.get(norm(title))
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
        row = self.uniques.get(norm(title))
        if row is None:
            return ""
        return self.base_names.get(row.get("code", "").strip(), "")

    def base_type(self, title):
        """Base type code of a unique's base item (for example "axe"), or ""."""
        row = self.uniques.get(norm(title))
        if row is None:
            return ""
        return self.base_types.get(row.get("code", "").strip(), "")

    def unique_group(self, title):
        """Display group for a unique, from its base item: Weapon, Helm, ... or Other."""
        row = self.uniques.get(norm(title))
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

    def runeword_fields(self, title, rune_codes):
        rows = self.runewords.get(norm(title), [])
        wanted = list(rune_codes)
        match = [r for r in rows if [r.get("Rune%d" % i, "") for i in range(1, RUNE_COUNT + 1) if r.get("Rune%d" % i, "")] == wanted]
        if not match:
            match = [r for r in rows if sorted(r.get("Rune%d" % i, "") for i in range(1, RUNE_COUNT + 1) if r.get("Rune%d" % i, "")) == sorted(wanted)]
        if not match:
            # Same name, different rune family: the card's codes do not match any row,
            # so report the base types of every row with this name.
            match = rows
        if not match:
            return None
        bases = OrderedDict()
        for row in match:
            for i in range(1, RUNE_COUNT + 1):
                code = row.get("itype%d" % i, "")
                if code:
                    bases[self.base_type(code)] = True
        return [("Base types", ", ".join(bases))] if bases else []
