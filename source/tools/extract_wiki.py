#!/usr/bin/env python3
"""Turn the ESR wiki layouts (and the mod's excel tables) into the overlay's data file.

Usage (from esr-wiki-plugin/overlay):
    python3 tools/extract_wiki.py --excel <mod>/data/global/excel

Reads  pages.json, ../data/**/*.json layouts, ../data/strings/esr-wiki-strings.json,
       and the excel tables when --excel is given (uniqueitems, setitems, runes, itemtypes)
Writes data/wiki-data.txt        (read by the C++ overlay at load time)
       data/image-manifest.txt   (every sprite the overlay needs, key -> D2R path)

Model: a PAGE holds ENTRY rows, which are the left-hand list. An ENTRY is a category
(uniques, runes, runewords...) or one guide section, and holds BLOCK rows: one item
each (item = 1, gets a hover tooltip) or one guide section (item = 0).

Output format, tab separated, UTF-8, one record per line:
    #ESRWIKI  2
    PAGE    <id> <title> <tab group>
    ENTRY   <title> <subtitle>                 left-list row
    BLOCK   <0|1 item> <title> <subtitle> <section>   item or section inside the entry
            section = group header in the detail pane (set name, "2 Sockets", class)
    FIELD   <label> <value>                    structured fact for the current block
    IMG     <image key> [small]                art for the current block (small: half size)
    NOTE    <text>                             extra notes beside the card (not in the tooltip)
    RUNE    <image key> <label>                rune icon, in sequence order
    LINE    <H|T|S> <text>                     heading, body text, or colored stat line
Tabs and newlines inside text are replaced so each record stays on one line.
"""
import argparse
import copy
import json
import re
import shutil
from collections import OrderedDict
from pathlib import Path, PurePosixPath

from wiki_layout import fields, load_layout, load_string_table, tooltip_key, walk
import excel_items
import runeword_gen

OVERLAY_DIR = Path(__file__).resolve().parent.parent
PLUGIN_DIR = OVERLAY_DIR.parent
DATA_OUT = OVERLAY_DIR / "data"

HEADING_STYLE_MARKERS = ("StyleTitleBlock", "LargeFontSize", "FontColorGold")
# Top-level folders of sprite paths that are chrome (panels, dividers, buttons), not content.
CHROME_FOLDERS = {"panel", "lobby", "pausemenu", "frontend", "options", "controller"}
STAT_PREFIXES = ("EsrWikiRuneword_", "EsrWikiSetBonus_", "EsrWikiSet_", "EsrWikiUnique_",
                 "EsrWikiGemword_", "EsrWikiClassCraft_")
RUNE_PREFIX = "EsrWikiRune_"
DEFAULT_RUNE_SPRITE = "items\\misc\\rune\\i_rune"


def clean(text):
    return re.sub(r"[\t\r\n]+", " ", str(text)).strip()


def split_lines(text):
    return [clean(part) for part in str(text).split("\n") if clean(part)]


def strip_color(text):
    return re.sub(r"ÿc.|ÿ.", "", text)


# Generic inventory icon per base item type, used when a unique has no artwork of its own
# and neither its name nor its type matches an icon file. Stems come from the base game
# item sprites (items/<category>/<stem>.sprite).
ALTERNATE_LABEL = {"Mis": "Missile", "2H": "Two-Handed"}

# A runeword card whose art is from the other family (a shield on a missile runeword, chain mail on
# a polearm) gets one of these pictures instead. Keys are the first base type from runes.txt.
RUNEWORD_FALLBACK_IMAGE = {
    "Armor": "items_armor_armor_goldskin",
    "Missile Weapon": "items_weapon_bow_long_battle_bow",
    "Polearm": "items_weapon_polearm_athenas_wrath",
    "Staff": "items_weapon_staff_long_staff",
    "Melee Weapon": "items_weapon_sword_war_sword",
    "Axe": "items_weapon_axe_the_gnasher",
    "Sword": "items_weapon_sword_war_sword",
    "Scepter": "items_weapon_scepter_war_scepter",
    "Wand": "items_weapon_wand_blackhand_key",
    "Gloves": "items_armor_glove_heavy_gloves",
    "Boots": "items_armor_boot_light_plate_boots",
    "Belt": "items_armor_belt_belt_m",
    "Helm": "items_armor_helmet_great_helm",
    "Any Shield": "items_armor_shield_small_shield",
    "Weapon": "items_weapon_sword_war_sword",
    "Charm": "items_misc_charm_odd_charm",
}
# Default picture for each card category when no base type of it has one.
RUNEWORD_GROUP_DEFAULT = {"Weapon": "Weapon", "Torso": "Armor", "Helms": "Helm", "Shields": "Any Shield",
                          "Charms": "Charm", "Belts": "Belt", "Boots": "Boots", "Gloves": "Gloves"}
# Sockets that take a jewel or a gem rather than a rune. Icons are generic; labels say what fits.
JEWEL_CODES = {"jew": "Any Jewel", "mjw": "Any Jewel"}
GEM_ICON = "items\\misc\\gem\\flawless_diamond"
JEWEL_ICON = "items\\misc\\gem\\celestial_shard"


def socket_label(code):
    """"Any Jewel" or "Any Gem" for a socket code that is not a rune, else None."""
    if code in JEWEL_CODES:
        return JEWEL_CODES[code]
    if re.fullmatch(r"g[a-z]{2}", code or ""):
        return "Any Gem"
    return None


SHIELD_RUNEWORD_IMAGE = "items_armor_shield_small_shield"
ARMOR_BASE = re.compile(r"Armor|Helm|Shield|Charm|Boots|Gloves|Belt")
WEAPON_BASE = re.compile(r"Weapon|Sword|Axe|Mace|Hammer|Club|Staff|Polearm|Spear|Wand|Knife|Scepter|Orb|Blade|Bow|Crossbow|Javelin|Hand to Hand|Blunt|Katana|Shuriken|Claw")


def runeword_category(name):
    """The card category (group) a runeword base type belongs in."""
    if name in ("Helm", "Pelt"):
        return "Helms"
    if name in ("Armor", "Any Armor"):
        return "Torso"
    if name in ("Any Shield", "Shield"):
        return "Shields"
    if name == "Charm":
        return "Charms"
    if name == "Belt":
        return "Belts"
    if name == "Boots":
        return "Boots"
    if name == "Gloves":
        return "Gloves"
    return "Weapon"


def runeword_category_of_card(name):
    """The category a carousel card name (card_<Group>_<row>_<n>) belongs to."""
    parts = name.split("_")
    return parts[1] if len(parts) > 1 else ""


def set_card_type_line(block, label):
    block["lines"] = [(kind, ("\u00ffc0" + label) if kind == "S" and text.startswith("\u00ffc0") else text)
                      for kind, text in block["lines"]]
    block["fields"] = [(name, label if name == "Base types" else value) for name, value in block["fields"]]


def base_family(first):
    """"armor" or "weapon" for a base type, or None when it cannot be told."""
    if first in RUNEWORD_FALLBACK_IMAGE:
        return "armor" if RUNEWORD_FALLBACK_IMAGE[first].startswith("items_armor") else "weapon"
    if ARMOR_BASE.search(first):
        return "armor"
    if WEAPON_BASE.search(first):
        return "weapon"
    return None

TYPE_ICON_STEM = {
    "abow": "long_bow", "bow": "short_bow", "xbow": "crossbow",
    "ajav": "javelin", "jave": "javelin", "bjav": "spear", "aspe": "spear", "spea": "spear",
    "ashd": "rondache", "shie": "kite_shield",
    "asnx": "short_sword", "asns": "short_sword", "wswd": "short_sword", "circ": "circlet", "robe": "quilted_armor", "mace": "mace", "swor": "long_sword", "bedg": "scimitar",
    "mana": "blade", "knif": "dagger", "tkni": "dagger", "ndgr": "dagger", "srkn": "throwing_knife",
    "h2h1": "claws", "h2h2": "claws", "knuc": "katar",
    "axe": "axe", "taxe": "hand_axe", "hamm": "war_hammer",
    "club": "club", "dclb": "club", "pole": "poleaxe", "npol": "scythe",
    "scep": "scepter", "staf": "short_staff", "wand": "wand", "orb": "clasped_orb",
    "helm": "helm", "phlm": "great_helm", "head": "bone_helm", "pelt": "wolf_head", "scrn": "crown",
    "tors": "chain_mail", "clok": "victors_silk",
    "belt": "belt_m", "boot": "leather_boots", "glov": "gloves_l",
    "grim": "grimoire", "amul": "amulet", "ring": "ring", "tors2": "quilted_armor",
}


# Mythical base words -> generic icon stem, for mythicals without artwork of their own.
MYTHICAL_ICON_WORDS = [
    ("Scepter", "scepter"), ("Wand", "wand"), ("Claws", "claws"), ("Suwayyah", "claws"),
    ("Blade", "blade"), ("Staff", "short_staff"), ("Javelin", "javelin"), ("Whale Hunter", "spear"),
    ("Katana", "long_sword"), ("Hatchet", "hand_axe"), ("Dream Spirit", "wolf_head"), ("Ring", "ring"),
    ("Large Charm", "charm_large"), ("Small Charm", "charm_small"), ("Amulet", "amulet"),
]


# Class amulets: the base code of each class's amulet, and the class it belongs to.
CLASS_BY_AMULET_CODE = {"zam": "Amazon", "sam": "Sorceress", "nam": "Necromancer", "pam": "Paladin",
                        "bam": "Barbarian", "dam": "Druid", "aam": "Assassin", "wam": "Warlock"}


def tag_class_skill(text, class_name):
    """'+1 to Bow and Crossbow Skills' -> '... (Amazon Only)' on a class amulet. 'All Skills' is not class-only."""
    plain = text.rstrip()
    if (re.search(r" Skills?$", plain) and "All Skills" not in plain and "Only" not in plain
            and "Class" not in plain):
        return plain + " (%s Only)" % class_name
    return text


def unique_key(name):
    """'Lothar's Will' -> 'lothars_will', the key style used by uniques.json."""
    return re.sub(r"[^a-z0-9]+", "_", name.lower().replace("'", "")).strip("_")


def load_sprite_index(ui_root):
    """Index the non-low-end unique artwork under <ui root>/items."""
    index = {}
    root = Path(ui_root)
    for path in (root / "items").rglob("*.sprite"):
        if path.name.lower().endswith(".lowend.sprite"):
            continue
        rel = path.relative_to(root).with_suffix("").as_posix()
        index.setdefault(path.stem.lower(), rel)
    return index


def image_key(filename):
    """'items\\\\armor\\\\helm' -> 'items_armor_helm' (lower case, stable across runs)."""
    parts = [p for p in re.split(r"[\\/]+", filename) if p]
    return "_".join(parts).lower()


def sprite_rel_path(filename):
    parts = [p for p in re.split(r"[\\/]+", filename) if p]
    return "/".join(parts) + ".sprite"


def is_content_image(filename):
    if not filename:
        return False
    parts = [p for p in re.split(r"[\\/]+", filename) if p]
    if not parts or parts[0].lower() in CHROME_FOLDERS:
        return False
    return "divider" not in filename.lower()


def is_heading(text_node):
    style = str(fields(text_node).get("style", ""))
    return any(marker in style for marker in HEADING_STYLE_MARKERS)


def new_block(item=False, title="", subtitle="", section=""):
    return {"item": item, "title": title, "subtitle": subtitle, "section": section, "images": [],
            "runes": [], "fields": [], "lines": [], "note": "",
            "tier": "", "family": "", "level": 0}


REQUIRED_LEVEL = re.compile(r"Required Level:\s*(\d+)")


def required_level(lines):
    """The 'Required Level' number in a block's text lines, or 0 when it has none."""
    for _kind, text in lines:
        match = REQUIRED_LEVEL.search(strip_color(text))
        if match:
            return int(match.group(1))
    return 0


TIER_SUFFIX = {"Normal": "N", "Nightmare": "NM", "Hell": "H"}
TITLE_COLOR = "\u00ffcC"            # set bonus card title, as in the layout
COLOR_GOLD_LINE = "\u00ffc4"        # "Partial Set Bonus", "(2 items)", "Full Set Bonus"
COLOR_BONUS_LINE = "\u00ffc3"       # each bonus property

# Property code in sets.txt -> text on the set bonus card. {v} is the value.
SET_BONUS_TEXT = {
    "allskills": "+{v} to All Skills",
    "hp": "+{v} to Life",
    "hp%": "Increase Maximum Life +{v}%",
    "all-stats": "+{v} to All Attributes",
    "res-all-max": "+{v}% to All Maximum Resists",
    "red-dmg": "Damage Reduced by {v}",
    "thorns": "Attacker Takes Damage of {v}",
}


def bonus_texts(props, unmatched):
    texts = []
    for code, param, value in props:
        if code == "oskill":
            texts.append(f"+{value} To {param}")
        elif code in SET_BONUS_TEXT:
            texts.append(SET_BONUS_TEXT[code].format(v=value))
        else:
            unmatched.add(code)
            texts.append(" ".join(part for part in (code, param, value) if part))
    return texts


def pick_shard_source(sources, tier):
    """The source card whose art matches the difficulty (nightmare or hell in the image key);
    Normal has no difficulty word in its art. Falls back to the first card."""
    word = {"Nightmare": "nightmare", "Hell": "hell"}.get(tier)
    for card in sources:
        art = " ".join(card["images"]).lower()
        if word and word in art:
            return card
        if not word and "nightmare" not in art and "hell" not in art:
            return card
    return sources[0]


# Mythical bases drawn at half art size: amulets, belts, helms (circlets), rings, charms, jewels
# and the Dream Spirit.
SMALL_ART_BASES = {"Amulet", "Amulet (6 elemental variants)", "Mythical Hemp Band", "Mythical Diadem",
                   "Ring", "Large Charm", "Small Charm", "Jewel", "Mythical Dream Spirit"}


# Unique cards whose layout art is not the item's inventory art. The mod's uniques.json names the
# inventory sprite for these (Yello Jello: charm/charm_medium3), so the card uses that instead.
UNIQUE_ART_OVERRIDES = {
    "Yello Jello": "items\\misc\\charm\\charm_medium3",
}

# Uniques-page cards that use the warlock blade sprite (items/weapon/sword/warlock_blade.sprite).
# Only the uniques page uses these, so the Delirium runeword card keeps its own art.
WARLOCK_BLADE_ART = "items\\weapon\\sword\\warlock_blade"
UNIQUE_PAGE_ART_OVERRIDES = {
    "Ace in the Hole": WARLOCK_BLADE_ART,
    "The Penitent": WARLOCK_BLADE_ART,
    "Delirium": WARLOCK_BLADE_ART,
    "Wren's Reinforced Tech Boots": "items\\armor\\boot\\crystal_boots",
}

# setitems.txt cuts item names to 24 characters; these are the full names from item-names.json.
SET_ITEM_FULL_NAMES = {
    "Reaver of the Angel of A": "Reaver of the Angel of Annihilation",
    "Harbinger of Ruin's Merciless": "Harbinger of Ruin's Merciless Rule",
    "Harbinger of Ruin's Smoldering": "Harbinger of Ruin's Smoldering Gemstone",
    "Harbinger of Ruin's Inevitable": "Harbinger of Ruin's Inevitable Conquest",
    "Harbinger of Ruin's Soul": "Harbinger of Ruin's Soul Compendium",
}

# Gemword cards whose gem sprite is not in the mod: the Named card uses Smiiiff Stone's art instead.
MISSING_GEM_ART = {
    "items_misc_jewel_jewel": "items\\misc\\gem\\smiiiff_stone",
}


# Tier -> field in the mod's uniques.json / sets.json entries (normal / uber / ultra).
MOD_ART_TIER_FIELD = {"Normal": "normal", "Exceptional": "uber", "Nightmare": "uber",
                      "Elite": "ultra", "Hell": "ultra"}
# Folders the mod's inventory sprite paths are relative to (same search order as art-check).
MOD_ART_PREFIXES = ["items/weapon", "items/armor", "items/misc", "items/misc/amulet", "items/misc/ring"]


def mod_art_table(path):
    """Item name (letters and digits only, colour codes removed) -> {normal, uber, ultra} sprite paths."""
    table = {}
    for entry in json.loads(Path(path).read_text(encoding="utf-8")):
        for key, variants in entry.items():
            table.setdefault(re.sub(r"[^a-z0-9]", "", strip_color(key).lower()), variants)
    return table


def norm_name(text):
    return re.sub(r"\W+", "", text or "").lower()


class Builder:
    def __init__(self, strings, excel):
        self.strings = strings
        self.excel = excel          # excel_items.Tables or None
        self.pages = []
        self.images = {}            # key -> sprite rel path
        self.custom_art = {}        # key -> PNG in data/mythical-art (copied as-is)
        self.missing_strings = set()
        self.unmatched = []         # (page, title) with no excel row
        self.unmatched_bonus = set()  # sets.txt property codes with no card text
        self.sprite_index = {}      # lower-case sprite name -> path under the ui root, no extension
        self.unique_paths = {}      # unique key -> sprite name from uniques.json
        self.mod_uniques = {}       # item name -> variants, from the mod's uniques.json
        self.mod_sets = {}          # item name -> variants, from the mod's sets.json
        self.ui_roots = []          # folders the sprite paths are relative to

    def string_lines(self, key):
        text = self.strings.get(key)
        if text is None:
            self.missing_strings.add(key)
            return []
        return split_lines(text)

    @staticmethod
    def add_unique(items, value):
        if value not in items:
            items.append(value)

    def note_image(self, filename):
        key = image_key(filename)
        self.images.setdefault(key, sprite_rel_path(filename))
        return key

    # --- tables from a data file --------------------------------------------
    def build_data_tables(self, spec):
        """A page whose content is a JSON file (data/*.json): optional intro notes, then one
        section per entry, each with an optional table of [input, outcome] rows."""
        data = json.loads((PLUGIN_DIR / spec["source"]).read_text(encoding="utf-8"))
        entries = []
        if data.get("notes"):
            block = new_block(item=False)
            block["lines"] = [("T", note) for note in data["notes"]]
            entries.append({"title": spec["title"], "subtitle": "", "blocks": [block]})
        for section in data["sections"]:
            if spec.get("groupByItem"):
                entries.append({"title": section["title"], "subtitle": "",
                                "blocks": grouped_item_blocks(section)})
                continue
            block = new_block(item=False)
            block["lines"] = [("T", note) for note in section.get("notes", [])]
            if section.get("rows"):
                block["lines"].append(("C", TABLE_SEPARATOR.join(["Input(s)", "Possible Outcome(s)"])))
                for recipe_in, recipe_out in section["rows"]:
                    block["lines"].append(("R", TABLE_SEPARATOR.join([recipe_in, COLOR_BLUE + recipe_out])))
            entries.append({"title": section["title"], "subtitle": "", "blocks": [block]})
        self.pages.append({"spec": spec, "entries": entries})

    # --- cube recipe pages as tables ---------------------------------------
    def build_cube_tables(self, spec):
        sections = split_cube_sections(load_cube_page(spec["cubePage"]))

        entries = []
        for section in sections:
            if not section["notes"] and not section["rows"]:
                continue
            block = new_block(item=False)
            block["lines"] = [("T", note) for note in section["notes"]]
            if section["rows"]:
                block["lines"].append(("C", TABLE_SEPARATOR.join(["Ingredients", "Result"])))
                for recipe_in, recipe_out in section["rows"]:
                    block["lines"].append(("R", TABLE_SEPARATOR.join(
                        [tidy_recipe(recipe_in), COLOR_BLUE + tidy_recipe(recipe_out)])))
            entries.append({"title": section["title"], "subtitle": "", "blocks": [block]})
        self.pages.append({"spec": spec, "entries": entries})

    # --- guide pages -------------------------------------------------------
    def build_guide(self, spec, roots):
        """Each heading starts an entry; the text under it is one section block.

        With "itemPerButton" in the spec, every stat button is an item: it becomes
        its own item block with the image directly above it and the recipe text
        directly below it, so each image only shows its own stats."""
        per_button = bool(spec.get("itemPerButton"))
        entries = []
        current = None
        group = ""
        pending_image = None
        item_block = None

        def start(title, subtitle=""):
            nonlocal current, pending_image, item_block
            current = {"title": title, "subtitle": subtitle, "blocks": []}
            entries.append(current)
            pending_image = None
            item_block = None

        def section():
            if not current["blocks"]:
                current["blocks"].append(new_block())
            return current["blocks"][-1]

        for root in roots:
            for node, _ in walk(root):
                kind = node.get("type")
                name = node.get("name", "")
                text = fields(node).get("text")
                key = tooltip_key(node)
                if kind == "ScrollViewWidget":
                    group = name[len("group_"):] if name.startswith("group_") else ""
                if kind == "TextBoxWidget" and isinstance(text, str) and clean(text):
                    if name == "GroupHeader":
                        group = clean(text).strip(": ")
                        continue
                    if is_heading(node):
                        start(clean(text).strip(), group)
                        continue
                    if current is None:
                        start(clean(text).split("\n")[0], group)
                    target = item_block if (per_button and item_block is not None) else section()
                    for line in split_lines(text):
                        target["lines"].append(("T", line))
                    continue
                if per_button and kind == "ButtonWidget" and key and key.startswith("EsrWiki") and current is not None:
                    lines = self.string_lines(key)
                    item_block = new_block(item=True, title=strip_color(lines[0]) if lines else key,
                                           section=current["title"])
                    if pending_image:
                        item_block["images"].append(pending_image)
                    pending_image = None
                    item_block["lines"].extend(("S", line) for line in lines[1:])
                    current["blocks"].append(item_block)
                    continue
                if key and key.startswith("EsrWiki") and current is not None:
                    for line in self.string_lines(key):
                        section()["lines"].append(("S", line))
                    continue
                if kind == "ImageWidget" and current is not None:
                    filename = fields(node).get("filename")
                    if is_content_image(filename):
                        if per_button:
                            pending_image = self.note_image(filename)
                            item_block = None
                        else:
                            self.add_unique(section()["images"], self.note_image(filename))
        if spec.get("id") == "class-crafting":
            # Crafted rings and amulets are drawn at 50% like the other jewelry.
            for entry in entries:
                for block in entry["blocks"]:
                    if re.search(r"Crafted (Ring|Amulet)$", block["title"]):
                        block["tiny"] = True
        self.pages.append({"spec": spec, "entries": entries})

    # --- plain-text pages ----------------------------------------------------
    def build_endgame(self, spec):
        """A page written as plain text (spec "source"). ENTRY|title starts an entry; IMG|key adds
        the PNG data/images/<key>.png to the entry; H|, T| and S| add a sub-heading, a paragraph
        and a stat line. C| and R| are table header and row lines: cells are split by U+001F."""
        entries = []
        current = None
        for raw in (PLUGIN_DIR / spec["source"]).read_text(encoding="utf-8").splitlines():
            if not raw.strip() or raw.startswith("#"):
                continue
            kind, _, text = raw.partition("|")
            text = text.strip()
            if kind == "ENTRY":
                current = {"title": text, "subtitle": "", "blocks": [new_block(item=False)]}
                entries.append(current)
            elif current is None:
                raise SystemExit(f"{spec['id']}: text before the first ENTRY: {raw!r}")
            elif kind == "IMG":
                current["blocks"][-1]["images"].append(text)
                current["blocks"][-1]["original"] = True
            elif kind in ("H", "T", "S", "C", "R"):
                current["blocks"][-1]["lines"].append((kind, text))
            else:
                raise SystemExit(f"{spec['id']}: unknown line type {kind!r}")
        self.pages.append({"spec": spec, "entries": entries})

    # --- base item cards (armor and weapons, five per row) ------------------
    def build_base_item_cards(self, spec):
        """Armor and Weapons categories as item cards, five per row. Each card has its base type's
        icon, its name and tier, and the site's stats in the tooltip (blank columns left out).
        Armor cards are filtered by base type (Helm, Boots, ...), weapon cards by weapon family.
        Level is the site's Qlvl."""
        data = json.loads((PLUGIN_DIR / spec["source"]).read_text(encoding="utf-8"))
        item_art = {}
        art_file = PLUGIN_DIR / "data" / "base-item-art.txt"
        if art_file.exists():
            for line in art_file.read_text(encoding="utf-8").splitlines():
                if line.strip() and not line.startswith("#"):
                    code, _, sprite = line.partition("\t")
                    item_art[code.strip()] = sprite.strip()
        entries = []
        for kind, title in (("armor", "Armor"), ("weapons", "Weapons")):
            blocks = []
            for section in data.get(kind, []):
                headers = section["headers"]
                qlvl_at = headers.index("Qlvl") if "Qlvl" in headers else None
                seen = set()
                for item in section["items"]:
                    # The site lists some bases twice under different codes with the same name, tier and
                    # stats; they show as one card.
                    tier_name = self.excel.code_tier(item["code"]) if self.excel is not None else ""
                    key = (item["name"], tier_name, tuple(
                        tuple(cell for label, cell in zip(headers, row) if label not in ("Name", "Code"))
                        for row in item["rows"]))
                    if key in seen:
                        continue
                    seen.add(key)
                    block = new_block(item=True, title=item["name"], section=section["title"])
                    first = item["rows"][0]
                    block["fields"] = [(label, value) for label, value in zip(headers, first)
                                       if label not in ("Name", "Code") and value.strip()]
                    for row in item["rows"][1:]:
                        if row[2].strip():  # Missile or Two-Handed alternate: its damage
                            label = ALTERNATE_LABEL.get(row[1], row[1] or "Alternate")
                            block["fields"].append((label, row[2] + " (" + row[3] + ")"))
                    if qlvl_at is not None:
                        qlvl = first[qlvl_at]
                        block["level"] = int(qlvl) if str(qlvl).lstrip("-").isdigit() else 0
                    if self.excel is not None:
                        block["tier"] = self.excel.code_tier(item["code"])
                        block["subtitle"] = " ".join(x for x in (block["tier"], item["code"]) if x)
                        if kind == "weapons":
                            block["family"] = self.excel.code_family(item["code"])
                        if kind == "armor":
                            block["family"] = section["title"]
                    elif kind == "armor":
                        block["family"] = section["title"]
                    # Each base item's own art (data/base-item-art.txt); the generic base type icon otherwise.
                    art = item_art.get(item["code"])
                    if art is None and self.excel is not None:
                        stem = TYPE_ICON_STEM.get(self.excel.base_types.get(item["code"], ""), "")
                        art = self.sprite_index.get(stem) if stem else None
                    if art:
                        block["images"] = [self.note_image(art)]
                    blocks.append(block)
            entries.append({"title": title, "subtitle": "%d items" % len(blocks), "blocks": blocks,
                            "wide": False, "compact": False, "sortable": True, "fiveacross": True})
        self.pages.append({"spec": spec, "entries": entries})

    # --- base item tables (armor and weapons, one raw text table per item) ----
    def build_base_item_tables(self, spec):
        """Armor and Weapons categories laid out like the website. Each item is one block: its name
        is the title, and its table uses the site's column headers, with one row per site row (a
        second row for a Missile or Two-Handed alternate). Armor uses its base section (Helm, Boots,
        ...) for the Type filter; weapons use the weapon family. Level is the site's Qlvl."""
        data = json.loads((PLUGIN_DIR / spec["source"]).read_text(encoding="utf-8"))
        entries = []
        for kind, title in (("armor", "Armor"), ("weapons", "Weapons")):
            blocks = []
            for section in data.get(kind, []):
                headers = section["headers"]
                qlvl_at = headers.index("Qlvl") if "Qlvl" in headers else None
                for item in section["items"]:
                    block = new_block(item=False, title=item["name"], section=section["title"])
                    if qlvl_at is not None:
                        qlvl = item["rows"][0][qlvl_at]
                        block["level"] = int(qlvl) if str(qlvl).lstrip("-").isdigit() else 0
                    if self.excel is not None:
                        block["tier"] = self.excel.code_tier(item["code"])
                        block["subtitle"] = block["tier"]
                    if kind == "armor":
                        block["family"] = section["title"]
                    elif self.excel is not None:
                        block["family"] = self.excel.code_family(item["code"])
                    block["lines"] = [("C", TABLE_SEPARATOR.join(headers))]
                    # Blank cells hold a zero-width space: the writer trims whitespace at line ends, which
                    # would otherwise shift a blank first or last cell out of its column.
                    block["lines"] += [("R", TABLE_SEPARATOR.join(cell or "\u200b" for cell in row))
                                       for row in item["rows"]]
                    blocks.append(block)
            entries.append({"title": title, "subtitle": "%d items" % len(blocks), "blocks": blocks,
                            "wide": False, "compact": False, "sortable": True})
        self.pages.append({"spec": spec, "entries": entries})

    # --- base items (armor and weapons) -------------------------------------
    def build_base_items(self, spec):
        """Base items from data/base-items.json: one sortable entry per base section. Each item
        is titled by its name; its tier comes from the base code, and weapons get a family for
        the Type filter. Level is Qlvl (item level), since Req Lvl is 0 for most bases."""
        data = json.loads((PLUGIN_DIR / spec["source"]).read_text(encoding="utf-8"))
        entries = []
        for kind in ("armor", "weapons"):
            for section in data.get(kind, []):
                blocks = []
                for item in section["items"]:
                    block = new_block(item=True, title=item["name"], section=section["title"])
                    block["fields"] = [(label, value) for label, value in item["fields"]]
                    block["fields"] += [(label, value) for label, value in item.get("alt", [])]
                    qlvl = dict(item["fields"]).get("Qlvl", "")
                    block["level"] = int(qlvl) if str(qlvl).lstrip("-").isdigit() else 0
                    if self.excel is not None:
                        block["tier"] = self.excel.code_tier(item["code"])
                        block["subtitle"] = block["tier"]
                        if kind == "weapons":
                            block["family"] = self.excel.code_family(item["code"])
                    blocks.append(block)
                entries.append({"title": section["title"], "subtitle": "%d items" % len(blocks),
                                "blocks": blocks, "wide": False, "compact": False, "sortable": True})
        self.pages.append({"spec": spec, "entries": entries})

    # --- item cards (runewords, set items, uniques, gemwords) ---------------
    def read_card(self, node):
        parts = node["name"].split("_")
        group = parts[1] if len(parts) > 1 else ""
        subtitle = " ".join(parts[2:]) if self.subtitle_from_name else ""
        block = new_block(item=True, subtitle=subtitle)
        seen_keys = set()
        for child, _ in walk(node):
            cfields = fields(child)
            text = cfields.get("text")
            if child.get("type") == "TextBoxWidget" and isinstance(text, str) and clean(text):
                if not block["title"]:
                    # The overlay draws block titles as plain text, so colour codes would show.
                    block["title"] = strip_color(clean(text))
                else:
                    block["lines"].extend(("T", line) for line in split_lines(text))
            if child.get("type") == "ImageWidget" and is_content_image(cfields.get("filename")):
                self.add_unique(block["images"], self.note_image(cfields["filename"]))
            tkey = tooltip_key(child)
            # A runeword repeats a rune in its card (Conquest is Shi Shi O), so rune icons are not
            # de-duplicated. Other tooltips still are.
            if tkey and (tkey not in seen_keys or tkey.startswith(RUNE_PREFIX)):
                seen_keys.add(tkey)
                if tkey.startswith(RUNE_PREFIX):
                    label = self.string_lines(tkey)
                    rune_name = re.sub(r"\s*Rune$", "", strip_color(label[0])) if label else tkey
                    filename = cfields.get("filename") or DEFAULT_RUNE_SPRITE
                    code = tkey[len(RUNE_PREFIX):]
                    block["runes"].append((self.note_image(filename), rune_name, code))
                elif tkey.startswith(STAT_PREFIXES):
                    if tkey.startswith("EsrWikiSetBonus_"):
                        block["lines"].append(("H", "Set bonus"))
                    block["lines"].extend(("S", line) for line in self.string_lines(tkey))
        if not block["title"]:
            block["title"] = node["name"]
        return group, block

    def excel_fields(self, kind, block):
        if self.excel is None or not kind:
            return None
        if kind == "unique":
            return self.excel.unique_fields(block["title"])
        if kind == "set":
            return self.excel.set_fields(block["title"])
        if kind == "runeword":
            codes = [code for _, _, code in block["runes"]]
            return self.excel.runeword_fields(block["title"], codes, block.get("rwkey"))
        return None

    def add_socket_cells(self, block, spec):
        """Lists a runeword's sockets in order. Rune sockets keep their card icon; jewel and gem sockets
        get a generic icon with the label "Any Jewel" or "Any Gem"."""
        if spec.get("excel") != "runeword" or self.excel is None:
            return
        codes = [code for _, _, code in block["runes"]]
        cells = self.excel.runeword_cells(block["title"], codes, block.get("rwkey"))
        if not any(socket_label(cell) for cell in cells):
            return
        queues = {}
        for image, label, code in block["runes"]:
            queues.setdefault(code, []).append((image, label, code))
        ordered = []
        for cell in cells:
            label = socket_label(cell)
            if label:
                icon = JEWEL_ICON if label == "Any Jewel" else GEM_ICON
                ordered.append((self.note_image(icon), label, cell))
            elif queues.get(cell):
                ordered.append(queues[cell].pop(0))
        block["runes"] = ordered

    def ensure_category_cards(self, by_group):
        """A runeword with base types in several categories gets one card in each category, with that
        category's picture and only its own base types. Missing cards are copied from a sibling card;
        extra cards in a category are dropped."""
        if self.excel is None:
            return
        pairs_by_key = OrderedDict()
        for group, blocks in by_group.items():
            for block in blocks:
                if block.get("rwkey"):
                    pairs_by_key.setdefault(block["rwkey"], []).append((group, block))
        for key, pairs in pairs_by_key.items():
            row = self.excel.runewords_by_id.get(key)
            if row is None:
                continue
            names = []
            for i in range(1, 7):
                code = row.get("itype%d" % i, "")
                if code:
                    name = self.excel.item_types.get(code, code)
                    if name not in names:
                        names.append(name)
            wanted = []
            for name in names:
                if runeword_category(name) not in wanted:
                    wanted.append(runeword_category(name))
            if not wanted:
                continue
            for group, block in pairs:
                by_group[group] = [b for b in by_group[group] if b is not block]
            claimed = OrderedDict()
            surplus = []
            for group, block in pairs:
                if group in wanted and group not in claimed:
                    claimed[group] = block
                else:
                    surplus.append(block)
            for group in wanted:
                label = ", ".join(n for n in names if runeword_category(n) == group)
                group_names = [n for n in names if runeword_category(n) == group]
                if group in claimed:
                    block = claimed[group]
                else:
                    block = surplus.pop(0) if surplus else copy.deepcopy(pairs[0][1])
                    known = getattr(self, "rw_art", {}).get((key, group))
                    block["images"] = [self.note_image(known)] if known else [self.note_image(RUNEWORD_FALLBACK_IMAGE.get(
                        next((n for n in group_names if n in RUNEWORD_FALLBACK_IMAGE), RUNEWORD_GROUP_DEFAULT[group]),
                        RUNEWORD_FALLBACK_IMAGE[RUNEWORD_GROUP_DEFAULT[group]]))]
                block_image = block["images"][0] if block["images"] else ""
                is_weapon_art = block_image.startswith("items_weapon")
                if is_weapon_art != (group == "Weapon"):
                    block["images"] = [self.note_image(RUNEWORD_FALLBACK_IMAGE.get(
                        next((n for n in group_names if n in RUNEWORD_FALLBACK_IMAGE), RUNEWORD_GROUP_DEFAULT[group]),
                        RUNEWORD_FALLBACK_IMAGE[RUNEWORD_GROUP_DEFAULT[group]]))]
                set_card_type_line(block, label)
                by_group.setdefault(group, []).append(block)

    def split_shield_runeword(self, block, spec, by_group, group):
        """A runeword whose row lists a shield and a weapon (Spirit: Sword and Any Shield) gets one
        card under each: the weapon card with its weapon picture, and a shield card with a shield."""
        if spec.get("excel") != "runeword" or self.excel is None:
            return
        codes = [code for _, _, code in block["runes"]]
        types = self.excel.runeword_row_types(block["title"], codes, block.get("rwkey"))
        armor_like = ARMOR_BASE
        weapons = [name for name in types if name != "Any Shield" and not armor_like.search(name)]
        if "Any Shield" not in types or not weapons:
            return
        weapon_label = ", ".join(weapons)

        def set_type_line(target, label):
            target["lines"] = [(kind, ("\u00ffc0" + label) if kind == "S" and text.startswith("\u00ffc0") else text)
                               for kind, text in target["lines"]]
            target["fields"] = [(name, label if name == "Base types" else value) for name, value in target["fields"]]

        shield = copy.deepcopy(block)
        shield["images"] = [self.note_image(SHIELD_RUNEWORD_IMAGE)]
        set_type_line(shield, "Any Shield")
        by_group.setdefault("Shield", []).append(shield)

        block["images"] = [self.note_image(RUNEWORD_FALLBACK_IMAGE[weapons[0]])] if weapons[0] in RUNEWORD_FALLBACK_IMAGE else block["images"]
        set_type_line(block, weapon_label)
        if group != "Weapon":
            by_group[group].remove(block)
            by_group.setdefault("Weapon", []).append(block)

    def group_hireling_sets(self, blocks):
        """Hireling sets come in Normal, Nightmare and Hell versions with the same name. Each set
        becomes one section, and each version is labelled by difficulty. The levels in this mod are
        Normal 10-25, Nightmare 30-45 and Hell 50-65. A set bonus card (no Set field) is written once
        per difficulty its set has, with that difficulty's bonus values from sets.txt."""
        tiers_by_set = {}
        for block in blocks:
            set_name = next((value for label, value in block["fields"] if label == "Set"), None)
            if set_name is None:
                continue
            section = re.sub(r"-[A-Z]{1,2}$", "", set_name)
            level = required_level(block["lines"])
            block["tier"] = "Normal" if level < 30 else ("Nightmare" if level < 50 else "Hell")
            tiers_by_set.setdefault(section, set()).add(block["tier"])

        shard_cards = OrderedDict()
        for block in blocks:
            if not any(label == "Set" for label, _ in block["fields"]):
                shard_cards.setdefault(block["title"], []).append(block)

        kept = []
        for block in blocks:
            set_name = next((value for label, value in block["fields"] if label == "Set"), None)
            if set_name is not None:
                block["section"] = re.sub(r"-[A-Z]{1,2}$", "", set_name)
                block["subtitle"] = block["tier"]
                kept.append(block)
                continue
            title = block["title"]
            if title not in shard_cards:
                continue  # already written for this set
            for card in self.shard_cards_for(title, shard_cards.pop(title), tiers_by_set.get(title, ())):
                kept.append(card)
        return kept

    HIRELING_TIERS = ("Normal", "Nightmare", "Hell")
    RESIST_NAMES = {"res-cold/lvl": "Cold", "res-fire/lvl": "Fire", "res-ltng/lvl": "Lightning", "res-pois/lvl": "Poison"}

    def hireling_lines(self, code, param, low, high):
        """Display lines for one setitems.txt property of a hireling item."""
        if code in self.RESIST_NAMES:
            value = int(param or 0) / 8.0
            text = ("%g" % value)
            return ["%sc3%s Resist +%s%% (Based on Character Level)" % ("\u00ff", self.RESIST_NAMES[code], text)]
        if self.line_generator is None:
            self.line_generator = runeword_gen.Generator(self.excel.folder, getattr(self, "strings_folder", None))
        lines = self.line_generator.lines(code, param, low, high)
        if lines is None:
            self.unmatched_bonus.add(code)
            return []
        return list(lines)

    def build_hireling_blocks(self, old_blocks):
        """The Hireling category, built from setitems.txt (the mercenary-only rows, set names ending -N,
        -NM and -H) and sets.txt. Each difficulty is its own group of sets: Normal, then Nightmare, then
        Hell. Card art comes from the mod's sets.json and
        the set bonus cards keep the art of the layout's cards. Laid out like the Warlock sets: a header for
        each set (with its difficulty and level), its pieces, then its set bonus card. The difficulty
        filter is kept."""
        self.line_generator = getattr(self, "line_generator", None)
        shard_sources = OrderedDict()
        for block in old_blocks:
            if not any(label == "Set" for label, _ in block["fields"]):
                shard_sources.setdefault(block["title"], []).append(block)

        old_art = {(block["title"], block.get("tier", "")): block["images"] for block in old_blocks if block["images"]}
        rows = self.excel.hireling_rows()
        set_order = []
        for _, set_name, _ in rows:
            if set_name not in set_order:
                set_order.append(set_name)
        pieces = {}
        for row, set_name, tier in rows:
            pieces.setdefault((set_name, tier), []).append(row)

        out = []
        for tier_index, tier in enumerate(self.HIRELING_TIERS):
            for set_index, set_name in enumerate(set_order):
                members = pieces.get((set_name, tier), [])
                if not members:
                    continue
                level = int(members[0].get("lvl req", "0") or 0)
                # Like the Warlock sets: a header for each set, its pieces under it, then its bonus card.
                section = "%s (%s, Lvl %d)" % (set_name, tier, level)
                for piece_index, row in enumerate(members):
                    block = new_block(True, re.sub(r"\s+(Normal|Night\w*|Hell)$", "", row["index"].strip()), tier,
                                      section)
                    block["tier"] = tier
                    block["level"] = int(row.get("lvl req", "0") or 0)
                    block["fields"] = [("Set", set_name), ("Base", row.get("*ItemName", "")),
                                       ("Required Level", row.get("lvl req", "")),
                                       ("Item Level", row.get("lvl", "")),
                                       ("Set pieces", str(len(members)))]
                    art = self.mod_inventory_art("set", row["index"].strip(), tier)
                    if art:
                        block["images"] = [self.note_image(art)]
                    elif old_art.get((block["title"], tier)):
                        block["images"] = list(old_art[(block["title"], tier)])
                    else:
                        # No art in the mod's sets.json: the base item's own inventory icon.
                        base_type = self.excel.base_types.get(row.get("item", "").strip(), "")
                        for stem in (unique_key(row.get("*ItemName", "")), base_type, TYPE_ICON_STEM.get(base_type, "")):
                            base_art = self.sprite_index.get(stem.lower()) if stem else None
                            if base_art:
                                block["images"] = [self.note_image(base_art)]
                                break
                    lines = [("S", TITLE_COLOR + row["index"].strip())]
                    for i in range(1, 10):
                        code = row.get("prop%d" % i, "").strip()
                        if code:
                            lines += [("S", text) for text in self.hireling_lines(
                                code, row.get("par%d" % i, "").strip(), row.get("min%d" % i, "").strip(),
                                row.get("max%d" % i, "").strip())]
                    for i in range(1, 6):
                        bonus = []
                        for suffix in ("a", "b"):
                            code = row.get("aprop%d%s" % (i, suffix), "").strip()
                            if code:
                                bonus += self.hireling_lines(code, row.get("apar%d%s" % (i, suffix), "").strip(),
                                                             row.get("amin%d%s" % (i, suffix), "").strip(),
                                                             row.get("amax%d%s" % (i, suffix), "").strip())
                        if bonus:
                            lines.append(("S", TITLE_COLOR + "(%d Items)" % (i + 1)))
                            lines += [("S", TITLE_COLOR + re.sub("^\u00ffc.", "", text)) for text in bonus]
                    lines.append(("S", "%sc5Required Level: %s" % ("\u00ff", row.get("lvl req", ""))))
                    block["lines"] = lines
                    block["_order"] = (tier_index, set_index, 0, piece_index)
                    out.append(block)
                for card in self.shard_cards_for(set_name, shard_sources.get(set_name, []), (tier,)):
                    card["level"] = level
                    card["section"] = section
                    card["fields"] = [("Set", set_name), ("Set pieces", str(len(members)))]
                    card["_order"] = (tier_index, set_index, 1, 0)
                    out.append(card)
        out.sort(key=lambda b: b["_order"])
        for block in out:
            del block["_order"]
        return out

    CLASS_SUFFIX = re.compile(r"-(Ama|Ass|Bar|Dru|Nec|Pal|Sor|War)$")

    def set_gen(self, code, par, low, high):
        """Lines for one set property: [] for a hidden one, None when the text cannot be written here."""
        if code in self.RESIST_NAMES:
            return self.hireling_lines(code, par, low, high)
        if self.line_generator is None:
            self.line_generator = runeword_gen.Generator(self.excel.folder, getattr(self, "strings_folder", None))
        if code in ("tinkerflag2", "vit-h2"):
            return []
        if code in runeword_gen.DESCRIPTION_CODES:
            return None
        got = self.line_generator.lines(code, par, low, high)
        return None if got is None else [t.replace("--", "-") for t in got]

    def rebuild_set_blocks(self, blocks, report):
        """Non-hireling set cards, rebuilt from setitems.txt and sets.txt. Same layout as before: a header
        per set, its pieces, then its set bonus card. A card with a property the generator cannot write
        keeps its old lines for that part."""
        self.line_generator = getattr(self, "line_generator", None)
        rows = [r for r in self.excel.set_table if not re.search(r"-(N|NM|H)$", r.get("set", "").strip())]
        by_norm = {}
        for row in rows:
            by_norm.setdefault(norm_name(row["index"]), row)
        keys = sorted(by_norm, key=len, reverse=True)
        sets_by_norm = {}
        for row in self.excel.set_bonuses.values():
            name = row["name"].strip()
            if not re.search(r"-(N|NM|H)$", name):
                sets_by_norm[norm_name(self.CLASS_SUFFIX.sub("", name))] = row

        def find_row(title, set_name):
            key = norm_name(title)
            row = by_norm.get(key)
            if row is not None:
                return row
            for k in keys:   # setitems.txt cuts long names at 24 characters
                if key.startswith(k) and by_norm[k].get("set", "").strip() == set_name:
                    return by_norm[k]
            return None

        def item_lines(row):
            lines = []
            for i in range(1, 10):
                code = row.get("prop%d" % i, "").strip()
                if code:
                    got = self.set_gen(code, row.get("par%d" % i, "").strip(), row.get("min%d" % i, "").strip(),
                                       row.get("max%d" % i, "").strip())
                    if got is None:
                        return None
                    lines += [("S", t) for t in got]
            for i in range(1, 6):
                bonus = []
                for suffix in ("a", "b"):
                    code = row.get("aprop%d%s" % (i, suffix), "").strip()
                    if code:
                        got = self.set_gen(code, row.get("apar%d%s" % (i, suffix), "").strip(),
                                           row.get("amin%d%s" % (i, suffix), "").strip(),
                                           row.get("amax%d%s" % (i, suffix), "").strip())
                        if got is None:
                            return None
                        bonus += got
                if bonus:
                    lines.append(("S", TITLE_COLOR + "(%d Items)" % (i + 1)))
                    lines += [("S", TITLE_COLOR + re.sub("^\u00ffc.", "", t)) for t in bonus]
            return lines

        def bonus_part(row, columns, param, low, high):
            part = []
            for column in columns:
                code = row.get(column, "").strip()
                if not code:
                    continue
                n = column[5:]
                got = self.set_gen(code, row.get(param + n, "").strip(), row.get(low + n, "").strip(),
                                   row.get(high + n, "").strip() or row.get(low + n, "").strip())
                if got is None:
                    return None
                part += got
            return part

        items = []
        bonus_cards = []
        for block in blocks:
            is_bonus = any(kind == "H" and text == "Set bonus" for kind, text in block["lines"])
            is_item = not is_bonus and any(label == "Base" for label, _ in block["fields"]) \
                and any(label == "Set" for label, _ in block["fields"])
            if is_item:
                items.append(block)
            else:
                bonus_cards.append(block)

        sections = OrderedDict()
        for block in items:
            raw = next(v for l, v in block["fields"] if l == "Set")
            row = find_row(block["title"], raw)
            if row is None:
                report.append("no setitems row: %s" % block["title"])
                sections.setdefault(raw, []).append(block)
                continue
            new = item_lines(row)
            if new is None:
                report.append("kept old lines (unsupported code): %s" % block["title"])
            else:
                head = [line for line in block["lines"][:1] if line[0] == "S"]
                new = head + new + [("S", "%sc5Required Level: %s" % ("\u00ff", row.get("lvl req", "")))]
                if [t for _, t in new] != [t for _, t in block["lines"]]:
                    report.append("changed: %s" % block["title"])
                block["lines"] = new
            set_name = row["set"].strip()
            size = sum(1 for r in rows if r.get("set", "").strip() == set_name)
            display = self.CLASS_SUFFIX.sub("", set_name)
            block["fields"] = [("Set", display), ("Base", row.get("*ItemName", "")),
                               ("Required Level", row.get("lvl req", "")), ("Set pieces", str(size))]
            block["level"] = int(row.get("lvl req", "0") or 0)
            block["section"] = display
            sections.setdefault(display, []).append(block)

        used = set()
        for card in bonus_cards:
            name = next((strip_color(t) for k, t in card["lines"] if k == "S" and t.startswith(TITLE_COLOR)), card["title"])
            key = norm_name(self.CLASS_SUFFIX.sub("", name))
            row = sets_by_norm.get(key)
            section = next((sec for sec in sections if norm_name(sec) == key), None)
            if section is None:
                report.append("bonus card without pieces: %s" % card["title"])
                continue
            used.add(section)
            card["fields"] = []
            card["section"] = section
            if row is None:
                report.append("no sets.txt row: %s" % name)
                sections[section].append(card)
                continue
            partial, ok = [], True
            for n in range(2, 6):
                part = bonus_part(row, ["PCode%da" % n, "PCode%db" % n], "PParam", "PMin", "PMax")
                if part is None:
                    ok = False
                    break
                if part:
                    partial += [("S", COLOR_BONUS_LINE + re.sub("^\u00ffc.", "", t)) for t in part]
                    partial.append(("S", COLOR_GOLD_LINE + "(%d items)" % n))
            full = bonus_part(row, ["FCode%d" % i for i in range(1, 9)], "FParam", "FMin", "FMax")
            old = card["lines"]
            name_line = next((l for l in old if l[0] == "S" and l[1].startswith(TITLE_COLOR)), ("S", TITLE_COLOR + name))
            cut = next((i for i, l in enumerate(old) if "Full Set Bonus" in l[1]), len(old))
            if not ok or full is None:
                report.append("bonus card kept (unsupported code): %s" % name)
                if ok and full is None:
                    new = [("H", "Set bonus"), name_line, ("S", COLOR_GOLD_LINE + "Partial Set Bonus")] + partial + old[cut:]
                    card["lines"] = new
                    if [t for _, t in new] != [t for _, t in old]:
                        report.append("bonus card partly changed: %s" % name)
            else:
                new = [("H", "Set bonus"), name_line, ("S", COLOR_GOLD_LINE + "Partial Set Bonus")] + partial
                new += [("S", COLOR_GOLD_LINE + "Full Set Bonus")]
                new += [("S", COLOR_BONUS_LINE + re.sub("^\u00ffc.", "", t)) for t in full]
                if [t for _, t in new] != [t for _, t in old]:
                    report.append("bonus card changed: %s" % name)
                card["lines"] = new
            sections[section].append(card)
        out = []
        for section_blocks in sections.values():
            out += section_blocks
        return out

    # Uniques whose card carries the mod's spelling of the name while the website card (with the orange
    # description from the client's mythicaldesc) uses another: mod name -> website name.
    DESCRIPTION_TWINS = {
        "Vizjeris' Knowledge": "Vizjereis' Knowledge", "Godstrike Arch": "Goldstrike Arch",
        "The Reedeemer": "The Redeemer", "Death Game": "Death's Game", "Peasent Crown": "Peasant Crown",
        "Cerebus": "Cerebus' Bite", "Steel Carapice": "Steel Carapace", "Kerke's Sanctuary": "Gerke's Sanctuary",
        "Radimant's Sphere": "Radament's Sphere", "Darkforge Spawn": "Darkforce Spawn",
        "Ars Al'Diablolos": "Ars Al'Diabolos", "Thudergod's Vigor": "Thundergod's Vigor",
        "Verdugo's Hearty Cord": "Verdungo's Hearty Cord", "Guardian Angel": "Guardian Angel",
        # Cards still titled with uniqueitems.txt's index name; item-names.json gives the name shown in game.
        "Cutthroat1": "Bartuc's Cut-Throat", "Fathom": "Death's Fathom", "Eschuta's temper": "Eschuta's Temper",
        "Ironward": "Astreon's Iron Ward", "Souldrain": "Soul Drainer",
    }

    def item_display_names(self):
        """item-names.json: the name a unique is shown with in game, by its uniqueitems.txt index."""
        display = {}
        folder = getattr(self, "strings_folder", None)
        if folder and (Path(folder) / "item-names.json").exists():
            for entry in json.loads((Path(folder) / "item-names.json").read_text(encoding="utf-8-sig")):
                display[str(entry.get("Key", ""))] = str(entry.get("enUS", ""))
        return display

    def drop_index_named_duplicates(self, by_group, mythical_group=None):
        """A unique can have two cards: one titled with its uniqueitems.txt index name ("Cutthroat1") and one with
        the name shown in game ("Bartuc's Cut-Throat"). The card with the in-game name stays. Only cards in the
        same group with the same base item count as twins."""
        display = self.item_display_names()
        self.duplicate_report = []
        for group, blocks in by_group.items():
            if group == mythical_group:
                continue
            by_title = {}
            for block in blocks:
                by_title.setdefault(excel_items.norm(strip_color(block["title"])), []).append(block)
            drop = set()
            for block in blocks:
                title = strip_color(block["title"]).strip()
                row = self.excel.unique_row(title)
                index = (row.get("index") or "").strip() if row else ""
                if index.lower().endswith("_lod"):
                    continue   # the Lord of Destruction version of a unique is its own card
                shown = display.get(index) if row else None
                if not shown or excel_items.norm(shown) == excel_items.norm(title):
                    continue
                base = next((v for l, v in block["fields"] if l == "Base"), "").lower()
                for twin in by_title.get(excel_items.norm(shown), []):
                    twin_base = next((v for l, v in twin["fields"] if l == "Base"), "").lower()
                    if twin is not block and (not base or not twin_base or base == twin_base):
                        drop.add(id(block))
                        self.duplicate_report.append("%s -> %s" % (title, shown))
                        break
            by_group[group] = [block for block in blocks if id(block) not in drop]

    def mod_descriptions(self):
        """The mod's own orange description strings: <name>desc keys of item-modifiers.json and skills.json,
        stored in reverse line order, "ÿc@" being orange. Returns {key without "desc": [lines top to bottom]}."""
        found = {}
        folder = getattr(self, "strings_folder", None)
        if not folder:
            return found
        for name in ("item-modifiers.json", "skills.json"):
            path = Path(folder) / name
            if not path.exists():
                continue
            for entry in json.loads(path.read_text(encoding="utf-8-sig")):
                key = str(entry.get("Key", ""))
                if key.lower().endswith("desc") and not key.lower().endswith("loddesc"):
                    lines = [re.sub("\u00ffc.", "", re.sub("\u00ffc6[^\u00ff]*", "", ln)).replace("%%", "%").strip()
                             for ln in str(entry.get("enUS", "")).split("\n")]
                    found[re.sub(r"[^a-z0-9]", "", key.lower())[:-4]] = [ln for ln in reversed(lines) if ln]
        return found

    def stat_literals(self):
        """Orange lines that are stat texts, not descriptions: the mod's non-description strings, word for word."""
        found = set()
        folder = getattr(self, "strings_folder", None)
        if not folder:
            return found
        for name in ("item-modifiers.json", "skills.json"):
            path = Path(folder) / name
            if not path.exists():
                continue
            for entry in json.loads(path.read_text(encoding="utf-8-sig")):
                if str(entry.get("Key", "")).lower().endswith("desc"):
                    continue
                for line in str(entry.get("enUS", "")).split("\n"):
                    found.add(re.sub("\u00ffc.", "", line).strip())
        return found

    @staticmethod
    def description_stems(text):
        dropped = re.sub(r"(?i)['\u2019]s\b|s['\u2019](?=\s|$)", "", text)
        out = []
        for variant in (text, dropped):
            plain = re.sub(r"[^a-z0-9]", "", variant.lower())
            for stem in (plain, plain[3:] if plain.startswith("the") else plain):
                if stem and stem not in out:
                    out.append(stem)
        return out

    def find_description(self, descriptions, names):
        """Key of the mod's description string for a unique: by name (with and without possessives), by a
        key that is the start of the name, then by a close spelling (the mod's keys have typos)."""
        for name in names:
            for stem in self.description_stems(name):
                if stem in descriptions:
                    return stem
        for name in names:
            for stem in self.description_stems(name):
                hits = [key for key in descriptions if len(key) >= 6 and stem.startswith(key)]
                if hits:
                    return max(hits, key=len)
        import difflib
        for name in names:
            close = difflib.get_close_matches(self.description_stems(name)[-1], list(descriptions), n=1, cutoff=0.88)
            if close:
                return close[0]
        return None

    def add_unique_descriptions(self, by_group):
        """Orange description lines of a unique whose row has mythicaldesc. The text is the mod's own
        (<name>desc in item-modifiers.json). Cards the mod has no string for keep their existing orange lines,
        or take them from their website twin by name."""
        descriptions = self.mod_descriptions()
        display = {}
        folder = getattr(self, "strings_folder", None)
        if folder and (Path(folder) / "item-names.json").exists():
            for entry in json.loads((Path(folder) / "item-names.json").read_text(encoding="utf-8-sig")):
                display[str(entry.get("Key", ""))] = str(entry.get("enUS", ""))
        orange = {}
        for key in self.strings:
            if key.startswith("EsrWikiUnique_"):
                lines = self.string_lines(key)
                if lines:
                    found = [line for line in lines[1:] if line.startswith("\u00ffc8")]
                    if found:
                        orange[excel_items.norm(strip_color(lines[0]).strip())] = found
        literals = self.stat_literals()

        def is_stat(text):
            plain = re.sub("\u00ffc.", "", text).strip()
            return bool(re.match(r"[+-]\d", plain)) or plain in literals

        self.description_report = []
        for group, blocks in by_group.items():
            for block in blocks:
                title = strip_color(block["title"]).strip()
                row = self.excel.unique_row(title)
                if row is None or not any((row.get("prop%d" % i) or "") == "mythicaldesc" for i in range(1, 13)):
                    continue
                index = (row.get("index") or "").strip()
                key = self.find_description(descriptions, [title, index, display.get(index, "")])
                head = 1 if block["lines"] and block["lines"][0][0] == "S" and block["lines"][0][1].startswith("\u00ffc4") else 0
                # The description is the run of orange lines under the name; orange stat lines further down stay.
                run = 0
                while head + run < len(block["lines"]) and block["lines"][head + run][1].startswith("\u00ffc8"):
                    run += 1
                kept = [line for line in block["lines"][head:head + run] if is_stat(line[1])]
                twin = self.DESCRIPTION_TWINS.get(title)
                twin_lines = orange.get(excel_items.norm(twin)) if twin else None
                # A card built from the string table lacks the website's orange stat lines (Total Resist and the like).
                kept += [("S", line) for line in (twin_lines or []) if is_stat(line)
                         and ("S", line) not in kept and ("S", line) not in block["lines"]]
                rest = kept + block["lines"][head + run:]
                if key:
                    new = [("S", "\u00ffc8" + text) for text in descriptions[key]]
                else:
                    existing = [line for line in block["lines"][head:head + run] if not is_stat(line[1])]
                    new = existing or [("S", line) for line in (twin_lines or []) if not is_stat(line)]
                    if not new:
                        self.description_report.append("no description text: %s" % title)
                block["lines"] = block["lines"][:head] + new + rest

    def rebuild_unique_lines(self, blocks, report):
        """Stat lines of unique cards, written from uniqueitems.txt (the mod's own properties) instead of
        the layout's text. A card with a property the generator cannot write keeps its old lines."""
        self.line_generator = getattr(self, "line_generator", None)
        for block in blocks:
            row = self.excel.unique_row(strip_color(block["title"]))
            if row is None:
                report.append("no row: %s" % block["title"])
                continue
            new = []
            for i in range(1, 13):
                code = (row.get("prop%d" % i) or "").strip()
                if not code:
                    continue
                got = self.set_gen(code, (row.get("par%d" % i) or "").strip(), (row.get("min%d" % i) or "").strip(),
                                   (row.get("max%d" % i) or "").strip())
                if got is None:
                    new = None
                    break
                new += [("S", text) for text in got]
            if new is None:
                report.append("kept old lines (unsupported code): %s" % block["title"])
                continue
            head = [line for line in block["lines"][:1] if line[0] == "S"]
            lines = head + new
            if [t for _, t in lines] != [t for _, t in block["lines"]]:
                report.append("changed: %s" % block["title"])
            block["lines"] = lines

    def shard_cards_for(self, title, sources, tiers):
        """One set bonus card per difficulty. The card art comes from the source card for that
        difficulty when there is one. The bonus lines come from sets.txt when they are there."""
        cards = []
        for tier in [t for t in ("Normal", "Nightmare", "Hell") if t in tiers] or [""]:
            source = pick_shard_source(sources, tier)
            lines = list(source["lines"])
            bonus = None
            if self.excel is not None and tier:
                bonus = self.excel.set_bonus(title, TIER_SUFFIX[tier])
            if bonus:
                name_line = next((text for kind, text in source["lines"]
                                  if kind == "S" and text.startswith(TITLE_COLOR)), TITLE_COLOR + title)
                lines = [("H", "Set bonus"), ("S", name_line), ("S", COLOR_GOLD_LINE + "Partial Set Bonus")]
                lines += [("S", COLOR_BONUS_LINE + text) for text in bonus_texts(bonus["partial"], self.unmatched_bonus)]
                lines += [("S", COLOR_GOLD_LINE + "(2 items)"), ("S", COLOR_GOLD_LINE + "Full Set Bonus")]
                lines += [("S", COLOR_BONUS_LINE + text) for text in bonus_texts(bonus["full"], self.unmatched_bonus)]
            cards.append(dict(source, images=list(source["images"]), fields=list(source["fields"]),
                              lines=lines, runes=list(source["runes"]),
                              section=title, tier=tier, subtitle=tier))
        return cards

    def runeword_nodes(self, roots):
        """One card widget per runeword that has card text in the string table and a runes.txt row.
        The game layout is not read. Card art, titles and order come from data/runeword-art.json
        (the art was built to vary within each item category so the cards do not all look alike);
        rune icons come from the rune sprite files."""
        sprites = {}
        info = json.loads((OVERLAY_DIR / "data" / "runeword-art.json").read_text(encoding="utf-8"))
        self.rw_art = {(key, category): filename for key, by_category in info["art"].items()
                       for category, filename in by_category.items()}
        order = {key: index for index, key in enumerate(info["order"])}
        titles = info["titles"]
        keys = [k[len("EsrWikiRuneword_"):] for k in self.strings if k.startswith("EsrWikiRuneword_")]
        keys.sort(key=lambda k: (order.get(k, len(order)), int(re.sub(r"\D", "", k) or 0)))
        # Gemwords share runes.txt with runewords but belong on the Gemwords page only.
        gemwords = {norm_name(k[len("EsrWikiGemword_"):]) for k in self.strings if k.startswith("EsrWikiGemword_")}
        nodes = []
        for number, rwkey in enumerate(keys):
            row = self.excel.runewords_by_id.get(rwkey)
            if row is not None and norm_name(row.get("*Rune Name", "")) in gemwords:
                continue
            lines = self.string_lines("EsrWikiRuneword_" + rwkey)
            if row is None or not lines:
                continue
            first = self.excel.item_types.get(row.get("itype1", ""), "")
            category = runeword_category(first)
            children = [
                {"type": "TextBoxWidget", "name": "text", "fields": {"text": titles.get(rwkey, lines[0])}},
                {"type": "ButtonWidget", "name": "button",
                 "fields": {"tooltipString": "@esr-wiki:EsrWikiRuneword_" + rwkey}},
            ]
            art = self.rw_art.get((rwkey, category))
            if art:
                children.insert(0, {"type": "ImageWidget", "name": "image", "fields": {"filename": art}})
            for i in range(1, 7):
                code = (row.get("Rune%d" % i) or "").strip()
                if code:
                    label = self.string_lines(RUNE_PREFIX + code)
                    stem = re.sub(r"\W+", "_", re.sub(r"\s*Rune$", "", strip_color(label[0]) if label else "")).strip("_").lower()
                    if code not in sprites and self.sprite_index.get("%s_rune" % stem):
                        sprites[code] = "items\\misc\\rune\\%s_rune" % stem
                    children.append({"type": "ButtonWidget", "name": "rune_%d" % (i - 1), "fields": {
                        "filename": sprites.get(code, DEFAULT_RUNE_SPRITE),
                        "tooltipString": "@esr-wiki:" + RUNE_PREFIX + code}})
            nodes.append({"type": "Widget", "name": "card_%s_9_%d" % (category, number),
                          "synthetic": rwkey, "children": children})
        return nodes

    def build_cards(self, spec, roots):
        pattern = re.compile(spec["cardPattern"])
        self.subtitle_from_name = bool(spec.get("subtitleFromNameParts"))
        categories = spec.get("categories") or {}
        # Groups in pages.json order first, then any others in the order they appear.
        by_group = OrderedDict((group, []) for group in categories)
        def card_nodes():
            if spec.get("excel") == "runeword" and self.excel is not None:
                # The page is built from runes.txt and the string table. The layout only gives art.
                for node in self.runeword_nodes(roots):
                    yield node
                return
            for root in roots:
                for node, _ in walk(root):
                    yield node

        if True:
            for node in card_nodes():
                if node.get("type") != "Widget" or not pattern.search(node.get("name", "")):
                    continue
                group, block = self.read_card(node)
                if node.get("synthetic") and not block["images"]:
                    row = self.excel.runewords_by_id.get(node["synthetic"]) if self.excel is not None else None
                    first = self.excel.item_types.get(row.get("itype1", ""), "") if row else ""
                    block["images"] = [self.note_image(RUNEWORD_FALLBACK_IMAGE.get(
                        first, RUNEWORD_FALLBACK_IMAGE[RUNEWORD_GROUP_DEFAULT.get(group, "Weapon")]))]
                rw = re.search(r"EsrWikiRuneword_(Runeword\d+)", json.dumps(node))
                if rw:
                    block["rwkey"] = rw.group(1)
                if spec["id"] == "gemwords" and group == "Named":
                    block["images"] = [self.note_image(MISSING_GEM_ART[key]) if key in MISSING_GEM_ART
                                       else key for key in block["images"]]
                override = UNIQUE_ART_OVERRIDES.get(strip_color(block["title"]))
                if spec.get("excel") == "unique":
                    override = UNIQUE_PAGE_ART_OVERRIDES.get(strip_color(block["title"])) or override
                if override:
                    block["images"] = [self.note_image(override)]
                if spec.get("stringUniques") and self.excel is not None and not block["images"]:
                    art = self.unique_art(strip_color(block["title"]))
                    if art:
                        self.add_unique(block["images"], self.note_image(art))
                by_group.setdefault(group, []).append(block)
                if self.excel is not None and spec.get("excel"):
                    matched = self.excel_fields(spec["excel"], block)
                    if matched is None:
                        self.unmatched.append((spec["id"], block["title"]))
                    else:
                        block["fields"].extend(matched)
                        # A runeword's section follows its base type in runes.txt. The layout can
                        # place a missile runeword in a shield slot, so move it to the Weapon group.
                        base_types = [value for label, value in matched if label == "Base types"]
                        if spec.get("excel") == "runeword" and base_types == ["Missile Weapon"] and group != "Weapon":
                            by_group[group].remove(block)
                            group = "Weapon"
                            by_group.setdefault(group, []).append(block)
                        # A weapon runeword the layout placed in an armor slot (Torso, Helms...) belongs
                        # in the Weapon group. Skip it when that group already has the same runeword.
                        base_value = base_types[0] if base_types else ""
                        if (spec.get("excel") == "runeword" and base_value and group != "Weapon"
                                and "Any Shield" not in base_value
                                and base_family(base_value.split(",")[0].strip()) == "weapon"):
                            weapon_cards = by_group.setdefault("Weapon", [])
                            by_group[group].remove(block)
                            if not any(other.get("rwkey") == block.get("rwkey") for other in weapon_cards):
                                weapon_cards.append(block)
                            group = "Weapon"
                        for label, value in matched:
                            if label != "Base types" or not value:
                                continue
                            first = value.split(",")[0].strip()
                            family = base_family(first)
                            shown = block["images"][0] if block["images"] else ""
                            shown_family = "armor" if shown.startswith("items_armor") else "weapon" if shown.startswith("items_weapon") else None
                            if family and shown_family and family != shown_family and first in RUNEWORD_FALLBACK_IMAGE:
                                block["images"] = [self.note_image(RUNEWORD_FALLBACK_IMAGE[first])]
                            break
                if spec.get("excel") == "runeword":
                    count = len(block["runes"])
                    if self.excel is not None:
                        count = self.excel.runeword_socket_count(block["title"], [c for _, _, c in block["runes"]], block.get("rwkey")) or count
                    block["fields"].insert(0, ("Sockets", str(count)))
                    block["section"] = "%d Socket%s" % (count, "" if count == 1 else "s")
                    self.split_shield_runeword(block, spec, by_group, group)
                    self.add_socket_cells(block, spec)
                if spec.get("excel") == "set":
                    for label, value in block["fields"]:
                        if label == "Set":
                            block["section"] = value

        if spec.get("excel") == "set":
            for blocks in by_group.values():
                for block in blocks:
                    block["title"] = SET_ITEM_FULL_NAMES.get(block["title"], block["title"])
        if spec.get("excel") == "runeword":
            self.ensure_category_cards(by_group)
        if spec.get("excel") == "set" and self.excel is not None and self.excel.hireling_rows():
            self.set_report = []
            for group in list(by_group):
                if group != spec.get("difficultyGroup"):
                    by_group[group] = self.rebuild_set_blocks(by_group[group], self.set_report)
        if spec.get("difficultyGroup") in by_group:
            if self.excel is not None and self.excel.hireling_rows():
                by_group[spec["difficultyGroup"]] = self.build_hireling_blocks(by_group[spec["difficultyGroup"]])
            else:
                by_group[spec["difficultyGroup"]] = self.group_hireling_sets(by_group[spec["difficultyGroup"]])
        if spec.get("mythicals"):
            self.add_mythicals(by_group, spec)
        if spec.get("stringUniques") and self.excel is not None:
            self.add_string_uniques(by_group)
        if spec.get("excel") == "unique" and self.excel is not None:
            self.drop_index_named_duplicates(by_group, spec.get("mythicalGroup"))
        if spec.get("excel") == "unique":
            # Runs after the string-table uniques are added, which take a generic icon otherwise.
            for blocks in by_group.values():
                for block in blocks:
                    page_art = UNIQUE_PAGE_ART_OVERRIDES.get(strip_color(block["title"]))
                    if page_art:
                        block["images"] = [self.note_image(page_art)]
        if spec.get("excel") == "unique" and self.excel is not None:
            for group, blocks in by_group.items():
                for block in blocks:
                    if self.excel.coupon_only(strip_color(block["title"])):
                        block["tag"] = "(Coupon only)"
                    elif group != spec.get("mythicalGroup"):
                        info = self.excel.gamble_info(strip_color(block["title"]))
                        if info and info[0]:
                            # "\\n" is a line break inside the TAG field (the file is one line per record).
                            block["tag"] = "Gamble Item:\\n%s - ilvl: %s" % info
        if spec.get("excel") == "unique" and self.excel is not None:
            self.add_unique_descriptions(by_group)
        if spec.get("excel") == "unique" and self.excel is not None:
            self.unique_report = []
            for group in ("Jewel",):
                if group in by_group:
                    self.rebuild_unique_lines(by_group[group], self.unique_report)
        if spec.get("excel") in ("unique", "set") and self.excel is not None:
            self.apply_mod_inventory_art(spec["excel"], by_group, spec.get("mythicalGroup"))
            self.mark_tiny_art(spec["excel"], by_group, spec.get("mythicalGroup"))

        if spec.get("sortable") and self.excel is not None:
            for group, blocks in by_group.items():
                if group == spec.get("mythicalGroup"):
                    continue
                for block in blocks:
                    if spec.get("excel") == "runeword":
                        # A runeword needs the highest level of its runes (misc.txt), so its
                        # "Required Level" line follows its runes rather than the authored text.
                        rune_levels = [self.excel.rune_level(code) for _, _, code in block["runes"]]
                        level = max(rune_levels, default=0) or required_level(block["lines"])
                        block["level"] = level
                        block["lines"] = [
                            (kind, REQUIRED_LEVEL.sub("Required Level: %d" % level, text)
                             if REQUIRED_LEVEL.search(strip_color(text)) else text)
                            for kind, text in block["lines"]]
                        continue
                    class_name = CLASS_BY_AMULET_CODE.get(self.excel.code(block["title"]))
                    if class_name:
                        block["lines"] = [(kind, tag_class_skill(text, class_name)) for kind, text in block["lines"]]
                    block["tier"] = self.excel.tier(block["title"])
                    block["family"] = self.excel.family(block["title"]) if group == "Weapon" else ""
                    block["level"] = self.excel.level(block["title"])

        entries = []
        for group, blocks in by_group.items():
            if not blocks:
                continue
            title = categories.get(group, group)
            wide = group in (spec.get("wideCategories") or [])
            compact = group in (spec.get("compactCategories") or [])
            entries.append({"title": title, "subtitle": "%d items" % len(blocks), "blocks": blocks,
                            "wide": wide, "compact": compact,
                            "sortable": (bool(spec.get("sortable")) or group in (spec.get("sortableGroups") or [])) and not wide})
        self.pages.append({"spec": spec, "entries": entries})

    # --- rune families ------------------------------------------------------
    TINY_ART_TYPES = {"ring", "amul", "scha", "mcha", "jewl"}   # rings, amulets, small and medium charms, jewels

    def mark_tiny_art(self, kind, by_group, mythical_group=None):
        """Rings, amulets, jewels and small and medium charms are drawn at 50% of the usual art size (not the mythicals)."""
        names = {}
        for code, name in self.excel.base_names.items():
            if self.excel.base_types.get(code) in self.TINY_ART_TYPES:
                names.setdefault(name.lower(), code)
        for group, blocks in by_group.items():
            if group == mythical_group:
                continue
            for block in blocks:
                if kind == "unique":
                    base_type = self.excel.base_type(strip_color(block["title"]))
                else:
                    base = next((v for l, v in block["fields"] if l == "Base"), "")
                    base_type = self.excel.base_types.get(names.get(base.lower(), ""), "")
                # Cards whose item is not in uniques.txt (string-table uniques) are told by their picture.
                class_jewelry = any(key.startswith(("items_misc_ring_", "items_misc_amulet_")) for key in block["images"])
                if base_type in self.TINY_ART_TYPES or class_jewelry:
                    block["tiny"] = True

    def apply_mod_inventory_art(self, kind, by_group, mythical_group=None):
        """Each unique or set card shows the inventory art that the mod's uniques.json / sets.json
        names for the item at its tier. Items the JSON does not name keep the layout's art.
        Mythicals keep their own website art."""
        for group, blocks in by_group.items():
            if group == mythical_group:
                continue
            for block in blocks:
                title = strip_color(block["title"])
                if title in UNIQUE_ART_OVERRIDES or title in UNIQUE_PAGE_ART_OVERRIDES or any(k.startswith("mythical_") for k in block["images"]):
                    continue
                tier = block.get("tier") or (self.excel.tier(title) if kind == "unique" else "") or "Normal"
                art = self.mod_inventory_art(kind, title, tier)
                if art:
                    block["images"] = [self.note_image(art)]

    def mod_inventory_art(self, kind, title, tier):
        name = re.sub(r"[^a-z0-9]", "", title.lower())
        variants = self.mod_sets.get(name) if kind == "set" else None
        if variants is None:
            variants = self.mod_uniques.get(name)
        if not variants:
            return None
        path = variants.get(MOD_ART_TIER_FIELD.get(tier, "normal")) or variants.get("normal")
        if not path:
            return None
        for prefix in MOD_ART_PREFIXES:
            rel = prefix + "/" + path.replace("\\", "/")
            if any((Path(root) / (rel + ".sprite")).is_file() for root in self.ui_roots):
                return rel
        return None

    def unique_art(self, title):
        """Sprite path (under the ui root, no extension) for a unique, or None."""
        key = unique_key(title)
        stem = self.unique_paths.get(key) or key
        art = self.sprite_index.get(stem.lower())
        if art:
            return art
        # Display names differ from the internal names uniques.json and the art use.
        index_name = self.excel.unique_index_name(title) if self.excel else ""
        if index_name and unique_key(index_name) != key:
            stem = self.unique_paths.get(unique_key(index_name)) or unique_key(index_name)
            art = self.sprite_index.get(stem.lower())
            if art:
                return art
        # No artwork of its own: use the base item's inventory icon (Ring, Amulet, ...).
        base = self.excel.base_name(title)
        if not base:
            return None
        stems = []
        if base.endswith(" Ring"):          # class rings (Nec Ring, Pal Ring, ...) share the ring icon
            stems.append("ring")
        if base == "Magic Arrows":
            stems.append("arrows")
        if base == "Magic Bolts":
            stems.append("bolts")
        stems.append(unique_key(base))
        base_type = self.excel.base_type(title)
        stems.append(base_type)                    # icon named after the base type, e.g. "axe"
        stems.append(TYPE_ICON_STEM.get(base_type, ""))  # generic icon for the category
        for stem in stems:
            art = self.sprite_index.get(stem.lower()) if stem else None
            if art:
                return art
        return None

    def add_mythicals(self, by_group, spec):
        """Mythical uniques from data/mythical-uniques.txt: one card each, stats and notes beside it."""
        path = OVERLAY_DIR / spec["mythicals"]
        tooltips = self.mythical_tooltips()
        for raw in path.read_text(encoding="utf-8").splitlines():
            if not raw.strip() or raw.startswith("#"):
                continue
            name, base, required, stats, note = (part.strip() for part in raw.split("|"))
            title, variant = name, ""
            match = re.match(r"^(.*?) \((.+)\)$", name)
            if match:
                title, variant = match.group(1), match.group(2)
            subtitle = "%s (%s)" % (base, variant) if variant else base
            block = new_block(item=True, title=title, subtitle=subtitle)
            block["small"] = base in SMALL_ART_BASES
            # Tooltip: the website's property lines, in the same wording as the ESR site.
            block["fields"].append(("Required Level", required))
            if name in tooltips:
                # Website wording: orange description lines, then blue stats (data/mythical-tooltips.txt).
                for colour, text in tooltips[name]:
                    block["lines"].append(("S", ("\u00ffc8" if colour == "D" else "\u00ffc3") + text))
            else:
                for text in filter(None, (part.strip() for part in stats.split("~"))):
                    block["lines"].append(("S", "\u00ffc3" + text))
            block["note"] = note
            art_key = self.mythical_art_key(title)
            if art_key:
                self.add_unique(block["images"], art_key)
            else:
                art = self.unique_art(title) or self.mythical_icon(base)
                if art:
                    self.add_unique(block["images"], self.note_image(art))
            by_group.setdefault(spec["mythicalGroup"], []).append(block)

    def mythical_tooltips(self):
        """name -> [(colour, text)] with colour "D" (orange description) or "S" (blue stat)."""
        tooltips = {}
        path = OVERLAY_DIR / "data" / "mythical-tooltips.txt"
        if not path.exists():
            return tooltips
        for raw in path.read_text(encoding="utf-8").splitlines():
            parts = raw.split("\t")
            if len(parts) == 3:
                tooltips.setdefault(parts[0], []).append((parts[1], parts[2]))
        return tooltips

    def mythical_art_key(self, title):
        """The mythical's own art from the website (data/mythical-art/<name>.png), or None."""
        key = re.sub(r"[^a-z0-9]", "", title.lower())
        path = PLUGIN_DIR / "data" / "mythical-art" / f"{key}.png"
        if not path.exists():
            return None
        image_key_name = "mythical_" + key
        self.custom_art[image_key_name] = path
        return image_key_name

    def mythical_icon(self, base):
        """Generic icon for a mythical base, such as "Mythical Seraphic Scepter" -> scepter."""
        for word, stem in MYTHICAL_ICON_WORDS:
            if word.lower() in base.lower():
                art = self.sprite_index.get(stem)
                if art:
                    return art
        return None

    def add_string_uniques(self, by_group):
        """Uniques that have no carousel card: built from the string table and uniqueitems.txt."""
        seen = {excel_items.norm(block["title"]) for blocks in by_group.values() for block in blocks}
        for key in self.strings:
            if not key.startswith("EsrWikiUnique_"):
                continue
            lines = self.string_lines(key)
            if not lines:
                continue
            title = strip_color(lines[0]).strip()
            if not title or excel_items.norm(title) in seen:
                continue
            seen.add(excel_items.norm(title))
            block = new_block(item=True, title=title)
            block["lines"].extend(("S", line) for line in lines[1:])
            block["fields"].extend(self.excel.unique_fields(title) or [])
            art = self.unique_art(title)
            if art:
                self.add_unique(block["images"], self.note_image(art))
            by_group.setdefault(self.excel.unique_group(title), []).append(block)

    def build_runes(self, spec, roots):
        """Headings named in pages.json become categories; their rune icons are items."""
        families = list(spec["categories"])
        entries = []
        by_name = {}
        category = None
        guide = None
        pending = OrderedDict()  # id(guide entry) -> (entry, text fragments)

        def add_entry(title):
            entry = {"title": title, "subtitle": "", "blocks": []}
            entries.append(entry)
            return entry

        for root in roots:
            for node, _ in walk(root):
                kind = node.get("type")
                name = node.get("name", "")
                text = fields(node).get("text")
                if kind == "TextBoxWidget" and isinstance(text, str) and clean(text):
                    title = clean(text).strip()
                    if title in families:
                        if title not in by_name:
                            by_name[title] = add_entry(title)
                        category = by_name[title]
                        guide = None
                    elif name.startswith("info") and guide is not None:
                        # Text boxes are positioned by hand, so one sentence can be split across
                        # several of them. They are joined into paragraphs in guide_block below.
                        pending.setdefault(id(guide), (guide, []))[1].extend(split_lines(text))
                    else:
                        category = None
                        guide = add_entry(title)
                    continue
                key = tooltip_key(node)
                if key and key.startswith(RUNE_PREFIX) and category is not None:
                    lines = self.string_lines(key)
                    block = new_block(item=True, title=strip_color(lines[0]) if lines else key)
                    filename = fields(node).get("filename") or DEFAULT_RUNE_SPRITE
                    block["images"].append(self.note_image(filename))
                    block["lines"].extend(("S", line) for line in lines[1:])
                    rune_level = self.excel.rune_level(key[len(RUNE_PREFIX):]) if self.excel is not None else 0
                    if rune_level:
                        block["fields"].append(("Required Level", str(rune_level)))
                    category["blocks"].append(block)
        for guide_entry, fragments in pending.values():
            guide_entry["blocks"] = [guide_block(fragments)]
        self.pages.append({"spec": spec, "entries": entries})


STEP_START = re.compile(r"^\d+\.\s")
SENTENCE_END = re.compile(r"[.:!?)]$")


def merge_fragments(fragments):
    """Joins a sentence that was split across text boxes. A new paragraph starts after a sentence
    end, at a numbered step, or at a recipe line."""
    paragraphs = []
    for text in fragments:
        text = text.strip()
        if not text:
            continue
        previous = paragraphs[-1] if paragraphs else None
        continues = (previous is not None and not SENTENCE_END.search(previous)
                     and not STEP_START.match(text) and not RECIPE_SPLIT.search(text)
                     and not RECIPE_SPLIT.search(previous))
        if continues:
            paragraphs[-1] = previous + " " + text
        else:
            paragraphs.append(text)
    return paragraphs


def guide_block(fragments):
    """One guide block: paragraphs as text lines, and each run of 'ingredients -> result' lines
    as a two-column table."""
    block = new_block()
    rows = []

    def flush():
        if rows:
            block["lines"].append(("C", TABLE_SEPARATOR.join(["Ingredients", "Result"])))
            block["lines"].extend(("R", TABLE_SEPARATOR.join(row)) for row in rows)
            rows.clear()

    for paragraph in merge_fragments(fragments):
        parts = RECIPE_SPLIT.split(paragraph, maxsplit=1)
        if len(parts) == 2:
            rows.append(parts)
            continue
        flush()
        block["lines"].append(("T", paragraph))
    flush()
    return block


ITEM_RARITY_PREFIX = re.compile(r"^(?:(?:Unique|Set|Crafted|Rare|Magic|Normal)/?)+\s+")


def item_type(recipe_in):
    """'Unique/Set/Crafted/Rare/Magic/Normal Body Armor + Worldstone Shard' -> 'Body Armor'."""
    name = recipe_in.split(" + ")[0]
    return ITEM_RARITY_PREFIX.sub("", name).strip()


def grouped_item_blocks(section):
    """One block per item type. The type is the block's section and family, so the overlay shows
    it as a heading and as a Type filter. Each block is a one-column table of outcomes."""
    blocks = []
    if section.get("notes"):
        note_block = new_block(item=False)
        note_block["lines"] = [("T", note) for note in section["notes"]]
        blocks.append(note_block)
    by_type = OrderedDict()
    for recipe_in, recipe_out in section.get("rows", []):
        by_type.setdefault(item_type(recipe_in), []).append(recipe_out)
    for kind, outcomes in by_type.items():
        block = new_block(item=False, section=kind)
        # A Type filter with one choice does nothing, so only sections with several types get one.
        block["family"] = kind if len(by_type) > 1 else ""
        block["lines"] = [("C", "Possible Outcome(s)")]
        block["lines"] += [("R", COLOR_BLUE + outcome) for outcome in outcomes]
        blocks.append(block)
    return blocks


RECIPE_PREFIX = "To craft combine the following in the Horadric Cube:"


def move_recipes_above_cards(page):
    """Class crafting: every item in a class uses the same crafting materials and only the magic
    base changes, so each class gets one recipe line above its cards (one per distinct material set)
    instead of a line per item. The item cards keep only their stats."""
    for entry in page["entries"]:
        groups = {}
        for block in entry["blocks"]:
            if not block["item"]:
                continue
            ingredients = [text for kind, text in block["lines"] if kind == "T"]
            if len(ingredients) < 2:
                continue
            block["lines"] = [line for line in block["lines"] if line[0] != "T"]
            groups.setdefault(tuple(ingredients[1:]), []).append(ingredients[0])
        if not groups:
            continue
        intro = new_block(item=False)
        intro["lines"] = [
            ("T", f"{RECIPE_PREFIX} {' + '.join(materials)} "
                  "+ a corresponding Magic base item")
            for materials in groups
        ]
        entry["blocks"].insert(0, intro)


# --- Cube Recipes pages as tables ---------------------------------------------
# Built from the Cube Recipes overlay's data (overlay/data/cube-wiki-data.txt, written by
# build_cube_wiki.py). Recipe lines become tables; each sub-heading starts its own section.
CUBE_WIKI_DATA = OVERLAY_DIR / "data" / "cube-wiki-data.txt"
RECIPE_SPLIT = re.compile(r"\s+->\s+")
FORGING_SECTION_STARTS = [
    (re.compile(r"^(Skill|All Skill|Stat|Rune) Forging Target Item includes:"), lambda m: f"{m.group(1)} Forging"),
    (re.compile(r"^Aura Forging\b"), lambda m: "Aura Forging"),
    (re.compile(r"^Merc Only Conversion\b"), lambda m: "Merc Only Conversion"),
]
TABLE_SEPARATOR = "\x1f"
# Long D-Stone notes run several topics together; each one starts a new paragraph.
CUBE_NOTE_TOPICS = re.compile(r"(?=\b(?:D-Stoning Caps|D-Stoning Runeworded Items|Fail-Safe Features) )")


def split_cube_notes(text):
    return [part.strip() for part in CUBE_NOTE_TOPICS.split(text) if part.strip()]


def tidy_recipe(text):
    """Plain recipe text: colour codes gone, one space between words, and no doubled '+' left
    where a coloured part was cut out of a sentence."""
    text = re.sub(r"\s+", " ", strip_color(text)).strip()
    while re.search(r"\+\s*\+", text):
        text = re.sub(r"\+\s*\+", "+", text)
    return re.sub(r"\s+", " ", text).strip(" +")


def split_cube_sections(entries):
    """(entry title, text lines) pairs from the cube data -> sections of notes and recipe rows.
    Forging sub-headings and D-Stone topics each start their own section."""
    sections = []

    def start(title):
        section = {"title": title, "notes": [], "rows": []}
        sections.append(section)
        return section

    current = None
    for entry_title, texts in entries:
        current = start(entry_title)
        for text in [part for line in texts for part in split_cube_notes(line)]:
            heading = next((title(m) for pattern, title in FORGING_SECTION_STARTS
                            for m in [pattern.match(text)] if m), None)
            if heading:
                current = start(heading)
                current["notes"].append(text)
                continue
            parts = RECIPE_SPLIT.split(text, maxsplit=1)
            if len(parts) == 2:
                current["rows"].append((parts[0], parts[1]))
            else:
                current["notes"].append(text)
    return sections


def load_cube_page(page_id):
    """(entry title, text lines) for each entry of one page in the cube recipes data."""
    if not CUBE_WIKI_DATA.exists():
        raise SystemExit(f"{CUBE_WIKI_DATA} is missing; run tools/build_cube_wiki.py first")
    entries = []
    current_page = None
    for raw in CUBE_WIKI_DATA.read_text(encoding="utf-8").split("\n"):
        fields = raw.split("\t")
        tag = fields[0]
        if tag == "PAGE":
            current_page = fields[1] if len(fields) > 1 else None
        elif current_page != page_id:
            continue
        elif tag == "ENTRY":
            entries.append((fields[1] if len(fields) > 1 else "", []))
        elif tag == "LINE" and len(fields) >= 3 and fields[1] == "T" and entries:
            entries[-1][1].append(fields[2])
        elif tag == "LINE" and len(fields) >= 3 and fields[1] == "R" and entries:
            # Recipe tables: back to the "input  ->  output" text the wiki pages were built from.
            recipe_in, _, recipe_out = fields[2].partition(TABLE_SEPARATOR)
            entries[-1][1].append(f"{recipe_in}  ->  {recipe_out}")
    return entries


# Colour codes the overlay reads: ÿc0 white, ÿc1 red, ÿc3 blue, ÿc4 gold.
COLOR_WHITE = "\u00ffc0"
COLOR_RED = "\u00ffc1"
COLOR_BLUE = "\u00ffc3"
COLOR_GOLD = "\u00ffc4"
# A zero-width space keeps a blank line in the output (empty LINE text is dropped).
SPACER_LINE = "\u200b"
TIER_HEADER = re.compile(r"^Tier \d+:$")


def color_asterisks(text, base):
    """Footnote markers (*, **, ***) are red; the rest of the text keeps the base colour."""
    return re.sub(r"\*+", lambda m: f"{COLOR_RED}{m.group(0)}{base}", text)


# A stat text box that ends on one of these words, or the next box that starts with a lower-case
# word, continues the same stat ("Gain 1 All Skills for every" / "1000 Attributes").
STAT_CONTINUES_AFTER = re.compile(r"\b(every|for|and|of|to|the|a|an|per|on|when|with|than)$", re.IGNORECASE)
STAT_CONTINUES_WITH = re.compile(r"^[a-z]")


def normalize_stat(text):
    """Stat text as the ascendancy reference writes it: single spaces, no final full stop."""
    return re.sub(r"\s+", " ", text).strip().rstrip(".").strip()


_ASC_STATS = None


def ascendancy_stats():
    """Every stat line of the ascendancies reference (data/ascendancy-stats.txt, one per line), and
    each word-prefix of those lines. A stat that is split across text boxes is joined only when the
    joined text is one of these lines."""
    global _ASC_STATS
    if _ASC_STATS is None:
        stats, prefixes = set(), set()
        path = PLUGIN_DIR / "data" / "ascendancy-stats.txt"
        if path.exists():
            for raw in path.read_text(encoding="utf-8").splitlines():
                stat = normalize_stat(raw)
                if not stat:
                    continue
                stats.add(stat)
                words = stat.split(" ")
                for k in range(1, len(words)):
                    prefixes.add(" ".join(words[:k]))
        _ASC_STATS = (stats, prefixes)
    return _ASC_STATS


def join_by_reference(lines):
    stats, prefixes = ascendancy_stats()
    out = []
    i = 0
    while i < len(lines):
        kind, text = lines[i]
        best = None  # (index of the last box used, joined stat)
        if kind == "T" and normalize_stat(text) in prefixes | stats:
            current = normalize_stat(text)
            k = i
            while k + 1 < len(lines) and lines[k + 1][0] == "T":
                candidate = normalize_stat(current + " " + lines[k + 1][1])
                if candidate in stats:
                    best = (k + 1, candidate)
                if candidate not in stats and candidate not in prefixes:
                    break
                current = candidate
                k += 1
        if best is not None:
            out.append(("T", best[1]))
            i = best[0] + 1
        else:
            out.append(lines[i])
            i += 1
    return out


# The reference writes each sentence of a stat box on its own line ("... Pierce." then "Gain ...").
SENTENCE_BREAK = re.compile(r"(?<=\.)\s+(?=[A-Z+])")


def split_sentences(lines):
    out = []
    for kind, text in lines:
        if kind == "T" and not re.match(r"^\s*\d+\*", text) and not TIER_HEADER.match(text):
            out.extend(("T", part.strip()) for part in SENTENCE_BREAK.split(text) if part.strip())
        else:
            out.append((kind, text))
    return out


def join_split_stats(lines, split=False):
    """Joins stats split across text boxes: first by the reference list, then by the wording rules for
    the stats the reference does not have (a box ending in "every", or the next box starting lower case).
    With split, sentences in one box are first put on their own lines, as the reference does."""
    if split:
        lines = split_sentences(lines)
    out = []
    for kind, text in join_by_reference(lines):
        if (kind == "T" and out and out[-1][0] == "T"
                and (STAT_CONTINUES_AFTER.search(out[-1][1]) or STAT_CONTINUES_WITH.match(text))):
            out[-1] = ("T", out[-1][1] + " " + text)
        else:
            out.append((kind, text))
    return out


_ASC_TIERS = None
TIER_NUMBER_LINE = re.compile(r"^Tier (\d)\*?:?$")


def ascendancy_tiers(title):
    """Each class's tier stats, as the ascendancies reference page writes them (data/ascendancy-tiers.json).
    None for a title the reference does not cover."""
    global _ASC_TIERS
    if _ASC_TIERS is None:
        path = PLUGIN_DIR / "data" / "ascendancy-tiers.json"
        _ASC_TIERS = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    return _ASC_TIERS.get(title)


def apply_reference_tiers(lines, reference):
    """Under each tier header, the reference's stat lines replace the layout's split stat boxes. Footnotes
    (lines starting with an asterisk) and the next header end a tier."""
    def is_footnote(text):
        return text.lstrip().startswith("*")
    out = []
    i = 0
    while i < len(lines):
        kind, text = lines[i]
        match = TIER_NUMBER_LINE.match(text.strip()) if kind == "T" else None
        if match and match.group(1) in reference:
            out.append(("T", text))
            out.extend(("T", stat) for stat in reference[match.group(1)])
            i += 1
            while i < len(lines) and not (lines[i][0] == "T" and (TIER_NUMBER_LINE.match(lines[i][1].strip())
                                                                or is_footnote(lines[i][1]))):
                i += 1
            continue
        out.append((kind, text))
        i += 1
    return out


def style_ascendancy_lines(entry, footnotes):
    """Tier headers gold, their stat lines blue, a blank line before each new tier and before the
    footnotes, footnotes red, and footnote asterisks red wherever they appear."""
    out_blocks = []
    seen_tier = False
    footnote_started = False
    for block in entry["blocks"]:
        lines = []
        reference = ascendancy_tiers(entry["title"])
        if reference is not None:
            block["lines"] = apply_reference_tiers(block["lines"], reference)
        else:
            block["lines"] = join_split_stats(block["lines"], split=entry["title"] != "Ascendancies")
        for kind, text in block["lines"]:
            if kind != "T":
                lines.append((kind, text))
                continue
            if text in footnotes:
                if not footnote_started:
                    lines.append(("T", SPACER_LINE))
                    footnote_started = True
                lines.append(("T", f"{COLOR_RED}{text}"))
            elif TIER_HEADER.match(text):
                if seen_tier:
                    lines.append(("T", SPACER_LINE))
                seen_tier = True
                lines.append(("T", f"{COLOR_GOLD}{text}{COLOR_WHITE}"))
            elif seen_tier and not footnote_started:
                lines.append(("T", f"{COLOR_BLUE}{color_asterisks(text, COLOR_BLUE)}{COLOR_WHITE}"))
            else:
                lines.append(("T", text))
        block["lines"] = lines


ASCENDANCY_GEM_IMAGE_PREFIX = "items_misc_"


def apply_ascendancy_notes(page, notes):
    """Ascendancy: the tier 1 orb and gem pictures are replaced by text. Each class gets a line under
    its class image with the input to ascend, and its footnotes replace the "mouse over class image" hint."""
    for entry in page["entries"]:
        info = notes.get(entry["title"])
        if info is None:
            if entry["blocks"] and entry["title"] != "Ascendancies":
                print(f"note: no ascendancy notes for {entry['title']}")
            continue
        for block in entry["blocks"]:
            block["images"] = [key for key in block["images"]
                               if not key.startswith(ASCENDANCY_GEM_IMAGE_PREFIX)]
            lines = []
            for kind, text in block["lines"]:
                if kind == "T" and "mouse over" in text.lower():
                    lines.extend(("T", note) for note in info["footnotes"])
                else:
                    lines.append((kind, text))
            block["lines"] = lines
        style_ascendancy_lines(entry, set(info["footnotes"]))
        entry["blocks"][0]["lines"].insert(0, ("T", f"To Ascend to {entry['title']} combine {info['input']}"))


ASCENDANCY_SENTENCE_ENDS = re.compile(r"special encounters\.|endgame content\.|any order\.|etc\.\)\.")
ASCENDANCY_ORB_PARAGRAPH = "In order to get an Ascendancy Stone"
ASCENDANCY_ORB_TITLE = "Orb of Creation / Material Locations"
ASCENDANCY_ORB_HEADER = ["Orb", "Act", "Location", "Monster"]
ASCENDANCY_ORB_ROWS = [
    ["Orb of Storms", "Act 1", "The Hole Level 2 (Black Marsh)", "Rathma's Acolyte"],
    ["Orb of Earth", "Act 2", "The Halls of the Dead Level 2 (Dry Hills)", "Horazon's Deciple"],
    ["Orb of Water", "Act 3", "South-East of the Council (Travincal)", "Uldyssian's Hound"],
    ["Orb of Wind", "Act 5", 'Westside of the "Crystal River" Entrance (Arreat Plateau)', "Mathael's Imp"],
    ["Orb of Fire", "Act 5", "North-West Corner of the Worldstone Chamber", "Inarius' Stalker"],
]
ASCENDANCY_ORB_COMBINE_LINE = "Combine the 5 elemental orbs in the cube to create Orb of Creation"
ASCENDANCY_UPGRADE_LINE = "To upgrade your Ascendancy refer to the Higher Tier Challenges"


def ascendancy_intro_lines(text):
    """The intro paragraph as lines, with two blank lines after each of its sentence ends
    (special encounters., endgame content., any order., etc.))."""
    lines = []
    pos = 0
    for match in ASCENDANCY_SENTENCE_ENDS.finditer(text):
        lines.append(("T", text[pos:match.end()].strip()))
        lines.extend([("T", SPACER_LINE)] * 2)
        pos = match.end()
    tail = text[pos:].strip()
    if tail:
        lines.append(("T", tail))
    return lines


def ascendancy_orb_table():
    """The Orb of Creation paragraph replaced by a table of where each Orb drops, then a pointer
    to the Higher Tier Challenges outside the table."""
    lines = [("T", ASCENDANCY_ORB_TITLE), ("C", TABLE_SEPARATOR.join(ASCENDANCY_ORB_HEADER))]
    lines.extend(("R", TABLE_SEPARATOR.join(row)) for row in ASCENDANCY_ORB_ROWS)
    lines.extend([("T", SPACER_LINE), ("T", ASCENDANCY_ORB_COMBINE_LINE), ("T", SPACER_LINE)])
    lines.append(("T", ASCENDANCY_UPGRADE_LINE))
    return lines


def arrange_ascendancy_page(page):
    """Ascendancies page layout: intro spacing, the Orb table, and Higher Tier Challenges directly
    under the Ascendancies entry instead of at the end of the page."""
    entries = page["entries"]
    titles = [entry["title"] for entry in entries]
    intro = next((entry for entry in entries if entry["title"] == "Ascendancies"), None)
    if intro is not None:
        for block in intro["blocks"]:
            lines = []
            for kind, text in block["lines"]:
                if kind == "T" and text.startswith(ASCENDANCY_ORB_PARAGRAPH):
                    lines.extend(ascendancy_orb_table())
                elif kind == "T" and ASCENDANCY_SENTENCE_ENDS.search(text):
                    lines.extend(ascendancy_intro_lines(text))
                else:
                    lines.append((kind, text))
            block["lines"] = lines
    if "Ascendancies" in titles and "Higher Tier Challenges" in titles:
        insert_at = titles.index("Ascendancies") + 1
        entries.insert(insert_at, entries.pop(titles.index("Higher Tier Challenges")))



def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--excel", help="the mod's data/global/excel folder (adds item fields)")
    parser.add_argument("--mod-strings", help="the mod's data/local/lng/strings folder (skill display names for item lines;"
                                              " found from --sprite-root when omitted)")
    parser.add_argument("--sprite-root", help="the mod's data/hd/global/ui folder (adds unique artwork)")
    parser.add_argument("--base-ui-root", help="the base game's data/hd/global/ui folder (fallback artwork)")
    parser.add_argument("--uniques-json", help="the mod's data/hd/items/uniques.json (names artwork per unique)")
    args = parser.parse_args()

    excel = None
    if args.excel:
        item_names = None
        if args.uniques_json:
            # <mod>/data/hd/items/uniques.json -> <mod>/data/local/lng/strings/item-names.json
            item_names = Path(args.uniques_json).parents[2] / "local" / "lng" / "strings" / "item-names.json"
        excel = excel_items.Tables(args.excel, item_names)
    else:
        print("note: no --excel folder given; item fields (base, level, bases) are skipped")

    config = json.loads((OVERLAY_DIR / "pages.json").read_text(encoding="utf-8"), object_pairs_hook=OrderedDict)
    strings = load_string_table(PLUGIN_DIR / "data" / "strings" / "esr-wiki-strings.json")
    builder = Builder(strings, excel)
    ascendancy_notes = json.loads((PLUGIN_DIR / "data" / "ascendancy-notes.json").read_text(encoding="utf-8"),
                                  object_pairs_hook=OrderedDict)
    if args.sprite_root:
        builder.sprite_index = load_sprite_index(args.sprite_root)
        # The mod's string tables (skill display names) sit beside hd/: data/local/lng/strings.
        guess = Path(args.sprite_root).resolve().parents[2] / "local" / "lng" / "strings"
        builder.strings_folder = args.mod_strings or (str(guess) if guess.is_dir() else None)
    if args.base_ui_root:
        # The mod's files win; the base game supplies art the mod does not override.
        for stem, rel in load_sprite_index(args.base_ui_root).items():
            builder.sprite_index.setdefault(stem, rel)
    if args.uniques_json:
        for entry in json.loads(Path(args.uniques_json).read_text(encoding="utf-8")):
            for key, variants in entry.items():
                builder.unique_paths[key] = PurePosixPath(variants["normal"]).name
        builder.mod_uniques = mod_art_table(args.uniques_json)
        sets_json = Path(args.uniques_json).with_name("sets.json")
        if sets_json.exists():
            builder.mod_sets = mod_art_table(sets_json)
    builder.ui_roots = [root for root in (args.sprite_root, args.base_ui_root) if root]
    for spec in config["pages"]:
        if spec.get("disabled"):
            continue  # kept for reference, not built
        roots = [load_layout(PLUGIN_DIR / source) for source in spec.get("sources", [])]
        if spec["mode"] == "cubeTables":
            builder.build_cube_tables(spec)
        elif spec["mode"] == "dataTables":
            builder.build_data_tables(spec)
        elif spec["mode"] == "guide":
            builder.build_guide(spec, roots)
        elif spec["mode"] == "endgame":
            builder.build_endgame(spec)
        elif spec["mode"] == "baseItems":
            builder.build_base_items(spec)
        elif spec["mode"] == "baseItemTables":
            builder.build_base_item_tables(spec)
        elif spec["mode"] == "baseItemCards":
            builder.build_base_item_cards(spec)
        elif spec["mode"] == "cards":
            builder.build_cards(spec, roots)
        elif spec["mode"] == "runes":
            builder.build_runes(spec, roots)
        else:
            raise SystemExit(f"unknown mode {spec['mode']} for {spec['id']}")

    for page in builder.pages:
        if page["spec"].get("itemPerButton"):
            move_recipes_above_cards(page)
        if page["spec"].get("ascendancyNotes"):
            arrange_ascendancy_page(page)
            apply_ascendancy_notes(page, ascendancy_notes)

    DATA_OUT.mkdir(parents=True, exist_ok=True)
    # Names are shown without the "_lod" marker (the LoD copies of uniques share the plain names).
    def text_out(value):
        return re.sub(r"_lod\b", "", clean(value))

    out = ["#ESRWIKI\t2"]
    for page in builder.pages:
        spec = page["spec"]
        style = f"\t{text_out(spec['style'])}" if spec.get("style") else ""
        out.append(f"PAGE\t{spec['id']}\t{text_out(spec['title'])}\t{text_out(spec['group'])}{style}")
        for entry in page["entries"]:
            flag = ("\twide" if entry.get("wide") else "\tcompact" if entry.get("compact")
                    else "\tsortable" if entry.get("sortable") else "")
            if entry.get("fiveacross"):
                flag += "\tfiveacross"
            out.append(f"ENTRY\t{text_out(entry['title'])}\t{text_out(entry['subtitle'])}{flag}")
            for block in entry["blocks"]:
                out.append(f"BLOCK\t{1 if block['item'] else 0}\t{text_out(block['title'])}\t{text_out(block['subtitle'])}"
                           f"\t{text_out(block['section'])}\t{text_out(block.get('tier', ''))}"
                           f"\t{text_out(block.get('family', ''))}\t{block.get('level', 0)}")
                if block.get("tag"):
                    out.append(f"TAG\t{text_out(block['tag'])}")
                for label, value in block["fields"]:
                    out.append(f"FIELD\t{text_out(label)}\t{text_out(value)}")
                flag = ("\tsmall" if block.get("small") else "\ttiny" if block.get("tiny")
                        else "\toriginal" if block.get("original") else "")
                for key in block["images"]:
                    out.append(f"IMG\t{key}{flag}")
                for key, label, code in block["runes"]:
                    lod = "\tlod" if excel is not None and excel.is_lod_rune(code) else ""
                    out.append(f"RUNE\t{key}\t{text_out(label)}{lod}")
                for kind, text in block["lines"]:
                    out.append(f"LINE\t{kind}\t{text_out(text)}")
                if block.get("note"):
                    out.append(f"NOTE\t{text_out(block['note'])}")
    (DATA_OUT / "wiki-data.txt").write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")

    for key, path in sorted(builder.custom_art.items()):
        (DATA_OUT / "images").mkdir(exist_ok=True)
        shutil.copyfile(path, DATA_OUT / "images" / f"{key}.png")

    manifest = [f"{key}\t{rel}" for key, rel in sorted(builder.images.items())]
    (DATA_OUT / "image-manifest.txt").write_text("\n".join(manifest) + "\n", encoding="utf-8", newline="\n")

    total_blocks = 0
    for page in builder.pages:
        spec = page["spec"]
        blocks = sum(len(e["blocks"]) for e in page["entries"])
        total_blocks += blocks
        print(f"{spec['id']:20s} categories/sections={len(page['entries']):4d} items/blocks={blocks:5d}")
    print(f"total blocks={total_blocks} images={len(builder.images)} "
          f"missing strings={len(builder.missing_strings)} excel unmatched={len(builder.unmatched)}")
    for line in getattr(builder, "set_report", []):
        print("  set: " + line)
    for line in getattr(builder, "unique_report", []):
        print("  unique: " + line)
    for line in getattr(builder, "duplicate_report", []):
        print("  dup: " + line)
    for line in getattr(builder, "description_report", []):
        print("  desc: " + line)
    if builder.unmatched:
        for page_id, title in builder.unmatched[:20]:
            print(f"  unmatched: {page_id}: {title}")


if __name__ == "__main__":
    main()
