#!/usr/bin/env python3
"""IAS Calculator page of wiki-data.txt, from the saved pages of the ESR website's Frame Table.

    python web_ias.py <ias-calculator.html> <ias-calculator-data.js> <wiki-data.txt>

The calculator itself is drawn by the overlay from the two saved website files (ias-calculator.html for its constants,
ias-calculator-data.js for the classes, forms, weapons and animation frames); this only checks that they look like the
expected pages and adds the page (a tab in the second row, with one placeholder entry) to wiki-data.txt, replacing any
earlier one. Standard library only. Anything that goes wrong leaves wiki-data.txt untouched and exits 1.
The C++ engine that reads the files is src/ias_calc.cpp; tests/ias_calc_test.cpp compares it with the page's script.
"""
import json
import re
import sys
from pathlib import Path

PAGE_ID = "ias-calculator"
PAGE_TITLE = "IAS Calculator"
PAGE_GROUP = "Endgame"  # the second row of tabs
CONSTANTS = ("FCR_CAP", "WW_HIT_INTERVAL", "WW_ATTACKRATE_WEIGHT", "WW_STRETCH", "WW_MIN_FRAMES", "WW_MAX_RATE",
             "WW_MIN_RATE", "WW_NO_ANIM", "XBOW_FRAMES", "STRAFE_ROLLBACK", "SEQ_PENALTY")


def check(html, js):
    for name in CONSTANTS:
        if not re.search(r"var\s+%s\s*=\s*(\d+|true|false)\s*;" % name, html):
            raise SystemExit("the page no longer defines %s" % name)
    start = js.find("__DATA__")
    brace = js.find("{", start) if start >= 0 else -1
    if brace < 0:
        raise SystemExit("the data file has no __DATA__")
    try:
        data, _ = json.JSONDecoder().raw_decode(js[brace:])
    except ValueError:
        raise SystemExit("the data file is incomplete")
    if len(data.get("c", [])) < 5 or len(data.get("w", [])) < 100 or not data.get("f"):
        raise SystemExit("the data file has too little in it; the website pages look broken")
    return data


def page_lines():
    return [
        "PAGE\t%s\t%s\t%s" % (PAGE_ID, PAGE_TITLE, PAGE_GROUP),
        "ENTRY\tCalculator\tAttack and cast breakpoints",
        "BLOCK\t0\t\t\t\t\t\t0",
        "LINE\tT\tThe calculator needs its data from the website. Open this tab once the plugin has loaded it.",
    ]


def splice(lines):
    starts = [i for i, l in enumerate(lines) if l.startswith("PAGE\t")]
    for n, i in enumerate(starts):
        if lines[i].split("\t")[1] == PAGE_ID:
            end = starts[n + 1] if n + 1 < len(starts) else len(lines)
            return lines[:i] + page_lines() + lines[end:]
    return lines + page_lines()


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    html_path, js_path, target = Path(argv[1]), Path(argv[2]), Path(argv[3])
    try:
        html = html_path.read_text(encoding="utf-8", errors="replace")
        if "</html" not in html.lower():
            raise SystemExit("%s is incomplete" % html_path.name)
        data = check(html, js_path.read_text(encoding="utf-8", errors="replace"))
        lines = target.read_text(encoding="utf-8").split("\n")
        trailing = bool(lines) and lines[-1] == ""
        if trailing:
            lines = lines[:-1]
        merged = splice(lines)
    except SystemExit as stop:
        print("Website IAS calculator: %s" % stop)
        return 1
    tmp = target.with_suffix(target.suffix + ".new")
    tmp.write_text("\n".join(merged) + ("\n" if trailing else ""), encoding="utf-8", newline="\n")
    tmp.replace(target)
    print("Website IAS calculator: %d classes, %d forms, %d weapons" % (len(data["c"]), len(data["f"]), len(data["w"])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
