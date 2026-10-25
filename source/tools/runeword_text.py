"""Builds runeword stat text from the excel tables, using the display templates in properties.txt.

Each runeword's stat lines come from its T1Code/T1Param/T1Min/T1Max cells in runes.txt, in row order.
Hidden properties (names ending in -hidden) and properties without a template are not shown.
"""
import csv
import re
from pathlib import Path

SKIP_CODES = {"unsocketable"}


def read_rows(path):
    with open(path, encoding="utf-8", errors="replace", newline="") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def load_templates(folder):
    templates = {}
    for row in read_rows(Path(folder) / "properties.txt"):
        code = (row.get("code") or "").strip()
        tip = (row.get("*Tooltip") or "").strip()
        if code:
            templates[code] = tip
    return templates


def fill(template, param, low, high):
    """Fills a display template from a stat's min and max.

    Chance-and-level templates (hit-skill, kill-skill, gethit-skill, cast-skill, att-skill) take the
    chance from the minimum and the skill level from the maximum. Corpse Explosion's charged template
    takes the level from the maximum and the charges from the minimum. "#-#" takes the min-max range.
    """
    text = template.replace("[Class]", param or "Class")
    if "[Skill]" in text:
        if not param:
            return None
        text = text.replace("[Skill]", param)
    if text.startswith("Level # ") and "(#/# Charges)" in text:
        text = text.replace("(#/# Charges)", "(#/# Charges)")
        return text.replace("#", high, 1).replace("#", low, 1).replace("#", low, 1)
    if "Chance to Cast Level #" in text:
        return text.replace("#", low, 1).replace("#", high, 1)
    if "#-#" in text:
        if text.count("#") != 2:
            return None
        return text.replace("#-#", "%s-%s" % (low, high))
    if text.count("#") != 1:
        return None
    text = text.replace("#", value_text(low, high))
    return text.replace("+-", "-")


def value_text(low, high):
    if not high or low == high:
        return low
    return "%s-%s" % (low, high)


def runeword_lines(row, templates):
    """Stat lines for one runeword row, or None when a stat has no template."""
    lines = []
    for i in range(1, 8):
        code = (row.get("T1Code%d" % i) or "").strip()
        if not code or code in SKIP_CODES or code.endswith("-hidden"):
            continue
        template = templates.get(code)
        if template is None:
            return None
        if not template:
            continue
        param = (row.get("T1Param%d" % i) or "").strip()
        low = (row.get("T1Min%d" % i) or "").strip()
        high = (row.get("T1Max%d" % i) or "").strip()
        if not low and not high:
            return None
        text = fill(template, param, low, high or low)
        if text is None:
            return None
        lines.append(text)
    return lines
