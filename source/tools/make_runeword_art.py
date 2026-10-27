"""One-time export of the runeword card art from the old runeword carousel layout.

The carousel (data/runewordscarouselhd.json) is no longer read by the extractor. Its card art was
built to be varied within each item category (each runeword card shows a different weapon, armor or
helm picture) so the runeword cards do not all look identical. That assignment is kept in
data/runeword-art.json, which extract_wiki.py reads instead:

    order   runeword keys in the order the carousel listed them
    titles  card title text as the carousel had it (some carry a colour code)
    art     runeword key -> card category -> picture (a sprite path under the ui folder)

Run once from the overlay folder: python3 tools/make_runeword_art.py <carousel.json> data/runeword-art.json
Edit data/runeword-art.json by hand to change a card's picture.
"""
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import extract_wiki as ew  # noqa: E402


def main(source, target):
    root = ew.load_layout(Path(source))
    order, titles, art = [], {}, {}
    for node, _ in ew.walk(root):
        if node.get("type") != "Widget" or not node.get("name", "").startswith("card_"):
            continue
        found = re.search(r"EsrWikiRuneword_(Runeword\d+)", json.dumps(node))
        if not found:
            continue
        key = found.group(1)
        if key not in order:
            order.append(key)
        for child, _ in ew.walk(node):
            text = ew.fields(child).get("text")
            if child.get("type") == "TextBoxWidget" and isinstance(text, str) and ew.clean(text):
                titles.setdefault(key, ew.clean(text))
                break
        category = ew.runeword_category_of_card(node["name"])
        for child, _ in ew.walk(node):
            filename = ew.fields(child).get("filename")
            if child.get("type") == "ImageWidget" and ew.is_content_image(filename):
                art.setdefault(key, {}).setdefault(category, filename)
                break
    out = {"_note": "Card art was built to vary within each item category so runeword cards do not all look identical. Exported from the old carousel; see tools/make_runeword_art.py.",
           "order": order, "titles": titles, "art": art}
    Path(target).write_text(json.dumps(out, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print("runewords", len(order), "with art", len(art))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
