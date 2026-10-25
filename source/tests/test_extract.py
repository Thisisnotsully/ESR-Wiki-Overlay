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
            "runewords": (8, 408),
            "set-items": (10, 342),
            "uniques": (13, 1104),
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
