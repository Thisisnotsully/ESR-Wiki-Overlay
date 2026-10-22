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
import json
import re
import shutil
from collections import OrderedDict
from pathlib import Path, PurePosixPath

from wiki_layout import fields, load_layout, load_string_table, tooltip_key, walk
import excel_items

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
TYPE_ICON_STEM = {
    "abow": "long_bow", "bow": "short_bow", "xbow": "crossbow",
    "ajav": "javelin", "jave": "javelin", "bjav": "spear", "aspe": "spear", "spea": "spear",
    "ashd": "rondache", "shie": "kite_shield",
    "asnx": "short_sword", "wswd": "short_sword", "swor": "long_sword", "bedg": "scimitar",
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
                    block["title"] = clean(text)
                else:
                    block["lines"].extend(("T", line) for line in split_lines(text))
            if child.get("type") == "ImageWidget" and is_content_image(cfields.get("filename")):
                self.add_unique(block["images"], self.note_image(cfields["filename"]))
            tkey = tooltip_key(child)
            if tkey and tkey not in seen_keys:
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
            return self.excel.runeword_fields(block["title"], codes)
        return None

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

    def build_cards(self, spec, roots):
        pattern = re.compile(spec["cardPattern"])
        self.subtitle_from_name = bool(spec.get("subtitleFromNameParts"))
        categories = spec.get("categories") or {}
        # Groups in pages.json order first, then any others in the order they appear.
        by_group = OrderedDict((group, []) for group in categories)
        for root in roots:
            for node, _ in walk(root):
                if node.get("type") != "Widget" or not pattern.search(node.get("name", "")):
                    continue
                group, block = self.read_card(node)
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
                if spec.get("excel") == "runeword":
                    block["fields"].insert(0, ("Sockets", str(len(block["runes"]))))
                    count = len(block["runes"])
                    block["section"] = "%d Socket%s" % (count, "" if count == 1 else "s")
                if spec.get("excel") == "set":
                    for label, value in block["fields"]:
                        if label == "Set":
                            block["section"] = value

        if spec.get("difficultyGroup") in by_group:
            by_group[spec["difficultyGroup"]] = self.group_hireling_sets(by_group[spec["difficultyGroup"]])
        if spec.get("mythicals"):
            self.add_mythicals(by_group, spec)
        if spec.get("stringUniques") and self.excel is not None:
            self.add_string_uniques(by_group)
        if spec.get("excel") == "unique":
            # Runs after the string-table uniques are added, which take a generic icon otherwise.
            for blocks in by_group.values():
                for block in blocks:
                    page_art = UNIQUE_PAGE_ART_OVERRIDES.get(strip_color(block["title"]))
                    if page_art:
                        block["images"] = [self.note_image(page_art)]
        if spec.get("excel") == "unique" and self.excel is not None:
            for blocks in by_group.values():
                for block in blocks:
                    if self.excel.coupon_only(strip_color(block["title"])):
                        block["tag"] = "(Coupon only)"
        if spec.get("excel") in ("unique", "set") and self.excel is not None:
            self.apply_mod_inventory_art(spec["excel"], by_group, spec.get("mythicalGroup"))

        if spec.get("sortable") and self.excel is not None:
            for group, blocks in by_group.items():
                if group == spec.get("mythicalGroup"):
                    continue
                for block in blocks:
                    if spec.get("excel") == "runeword":
                        block["level"] = required_level(block["lines"])
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
                            "sortable": bool(spec.get("sortable")) and not wide})
        self.pages.append({"spec": spec, "entries": entries})

    # --- rune families ------------------------------------------------------
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
    parser.add_argument("--sprite-root", help="the mod's data/hd/global/ui folder (adds unique artwork)")
    parser.add_argument("--base-ui-root", help="the base game's data/hd/global/ui folder (fallback artwork)")
    parser.add_argument("--uniques-json", help="the mod's data/hd/items/uniques.json (names artwork per unique)")
    args = parser.parse_args()

    excel = None
    if args.excel:
        excel = excel_items.Tables(args.excel)
    else:
        print("note: no --excel folder given; item fields (base, level, bases) are skipped")

    config = json.loads((OVERLAY_DIR / "pages.json").read_text(encoding="utf-8"), object_pairs_hook=OrderedDict)
    strings = load_string_table(PLUGIN_DIR / "data" / "strings" / "esr-wiki-strings.json")
    builder = Builder(strings, excel)
    ascendancy_notes = json.loads((PLUGIN_DIR / "data" / "ascendancy-notes.json").read_text(encoding="utf-8"),
                                  object_pairs_hook=OrderedDict)
    if args.sprite_root:
        builder.sprite_index = load_sprite_index(args.sprite_root)
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
            out.append(f"ENTRY\t{text_out(entry['title'])}\t{text_out(entry['subtitle'])}{flag}")
            for block in entry["blocks"]:
                out.append(f"BLOCK\t{1 if block['item'] else 0}\t{text_out(block['title'])}\t{text_out(block['subtitle'])}"
                           f"\t{text_out(block['section'])}\t{text_out(block.get('tier', ''))}"
                           f"\t{text_out(block.get('family', ''))}\t{block.get('level', 0)}")
                if block.get("tag"):
                    out.append(f"TAG\t{text_out(block['tag'])}")
                for label, value in block["fields"]:
                    out.append(f"FIELD\t{text_out(label)}\t{text_out(value)}")
                flag = "\tsmall" if block.get("small") else "\toriginal" if block.get("original") else ""
                for key in block["images"]:
                    out.append(f"IMG\t{key}{flag}")
                for key, label, _code in block["runes"]:
                    out.append(f"RUNE\t{key}\t{text_out(label)}")
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
    if builder.unmatched:
        for page_id, title in builder.unmatched[:20]:
            print(f"  unmatched: {page_id}: {title}")


if __name__ == "__main__":
    main()
