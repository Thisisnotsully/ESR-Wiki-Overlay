"""Convert data/cube-data.txt (the Horadric Cube recipes) into the v2 wiki format
used by the overlay renderer, writing data/cube-wiki-data.txt.

Each category becomes a PAGE and each section an ENTRY. Notes are text lines. Recipes are
tables (Ingredients / Result), one table per run of recipes, the same format as the wiki's
Forging and D-Stone pages. Colour codes are stripped from the table cells, as in those pages.
"""
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
DATA = HERE.parent / "data"
sys.path.insert(0, str(HERE))
from extract_wiki import COLOR_BLUE, TABLE_SEPARATOR, split_cube_sections, tidy_recipe  # noqa: E402

# Pages laid out in the same sections as the main wiki's tab (same split, same titles).
SECTIONED_PAGES = {"for", "dst"}
# The Item Reroll groups. A group's sub-sections stay in the group's one entry: each block carries its sub-section's
# name as its family, so the overlay's Type filter picks between them. Standard Reroll is in both groups.
GROUP_PAGE = "reroll"
SUB_GROUPS = {
    "Magic/Rare Items": ["Standard Reroll", "Starter's Weapon", "CtC Skill Armor"],
    "Rings/Amulets": ["Standard Reroll", "Custom Reroll", "Skill Tab Reroll"],
}


def clean(text):
    return text.replace("\t", " ").replace("\r", " ").strip()


def side(text):
    # cell lines are joined with " | " by the extractor; show them as " + "
    parts = [p.strip() for p in text.split(" | ") if p.strip()]
    return " + ".join(parts)


def blocks_of(items):
    """Splits one entry's items into (notes, recipes, family) blocks. A block is its notes followed by its recipe
    table, so a note after a table starts a new block; so does a change of sub-section (family)."""
    blocks, notes, recipes = [], [], []
    family = None
    for item in items:
        if item[-1] != family:
            if notes or recipes:
                blocks.append((notes, recipes, family))
                notes, recipes = [], []
            family = item[-1]
        if item[0] == "T":
            if recipes:
                blocks.append((notes, recipes, family))
                notes, recipes = [], []
            notes.append(item[1])
        else:
            recipes.append((item[1], item[2]))
    if notes or recipes:
        blocks.append((notes, recipes, family))
    return blocks


def sectioned(entry_list):
    """Entries re-cut into the wiki's sections: notes first, then the recipe table."""
    raw = [(title, [item[1] if item[0] == "T" else f"{item[1]}  ->  {item[2]}" for item in items])
           for title, _, items in entry_list]
    return [[section["title"], "", [("T", note, "") for note in section["notes"]]
             + [("R", recipe_in, recipe_out, "") for recipe_in, recipe_out in section["rows"]]]
            for section in split_cube_sections(raw) if section["notes"] or section["rows"]]


def main(src, out):
    rows = [line.rstrip("\n").split("\t") for line in open(src, encoding="utf-8")]
    assert rows and rows[0][0] == "#ESRCUBE", "not a cube-data file"
    pages = []           # (id, title, group)
    page_index = {}
    entries = {}         # id -> list of [title, subtitle, lines]
    current = {}         # id -> entry being filled
    open_group = {}      # id -> the open Item Reroll group heading
    family = {}          # id -> the sub-section being filled
    for row in rows[1:]:
        kind = row[0]
        if kind == "CAT":
            _, cid, title, group = row
            pages.append((cid, title, group))
            page_index[cid] = len(pages) - 1
            entries[cid] = []
            continue
        cid = row[1]
        members = SUB_GROUPS if cid == GROUP_PAGE else {}
        if kind == "SEC":
            title = clean(row[2])
            if open_group.get(cid) in members and title in members[open_group[cid]]:
                family[cid] = title  # a sub-section of the open group: its recipes stay in the group's entry
                continue
            open_group[cid] = title if title in members else None
            family[cid] = None
            entry = [title, "", []]
            entries[cid].append(entry)
            current[cid] = entry
        elif kind in ("DESC", "NOTE", "REC"):
            entry = current.get(cid)
            if entry is None:
                entry = [pages[page_index[cid]][1], "", []]
                entries[cid].append(entry)
                current[cid] = entry
            fam = family.get(cid) or ""
            if kind == "REC":
                entry[2].append(("R", clean(side(row[2])), clean(side(row[3])), fam))
            else:
                entry[2].append(("T", clean(row[2]), fam))

    for cid in SECTIONED_PAGES & set(entries):
        entries[cid] = sectioned(entries[cid])

    lines = ["#ESRWIKI\t2"]
    for cid, title, group in pages:
        lines.append("\t".join(["PAGE", cid, title, group]))
        for etitle, esub, elines in entries[cid]:
            lines.append("\t".join(["ENTRY", etitle, esub]))
            for notes, recipes, family_name in blocks_of(elines):
                # a sub-section of an Item Reroll group also gives its family (the Type filter)
                fields = ["BLOCK", "0", "", "", "", "", family_name, ""] if family_name else ["BLOCK", "0", "", "", ""]
                lines.append("\t".join(fields))
                for text in notes:
                    lines.append("\t".join(["LINE", "T", text]))
                if recipes:
                    lines.append("\t".join(["LINE", "C", TABLE_SEPARATOR.join(["Ingredients", "Result"])]))
                    for recipe_in, recipe_out in recipes:
                        # One ingredient and one result per line (U+001E is a line break inside a cell).
                        ins = [p.strip() for p in tidy_recipe(recipe_in).split(" + ") if p.strip()]
                        outs = [p.strip() for p in tidy_recipe(recipe_out).split(" + ") if p.strip()]
                        lines.append("\t".join(["LINE", "R", TABLE_SEPARATOR.join(
                            ["\x1e".join(ins), COLOR_BLUE + "\x1e".join(outs)])]))
    pathlib.Path(out).write_text("\n".join(lines) + "\n", encoding="utf-8")
    total = sum(len(e[2]) for es in entries.values() for e in es)
    print(f"pages={len(pages)} lines={total} -> {out}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else str(DATA / "cube-data.txt"),
         sys.argv[2] if len(sys.argv) > 2 else str(DATA / "cube-wiki-data.txt"))
