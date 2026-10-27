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
        cls.text = cls.path.read_text(encoding="utf-8").splitlines()
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
        self.assertEqual(len(self.entries), 17)  # cube recipes moved to the cube overlay; Endgame Maps, Weapon Mastery and Item Bases added

    def test_category_and_block_counts(self):
        expected = {
            "runes": (4, 95),
            "runewords": (8, 415),
            "set-items": (10, 342),
            "uniques": (13, 1108),
            "gemwords": (4, 157),
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
        # Blocks per rune family: 46 Kanji, 33 LoD, 15 ESR (r50 and r84-r97).
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
        self.assertEqual(counts["Kanji Runes"], 46)
        self.assertEqual(counts["LoD Runes"], 33)
        self.assertEqual(counts["Elemental Runes"], 15)

    def test_item_blocks_carry_excel_fields(self):
        if not (OVERLAY / "data" / "wiki-data.txt").exists():
            self.skipTest("no generated data")
        robes = dict(self.fields.get("Autolycus' Robes", []))
        self.assertEqual(robes.get("Set"), "Autolycus' Magic Tools")
        self.assertEqual(robes.get("Base"), "Quilted Armor")
        self.assertEqual(robes.get("Required Level"), "3")
        self.assertEqual(robes.get("Set pieces"), "3")
        ring = dict(self.fields.get("Alakashar Rahl", []))
        self.assertEqual(ring.get("Base"), "Ring")
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
                blocks.append({"title": parts[2], "section": parts[4], "lines": []})
            elif parts[0] == "LINE" and page == "set-items" and entry != "Hireling" and blocks:
                blocks[-1]["lines"].append(parts[2])
        self.assertTrue(blocks)
        self.assertTrue(all(b["section"] for b in blocks))
        sections = [b["section"] for b in blocks]
        seen = []
        for section in sections:
            if not seen or seen[-1] != section:
                self.assertNotIn(section, seen)   # a set's cards are in one run
                seen.append(section)
        plate = next(b for b in blocks if b["title"] == "Naj's Light Plate")
        self.assertIn("\u00ffcC+6 Defense (Based on Character Level)", plate["lines"])
        bonus = [b for b in blocks if "Set bonus" in b["lines"]]
        self.assertEqual(len(bonus), len({b["section"] for b in blocks}))

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


if __name__ == "__main__":
    unittest.main()
