import re
"""Tests for the wiki extractor and sprite naming. Run from esr-wiki-plugin/overlay:
    python3 -m unittest discover -s tests -p "test_*.py"
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

OVERLAY = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(OVERLAY / "tools"))

import extract_wiki as ex  # noqa: E402
import wiki_layout as wl  # noqa: E402


class LayoutReaderTests(unittest.TestCase):
    def test_trailing_commas_are_tolerated(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as handle:
            handle.write('{"type": "Widget", "fields": {"a": [1, 2,],},}')
        data = wl.load_layout(handle.name)
        self.assertEqual(data["fields"]["a"], [1, 2])

    def test_tooltip_key_reads_esr_wiki_reference(self):
        node = {"fields": {"tooltipString": "@esr-wiki:EsrWikiRune_r01"}}
        self.assertEqual(wl.tooltip_key(node), "EsrWikiRune_r01")
        self.assertIsNone(wl.tooltip_key({"fields": {"tooltipString": "@d2r:strClose"}}))


class NamingTests(unittest.TestCase):
    def test_image_key_is_stable_and_lowercase(self):
        self.assertEqual(ex.image_key("items\\\\armor\\\\helm"), "items_armor_helm")
        self.assertEqual(ex.image_key("Items\\Weapon\\Sword"), "items_weapon_sword")

    def test_sprite_path_uses_forward_slashes(self):
        self.assertEqual(ex.sprite_rel_path("items\\\\misc\\\\rune\\\\i_rune"), "items/misc/rune/i_rune.sprite")

    def test_chrome_images_are_filtered(self):
        self.assertFalse(ex.is_content_image("panel\\\\loot_filter\\\\lootfilter_codeinput"))
        self.assertFalse(ex.is_content_image("PauseMenu\\\\Divider"))
        self.assertFalse(ex.is_content_image("controller\\\\frontend\\\\x"))
        self.assertTrue(ex.is_content_image("items\\\\weapon\\\\polearm\\\\scythe"))
        self.assertTrue(ex.is_content_image("hireables\\\\amazonicon"))


class ExtractionTests(unittest.TestCase):
    """Checks the generated data/wiki-data.txt (format 2) against the source layouts."""

    @classmethod
    def setUpClass(cls):
        cls.path = OVERLAY / "data" / "wiki-data.txt"
        cls.text = cls.path.read_text(encoding="utf-8").split("\n")
        cls.entries = {}   # page id -> list of entry titles
        cls.blocks = {}    # page id -> block count
        cls.fields = {}    # block title -> list of (label, value)
        page = None
        block = None
        for line in cls.text:
            parts = line.split("\t")
            if parts[0] == "PAGE":
                page = parts[1]
                cls.entries[page] = []
                cls.blocks[page] = 0
            elif parts[0] == "ENTRY":
                cls.entries[page].append(parts[1])
            elif parts[0] == "BLOCK":
                cls.blocks[page] += 1
                block = parts[2]
            elif parts[0] == "FIELD" and block is not None:
                cls.fields.setdefault(block, []).append((parts[1], parts[2]))

    def test_header_and_page_order(self):
        self.assertEqual(self.text[0], "#ESRWIKI\t2")
        self.assertEqual(list(self.entries)[0], "welcome")
        self.assertEqual(len(self.entries), 17)  # cube recipes moved to the cube overlay (with Forging and D-Stones); Endgame Maps, Weapon Mastery, Item Bases, Item Affixes and IAS Calculator added

    def test_category_and_block_counts(self):
        expected = {
            "runes": (4, 95),   # 94 website runes + the how-to note   # built from the website gems page
            "runewords": (8, 474),   # built from the website runewords page; a recipe on several item kinds has a card in each
            "set-items": (10, 346),   # built from the website sets page: 269 pieces and 77 set cards
            "uniques": (13, 1106),   # built from the website pages
            "gemwords": (8, 1408),   # built from the website gemwords page; a row has a card under each item kind it allows
        }
        for page, (categories, blocks) in expected.items():
            with self.subTest(page=page):
                self.assertEqual(len(self.entries[page]), categories)
                self.assertEqual(self.blocks[page], blocks)

    def test_rune_categories_in_order(self):
        self.assertEqual(
            self.entries["runes"],
            ["Kanji Runes", "LoD Runes", "Elemental Runes", "How to Acquire Elemental Runes"],
        )
        # Blocks per rune family on the website page: 47 Kanji (Null Rune ends them), 33 LoD, 14 elemental.
        text = self.text
        counts, current = {}, None
        runes_start = next(i for i, l in enumerate(text) if l.startswith("PAGE\trunes\t"))
        runes_end = next(i for i, l in enumerate(text) if l.startswith("PAGE\trunewords\t"))
        for line in text[runes_start:runes_end]:
            parts = line.split("\t")
            if parts[0] == "ENTRY":
                current = parts[1]
                counts[current] = 0
            elif parts[0] == "BLOCK":
                counts[current] += 1
        self.assertEqual(counts["Kanji Runes"], 47)
        self.assertEqual(counts["LoD Runes"], 33)
        self.assertEqual(counts["Elemental Runes"], 14)

    def test_website_runes_page(self):
        text = [l.split("\t") for l in self.text]
        start = next(i for i, p in enumerate(text) if p[0] == "PAGE" and p[1] == "runes")
        end = next(i for i in range(start + 1, len(text)) if text[i][0] == "PAGE")
        cards, title, group = [], None, None
        for p in text[start:end]:
            if p[0] == "ENTRY":
                group = p[1]
            elif p[0] == "BLOCK":
                title = {"name": p[2], "group": group, "lines": [], "img": None, "level": None}
                cards.append(title)
            elif p[0] == "LINE":
                title["lines"].append(p[2])
            elif p[0] == "IMG":
                title["img"] = p[1]
            elif p[0] == "FIELD":
                title["level"] = p[2]
        by = {(c["group"], c["name"]): c for c in cards}
        # Same cells are shown once; the website's own labels are used.
        self.assertEqual(by[("Kanji Runes", "I Rune")]["lines"], [
            "\u00ffc0Weapons / Gloves:", "\u00ffc3+15% Enhanced Damage",
            "\u00ffc0Helms / Boots / Armor / Shields / Belts:", "\u00ffc3+30 Defense"])
        self.assertEqual(by[("Kanji Runes", "Shi Rune")]["lines"], ["\u00ffc0All Items:", "\u00ffc3+5 to Mana after each Kill"])
        self.assertEqual(by[("Kanji Runes", "Null Rune")]["lines"], [])                 # empty on the website
        self.assertEqual(by[("Elemental Runes", "Moon Rune")]["lines"][0], "\u00ffc0Weapons / Gloves:")
        self.assertEqual(by[("Kanji Runes", "I Rune")]["level"], "2")
        self.assertEqual(by[("LoD Runes", "Hel Rune")]["level"], "0")                    # as the website gives it
        # Ko is both a Kanji and a LoD rune, each with its own art.
        self.assertEqual(by[("Kanji Runes", "Ko Rune")]["img"], "items_misc_rune_ko_rune")
        self.assertEqual(by[("LoD Runes", "Ko Rune")]["img"], "items_misc_rune_ko_lod_rune")
        self.assertFalse(any(" Rune" not in c["name"] for c in cards if c["group"] != "How to Acquire Elemental Runes"))
        self.assertTrue(any("Catalyst" in l for c in cards for l in c["lines"]))        # the fixed how-to note is kept
        # (str.splitlines would cut the cells at the line-break separator, so read the page by "\n" only.)
        raw = (OVERLAY / "data" / "wiki-data.txt").read_text(encoding="utf-8").split("\n")
        rows = [l.split("\t", 2)[2] for l in raw if l.startswith("LINE\tR\t") and "\x1fÿc3" in l and "Rune" in l.split("\x1f")[1]
                and l.split("\x1f")[1][3:].split(" ")[0] in {"Moon", "Fire", "Water", "Wood", "Metal", "Earth", "Sun", "Thunder", "Wind", "Dragon", "Life", "Death", "Heaven", "God"}]
        self.assertEqual(len(rows), 14)                                                 # from the cube recipes page
        self.assertIn("Null Rune\x1eFrozen Soul\x1f\u00ffc3Moon Rune", rows)       # laid out like the cube recipes

    def test_website_runewords_and_gemwords_pages(self):
        def cards(page):
            out, cur, on = {}, None, False
            for l in self.text:
                p = l.split("\t")
                if p[0] == "PAGE":
                    on = p[1] == page
                elif on and p[0] == "BLOCK":
                    cur = out.setdefault(p[2], {"head": p, "lines": [], "runes": [], "fields": {}})
                elif on and cur is not None:
                    if p[0] == "LINE":
                        cur["lines"].append(p[2])
                    elif p[0] == "RUNE":
                        cur["runes"].append(p[1:])
                    elif p[0] == "FIELD":
                        cur["fields"][p[1]] = p[2]
            return out
        words = cards("runewords")
        enigma = words["Enigma"]
        self.assertEqual(enigma["fields"]["Sockets"], "3")
        self.assertEqual(enigma["head"][4], "3 Sockets")
        self.assertEqual(enigma["head"][7], "65")                          # the highest rune level, from the gems page
        self.assertEqual(enigma["runes"][0], ["items_misc_rune_jah_rune", "Jah", "lod"])
        self.assertIn("\u00ffc7Jah Rune + Ith Rune + Ber Rune", enigma["lines"])
        gems = cards("gemwords")
        holy = gems["Holy"]
        self.assertEqual(holy["head"][6], "Holy")                           # filter family = the gemword
        self.assertEqual(holy["runes"], [])                                 # gemword cards show no rune row

    def test_runeword_rune_bonuses_follow_the_category(self):
        page = cat = name = None
        bonus = {}
        for l in self.text:
            p = l.split("\t")
            if p[0] == "PAGE":
                page = p[1]
            elif p[0] == "ENTRY":
                cat = p[1]
            elif p[0] == "BLOCK":
                name = p[2]
                if page == "runewords":
                    bonus[(cat, name)] = None
            elif p[0] == "LINE" and page == "runewords":
                if p[2] == "\u00ffc4Rune Bonuses:":
                    bonus[(cat, name)] = []
                elif bonus.get((cat, name)) is not None and not p[2].startswith(("\u00ffc5", "\u00ffc0")):
                    bonus[(cat, name)].append(p[2])
        self.assertIsNone(bonus[("Charms", "Brandish")])                       # charms have no rune bonuses
        self.assertIn("\u00ffc3+15% Enhanced Damage", bonus[("Weapons", "Brandish")])
        self.assertEqual(bonus[("Belts", "Stone")], ["\u00ffc3+30 Defense", "\u00ffc3+5 to Mana after each Kill"])

    def test_ias_calculator_tab(self):
        pages = [l.split("\t") for l in self.text if l.startswith("PAGE\t")]
        self.assertEqual(pages[-1][1:4], ["ias-calculator", "IAS Calculator", "Endgame"])  # the second row of tabs
        self.assertEqual(len(self.entries["ias-calculator"]), 1)

    def test_ascendancy_page_follows_the_website(self):
        start = self.text.index("PAGE\tascendancy\tAscendancy\tWiki")
        page = self.text[start:]
        for k in range(1, len(page)):
            if page[k].startswith("PAGE\t"):
                page = page[:k]
                break
        entries = [l.split("\t")[1] for l in page if l.startswith("ENTRY\t")]
        self.assertEqual(entries[:2], ["Ascendancies", "Higher Tier Challenges"])
        self.assertEqual(entries[2:], sorted(entries[2:], key=str.lower))
        self.assertEqual(len(entries), 17)
        text = "\n".join(page)
        self.assertIn("Orb of Creation / Material Locations", text)
        self.assertIn("LINE\tC\tInput\x1fOutput", text)
        self.assertIn("Soul Warden Ascendancy Stone", text)
        self.assertEqual(text.count("LINE\tC\tInput\x1fOutput"), 15)
        self.assertIn("The second tier is achieved by killing Nihalthak", text)
        self.assertEqual(text.count("\u00ffc4Tier 1:\u00ffc0"), 15)
        self.assertEqual(text.count("\u00ffc4Tier 5:\u00ffc0"), 15)
        self.assertIn("Annihilation can proc only when you are below 30% current life.", text)
        self.assertIn("You need to set a hotkey for stance dancing", text)

    def test_sets_page_follows_the_website(self):
        start = self.text.index("PAGE\tset-items\tSet Items\tWiki")
        page = self.text[start:]
        for k in range(1, len(page)):
            if page[k].startswith("PAGE\t"):
                page = page[:k]
                break
        entries = [l.split("\t")[1] for l in page if l.startswith("ENTRY\t")]
        self.assertEqual(entries, ["Generic", "Amazon", "Barbarian", "Necromancer", "Paladin",
                                   "Sorceress", "Assassin", "Druid", "Warlock", "Hireling"])
        self.assertEqual(sum(1 for l in page if l == "LINE\tH\tSet bonus"), 77)
        self.assertIn("FIELD\tBase\tRing", page)

    def test_endgame_maps_page_follows_the_website(self):
        start = self.text.index("PAGE\tendgame-maps\tEndgame Maps\tEndgame")
        page = self.text[start:]
        for k in range(1, len(page)):
            if page[k].startswith("PAGE\t"):
                page = page[:k]
                break
        entries = [l.split("\t")[1] for l in page if l.startswith("ENTRY\t")]
        self.assertEqual(entries[0], "Overview")
        self.assertEqual(len(entries), 19)
        self.assertEqual(entries[1], "Lucion's Whisper - Cellar of Pain")
        self.assertEqual(sum(1 for l in page if l.startswith("IMG\t")), 18)
        self.assertIn("LINE\tT\t\u00ffcPOrb of Anointment\u00ffcN (5% chance on average)", page)
        self.assertIn("LINE\tH\t\u00ffcHRewards (for each boss)", page)

    def test_special_crafting_page_follows_the_website(self):
        start = self.text.index("PAGE\tcorruption\tSpecial Craft\tEndgame")
        page = self.text[start:]
        for k in range(1, len(page)):
            if page[k].startswith("PAGE\t"):
                page = page[:k]
                break
        entries = [l.split("\t")[1] for l in page if l.startswith("ENTRY\t")]
        self.assertEqual(entries, ["Corruption Outcomes", "Anointment Outcomes", "Sanctification Outcomes",
                                   "Standard of Heroes Outcomes"])
        self.assertEqual(sum(1 for l in page if l.startswith("LINE\tR\t")), 532)
        self.assertIn("BLOCK\t0\t\t\tWeapon\t\tWeapon\t0", page)
        self.assertIn("BLOCK\t0\t\t\tAny Jewel\t\t\t0", page)
        self.assertIn("LINE\tR\t\u00ffc3Same Item: *** Upgraded ***; +(2 to 3)% Bonus to All Attributes", page)

    def test_skill_information_page_follows_the_website(self):
        start = self.text.index("PAGE\tmerc-oskill\tMercenary/O-Skill\tEndgame")
        page = self.text[start:]
        for k in range(1, len(page)):
            if page[k].startswith("PAGE\t"):
                page = page[:k]
                break
        entries = [l.split("\t")[1] for l in page if l.startswith("ENTRY\t")]
        self.assertEqual(entries, ["Mercenaries", "Oskills"])
        self.assertEqual(sum(1 for l in page if l.startswith("LINE\tR\t")), 54)
        self.assertIn("LINE\tC\tHireling\x1fSubtype\x1fSkills", page)
        self.assertIn("LINE\tT\tHere you can find information about mercenaries and various oskills.", page)

    def test_vessel_of_souls_page_follows_the_website(self):
        start = self.text.index("PAGE\tvessel-of-souls\tVessel of Souls\tEndgame")
        page = self.text[start:]
        for k in range(1, len(page)):
            if page[k].startswith("PAGE\t"):
                page = page[:k]
                break
        entries = [l.split("\t")[1] for l in page if l.startswith("ENTRY\t")]
        self.assertEqual(entries, ["Overview", "Tier 1", "Tier 2", "Tier 3", "Tier 4", "Completion"])
        self.assertEqual(sum(1 for l in page if l.startswith("LINE\tR\t")), 409)
        self.assertIn("LINE\tR\t2x \u00ffcRHeart\u00ffcN\x1e\u00ffcRBrain\u00ffcN\x1e\u00ffcOTail\u00ffcN\x1e\u00ffcNQuill\u00ffcN\x1f+5% Chance to Cast Level 15 Thrown Axe on Striking\x1f350 Souls", page)
        self.assertIn("LINE\tR\t\u00ffcPOrb of Anointment\u00ffcN (with 5 souls remaining)\x1f\u00ffcPWeapon Anointment Bonus\u00ffcN\x1f-5 Souls", page)

    def test_weapon_mastery_page_follows_the_website(self):
        start = self.text.index("PAGE\tweapon-mastery\tWeapon Mastery\tEndgame")
        page = self.text[start:]
        for k in range(1, len(page)):
            if page[k].startswith("PAGE\t"):
                page = page[:k]
                break
        entries = [l.split("\t")[1] for l in page if l.startswith("ENTRY\t")]
        self.assertEqual(entries[:2], ["Overview", "General Recipes"])
        self.assertIn("Stag Bow Variant", entries)
        self.assertEqual(len(entries), 20)  # Overview, General Recipes and 18 specialization variants
        self.assertEqual(sum(1 for l in page if l.startswith("LINE\tR\t")), 31)  # 3 general rows, 28 skill rows
        self.assertIn("LINE\tR\t\u00ffcHAwakened Weapon Mastery Token\u00ffcN\x1e\u00ffcYSpider's Silk\u00ffcN\x1e"
                      "\u00ffcPAnvil Stone\u00ffcN\x1f\u00ffcOStrafe\u00ffcN\x1fTriples the number of targets", "\n".join(page))

    def test_kill_ledger_page_follows_the_website(self):
        start = self.text.index("PAGE\tkill-ledger\tKill Ledger\tEndgame")
        page = self.text[start:]
        for k in range(1, len(page)):
            if page[k].startswith("PAGE\t"):
                page = page[:k]
                break
        entries = [l.split("\t")[1] for l in page if l.startswith("ENTRY\t")]
        self.assertEqual(entries, ["Overview", "Kill Bonuses", "Boss Bonuses"])
        self.assertEqual(sum(1 for l in page if l.startswith("LINE\tR\t")), 24)
        self.assertIn("LINE\tR\t500 kills\x1fAny Flawless Gem\x1f+10% to All Resistances", page)
        self.assertIn("LINE\tR\t\u00ffcNMaple Leaf\u00ffcN\x1e\u00ffcRFlawed Bleeding Stone\u00ffcN\x1fLucion\x1f+100 to Life", page)

    def test_website_item_affixes_page(self):
        self.assertEqual(list(self.entries["item-affixes"]), ["Armor", "Weapons", "Other"])
        rows = {"Prefix": 0, "Suffix": 0, "Superior": 0}
        families = {}
        entry = block = tier = page = None
        for line in self.text:
            parts = line.split("\t")
            if parts[0] == "PAGE":
                page = parts[1]
            elif page == "item-affixes" and parts[0] == "ENTRY":
                entry = parts[1]
                families[entry] = set()
            elif page == "item-affixes" and parts[0] == "BLOCK":
                block, tier = parts[6], parts[5]  # item type, affix kind
                families[entry].add(block)
            elif page == "item-affixes" and parts[0] == "LINE" and parts[1] == "R" and tier in rows:
                rows[tier] += 1
        # 7890 / 15026 / 1428 rows on the website, less the charm and jewel tables it lists twice
        self.assertEqual(rows, {"Prefix": 7890 - 353 - 453 - 87 - 3, "Suffix": 15026 - 331 - 954 - 222, "Superior": 1428 - 83})
        self.assertIn("Helm", families["Armor"])
        self.assertIn("Amazon Bow", families["Weapons"])
        self.assertTrue({"Ring", "Amulet", "Small Charm", "Jewel"} <= families["Other"])
        self.assertTrue(not {"General", "voib", "Bow Quiver / Crossbow Quiver", "Item Type Keys"} & families["Other"])
        other = [p[6] for p in (l.split("\t") for l in self.text[self.text.index("PAGE\titem-affixes\tItem Affixes\tWiki"):]) if p[0] == "BLOCK"]
        seen = [x for i, x in enumerate(other) if x not in other[:i]]
        self.assertEqual(seen[seen.index("Ring") + 1], "Amulet")
        self.assertEqual(seen[seen.index("Small Charm") + 1], "Jewel")
        self.assertNotIn("Helm", families["Weapons"])
        # every entry allows the row sorts, and the rows of each table come alphabetically by name
        start = self.text.index("PAGE\titem-affixes\tItem Affixes\tWiki")
        page = self.text[start:]
        for k in range(1, len(page)):
            if page[k].startswith("PAGE\t"):
                page = page[:k]
                break
        entries = [l for l in page if l.startswith("ENTRY\t")]
        self.assertEqual(len(entries), 3)
        self.assertTrue(all(l.split("\t")[-1] == "rowsort" for l in entries))
        names, tables = [], 0
        for l in page:
            parts = l.split("\t")
            if parts[0] == "BLOCK" or (parts[0] == "LINE" and parts[1] == "C"):
                if len(names) > 1:
                    self.assertEqual(names, sorted(names), "rows are not in name order")
                names = []
                tables += 1
            elif parts[0] == "LINE" and parts[1] == "R" and "\x1f" in parts[2] and parts[2].startswith("\u00ffc3"):
                names.append(parts[2].split("\x1f")[0].replace("\u00ffc3", "").lower())
        self.assertGreater(tables, 100)

    def test_website_item_bases_page(self):
        blocks, page, cur = {}, None, None
        for l in self.text:
            p = l.split("\t")
            if p[0] == "PAGE":
                page = p[1]
            elif p[0] == "BLOCK" and page == "item-bases":
                cur = blocks.setdefault(p[2], {"head": p, "fields": {}, "img": None})
            elif p[0] == "FIELD" and page == "item-bases" and cur is not None:
                cur["fields"].setdefault(p[1], p[2])
            elif p[0] == "IMG" and page == "item-bases" and cur is not None:
                cur["img"] = p[1]
        quilted = blocks["Quilted Armor"]
        self.assertEqual(quilted["head"][3:8], ["Normal qui", "Armor", "Normal", "Armor", "1"])
        self.assertEqual(quilted["fields"]["Defense"], "18-22")
        self.assertNotIn("Mag Lvl", quilted["fields"])                          # empty website cells are left out
        self.assertEqual(blocks["Maiden Javelin"]["fields"]["Missile"], "6 to 22 (14.0 Avg)")
        self.assertEqual(blocks["Maiden Javelin"]["head"][6], "Amazon Javelin")  # type filter = the website table
        self.assertTrue(all(b["img"] for b in blocks.values()))

    def test_item_blocks_carry_excel_fields(self):
        if not (OVERLAY / "data" / "wiki-data.txt").exists():
            self.skipTest("no generated data")
        robes = dict(self.fields.get("Autolycus' Robes", []))
        self.assertEqual(robes.get("Set"), "Autolycus' Magic Tools")
        self.assertEqual(robes.get("Base"), "Quilted Armor")
        self.assertEqual(robes.get("Required Level"), "3")
        self.assertEqual(robes.get("Set pieces"), "3")
        ring = dict(self.fields.get("Alakashar Rahl", []))
        self.assertEqual(ring.get("Base"), "Druid's Seal")
        self.assertEqual(ring.get("Required Level"), "60")

    def _block_images(self, title):
        """Images of the first block with this title, from the raw data."""
        lines = self.text
        for index, line in enumerate(lines):
            parts = line.split("\t")
            if parts[0] == "BLOCK" and len(parts) > 2 and parts[2] == title:
                images = []
                for follow in lines[index + 1:]:
                    tag = follow.split("\t")[0]
                    if tag in ("BLOCK", "ENTRY", "PAGE"):
                        break
                    if tag == "IMG":
                        images.append(follow.split("\t")[1])
                return images
        return None

    def test_class_crafted_items_each_have_their_own_art(self):
        # Every class item is its own block with one image, so stats never repeat per image.
        self.assertEqual(self._block_images("Amazon Crafted Helm"), ["items_armor_circlet_diadem"])
        self.assertEqual(self._block_images("Amazon Crafted Boots"), ["items_armor_boot_heavy_boots"])
        self.assertEqual(self._block_images("Amazon Crafted Gloves"), ["items_armor_glove_heavy_gloves"])
        crafting = self.text[next(i for i, line in enumerate(self.text) if line.startswith("PAGE\tclass-crafting\tClass Crafting\t")):]
        crafting = crafting[:next(i for i, line in enumerate(crafting) if i > 0 and line.startswith("PAGE\t"))]
        image_counts = []
        for line in crafting:
            if line.startswith("BLOCK\t1\t"):
                image_counts.append(0)
            elif line.startswith("IMG\t") and image_counts:
                image_counts[-1] += 1
        self.assertTrue(image_counts)
        self.assertTrue(all(count <= 1 for count in image_counts), image_counts)

    def test_runewords_carry_socket_sections(self):
        self.assertEqual(self._section("Boar"), "1 Socket")
        self.assertEqual(self._section("Ist Rune"), "")

    def test_set_items_carry_their_set_name_as_section(self):
        self.assertEqual(self._section("Autolycus' Robes"), "Autolycus' Magic Tools")

    def _section(self, title):
        for line in self.text:
            parts = line.split("\t")
            if parts[0] == "BLOCK" and len(parts) > 2 and parts[2] == title:
                return parts[4] if len(parts) > 4 else ""
        return None

    def test_class_set_cards_follow_setitems(self):
        """Every non-hireling set card sits under its set's header, each set's bonus card follows its pieces,
        and item lines come from setitems.txt (Naj's Light Plate: 48/8 = +6 Defense per level)."""
        page = entry = None
        blocks = []
        for line in self.text:
            parts = line.split("\t")
            if parts[0] == "PAGE":
                page = parts[1]
            elif parts[0] == "ENTRY":
                entry = parts[1]
            elif parts[0] == "BLOCK" and page == "set-items" and entry != "Hireling":
                blocks.append({"title": parts[2], "section": parts[4], "entry": entry, "lines": []})
            elif parts[0] == "LINE" and page == "set-items" and entry != "Hireling" and blocks:
                blocks[-1]["lines"].append(parts[2])
        self.assertTrue(blocks)
        self.assertTrue(all(b["section"] for b in blocks))
        sections = [(b["entry"], b["section"]) for b in blocks]   # a class can have a set with the same name as another class's
        seen = []
        for section in sections:
            if not seen or seen[-1] != section:
                self.assertNotIn(section, seen)   # a set's cards are in one run
                seen.append(section)
        plate = next(b for b in blocks if b["title"] == "Naj's Light Plate")
        self.assertIn("\u00ffcC+6 Defense (Based on Character Level)", plate["lines"])
        bonus = [b for b in blocks if "Set bonus" in b["lines"]]
        self.assertEqual(len(bonus), len({(b["entry"], b["section"]) for b in blocks}))

    def test_hireling_cards_follow_setitems_by_difficulty(self):
        """The Hireling entry is laid out like the Warlock sets (a header per set, its pieces, then its bonus
        card), lists Normal, then Nightmare, then Hell, and each piece carries its own base item and required
        level from setitems.txt."""
        blocks = []
        page = entry = None
        flags = []
        for line in self.text:
            parts = line.split("\t")
            if parts[0] == "PAGE":
                page = parts[1]
            elif parts[0] == "ENTRY":
                entry = parts
                if page == "set-items" and parts[1] == "Hireling":
                    flags = parts[3:]
            elif parts[0] == "BLOCK" and page == "set-items" and entry[1] == "Hireling":
                blocks.append({"title": parts[2], "section": parts[4], "tier": parts[5], "level": int(parts[7]), "fields": {}})
            elif parts[0] == "FIELD" and page == "set-items" and entry[1] == "Hireling" and blocks:
                blocks[-1]["fields"][parts[1]] = parts[2]
        self.assertEqual(len(blocks), 51)
        self.assertNotIn("sortable", flags)
        self.assertEqual(blocks[0]["section"], "Kashya's Survival Kit (Normal, Lvl 10)")
        self.assertEqual(blocks[3]["title"], "Kashya's Survival Kit")  # the set bonus card follows its pieces
        self.assertEqual(blocks[3]["section"], blocks[0]["section"])
        tiers = [b["tier"] for b in blocks]
        self.assertEqual(tiers, sorted(tiers, key=["Normal", "Nightmare", "Hell"].index))
        hell_suit = next(b for b in blocks if b["title"] == "Rogue's Suit" and b["tier"] == "Hell")
        self.assertEqual(hell_suit["fields"]["Base"], "Viper Skin")
        self.assertEqual(hell_suit["fields"]["Required Level"], "50")
        self.assertEqual(hell_suit["level"], 50)

    def test_manifest_covers_every_referenced_image(self):
        manifest = (OVERLAY / "data" / "image-manifest.txt").read_text(encoding="utf-8").splitlines()
        keys = {row.split("\t")[0] for row in manifest if row}
        referenced = set()
        for line in self.text:
            if line.startswith("IMG\t") or line.startswith("RUNE\t"):
                referenced.add(line.split("\t")[1])
        # Website mythical art and endgame map art are copied in directly, not through the manifest.
        art = {path.stem for path in (OVERLAY / "data" / "images").glob("mythical_*.png")}
        art |= {path.stem for path in (OVERLAY / "data" / "images").glob("endgame_*.png")}
        self.assertEqual(referenced - keys - art, set())


class CubePageTests(unittest.TestCase):
    """The cube recipes page is read by two programs, Python for the shipped data and C++ in game.
    Both must give exactly tests/cube_live/cube-wiki-data.expected.txt for the saved page."""

    def test_python_pipeline_matches_expected_output(self):
        import subprocess
        import tempfile
        tools = OVERLAY / "tools"
        fixtures = OVERLAY / "tests" / "cube_live"
        with tempfile.TemporaryDirectory() as folder:
            cube = Path(folder) / "cube-data.txt"
            wiki = Path(folder) / "cube-wiki-data.txt"
            subprocess.run([sys.executable, "-I", str(tools / "build_cube.py"),
                            str(fixtures / "cube-recipes-page.html"), str(cube)], check=True, capture_output=True)
            subprocess.run([sys.executable, "-I", str(tools / "build_cube_wiki.py"), str(cube), str(wiki)],
                           check=True, capture_output=True)
            self.assertEqual(wiki.read_text(encoding="utf-8"),
                             (fixtures / "cube-wiki-data.expected.txt").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()


class ModOnlyUniqueTests(unittest.TestCase):
    """The Uniques page is written from the mod's tables only (uniqueitems.txt, itemstatcost.txt, strings)."""

    @classmethod
    def setUpClass(cls):
        # The mod-built Uniques page is kept beside the shipped data as a check of the mod's own tables.
        path = OVERLAY / "data" / "wiki-data.mod-uniques.txt"
        if not path.exists():
            raise unittest.SkipTest("no mod-built uniques page")
        lines = path.read_text(encoding="utf-8").split("\n")
        start = next(i for i, l in enumerate(lines) if l.startswith("PAGE\tuniques"))
        end = next(i for i, l in enumerate(lines) if l.startswith("PAGE\t") and i > start)
        cls.cards = {}
        current = None
        for line in lines[start:end]:
            parts = line.split("\t")
            if parts[0] == "BLOCK":
                current = cls.cards.setdefault(parts[2], [])
            elif parts[0] == "LINE" and current is not None:
                current.append(re.sub("\u00ffc.", "", parts[2]))

    def test_no_doubled_signs_or_unfilled_holes(self):
        for title, lines in self.cards.items():
            for line in lines:
                self.assertNotIn("--", line, title)
                self.assertNotRegex(line, r"%[+]?[dis]", title)

    def test_values_come_from_the_tables(self):
        self.assertIn("+400-500% Enhanced Defense", self.cards["Stoneraven"])
        self.assertIn("Adds 19200-38400 Poison Damage to Attacks over 3 seconds", self.cards["Viperfork"])
        self.assertIn("+50% Piercing Attack", self.cards["Ancient Piercer"])
        self.assertIn("Requirements -100%", self.cards["Free Lance"])

    def test_description_lines_use_the_mods_strings(self):
        self.assertTrue(any(l.startswith("Your cold damage to attacks can crit") for l in self.cards["Raven Frost"]))

    def test_website_uniques_page(self):
        text = [l.split("\t") for l in (OVERLAY / "data" / "wiki-data.txt").read_text(encoding="utf-8").split("\n")]
        start = next(i for i, p in enumerate(text) if p[0] == "PAGE" and p[1] == "uniques")
        end = next(i for i in range(start + 1, len(text)) if text[i][0] == "PAGE")
        page = text[start:end]
        self.assertEqual(page[0][2], "Uniques")
        lines = {}
        title = None
        for p in page:
            if p[0] == "BLOCK":
                title = p[2]
                lines[title] = []
            elif p[0] == "LINE" and title:
                lines[title].append(p[2])
        self.assertIn("\u00ffc01 of the following:", lines["Opalvein"])
        self.assertIn("\u00ffc8if you wear a spell damage opalvein", lines["Opalvein"])      # orange from the page
        self.assertIn("\u00ffc3+2-4 to All Skills", lines["Viperfork"])                       # "(2 to 4)" written as a range
        self.assertFalse(any("Tinkering" in l for ls in lines.values() for l in ls))
        self.assertTrue(any(p[0] == "IMG" for p in page))
        # Charms carry no gamble or coupon tag.
        charms = [i for i, p in enumerate(page) if p[0] == "ENTRY" and p[1] == "Charms"][0]
        after = next((i for i in range(charms + 1, len(page)) if page[i][0] == "ENTRY"), len(page))
        self.assertFalse(any(p[0] == "TAG" for p in page[charms:after]))
        # An orange run that starts after a blank gap is orange from its first line.
        art = (OVERLAY / "data" / "web-unique-art.txt").read_text(encoding="utf-8").split("\n")
        self.assertTrue(any(l.startswith("BASE\t7br\t") and l.endswith("Elite\tSpears") for l in art))
        self.assertTrue(any(l.startswith("UNIQUE\tViperfork\t") for l in art))
        blocks = {p[2]: p for p in page if p[0] == "BLOCK"}
        self.assertEqual(blocks["Viperfork"][5], "Elite")                   # the tier of its base item
        self.assertEqual({p[5] for p in blocks.values()} - {""}, {"Normal", "Exceptional", "Elite"})
        self.assertEqual(blocks["Viperfork"][6], "Spear")                    # the website's own weapon category
        self.assertGreaterEqual(len({p[6] for p in blocks.values() if p[6]}), 30)
        self.assertEqual(blocks["Veil of Steel"][6], "Helm")
        self.assertEqual(lines["Aegis of Corruption"][0][:3], "\u00ffc8")
        self.assertEqual(lines["Aegis of Corruption"][1][:3], "\u00ffc8")

    def test_opalvein_shows_both_blocks_under_the_mods_header(self):
        lines = self.cards["Opalvein"]
        self.assertIn("1 of the following:", lines)
        self.assertIn("if you wear a spell damage opalvein", lines)
        self.assertIn("if you wear an enhanced damage opalvein", lines)
        self.assertIn("Socketed (1)", lines)
        self.assertTrue(any(l.startswith("You can bind Baal") for l in self.cards["Ars Tor'Baalos"]))
