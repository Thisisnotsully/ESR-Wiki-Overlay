#!/usr/bin/env python3
"""Drop Finder data, from the Drop Finder page of the ESR website (drop-finder.htm and its data script).

    python web_drop.py <drop-finder.htm> <data folder>

The page carries its data as JSON literals: window.DF_DATA (items, types, uniques, sets, treasure classes, monsters,
super uniques, zones and the formula constants), window.DF_NAMES (the names of each), window.DF_UI (the English
interface texts) and window.DF_LANG. The engine that turns them into drop chances is the page's own script, kept in
the saved page between //ENGINE-START and //ENGINE-END; tests/drop_finder_reference.js runs it to check the C++ port
(src/drop_finder.cpp). This writes df-data.json, df-names.json and df-ui.json and the engine script, next to each other
in the data folder. Anything that does not parse leaves the data folder untouched and exits 1.
"""
import json
import re
import sys
from pathlib import Path

SCRIPT = re.compile(r"<script[^>]*>(.*?)</script>", re.S | re.I)


def page_scripts(text):
    return [m.group(1) for m in SCRIPT.finditer(text)]


def grab(text, name):
    """The JSON value of window.<name>= in the text, or None."""
    marker = "window." + name + "="
    at = text.find(marker)
    if at < 0:
        return None
    value, _ = json.JSONDecoder().raw_decode(text, at + len(marker))
    return value


def engine_of(text):
    start = text.find("//ENGINE-START")
    stop = text.find("//ENGINE-END")
    if start < 0 or stop < 0 or stop < start:
        return None
    return text[start:stop + len("//ENGINE-END")] + "\n"


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    page_path, folder = Path(argv[1]), Path(argv[2])
    try:
        page = page_path.read_text(encoding="utf-8", errors="replace")
        scripts = page_scripts(page)
        # the data script is the page's own file next to it, the names and interface texts are inline
        data_files = sorted(page_path.parent.glob("drop-finder_files/*.js")) or sorted(page_path.parent.glob("*.js"))
        data_text = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in data_files)
        inline = "\n".join(scripts)
        values = {}
        for name, source in (("DF_DATA", data_text), ("DF_NAMES", inline + data_text), ("DF_UI", inline + data_text)):
            values[name] = grab(source, name)
        lang = re.search(r'window\.DF_LANG="([^"]*)"', inline + data_text)
        engine = engine_of("\n".join(scripts))
    except (OSError, ValueError) as error:
        print("Drop Finder: %s" % error)
        return 1
    if values["DF_DATA"] is None or values["DF_NAMES"] is None or values["DF_UI"] is None or engine is None or not lang:
        print("Drop Finder: the page does not have the expected data, keeping the current files")
        return 1
    folder.mkdir(parents=True, exist_ok=True)
    outputs = {
        "df-data.json": values["DF_DATA"],
        "df-names.json": values["DF_NAMES"],
        "df-ui.json": values["DF_UI"],
    }
    for name, value in outputs.items():
        (folder / name).write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    (folder / "df-engine.js").write_text(engine, encoding="utf-8", newline="\n")
    (folder / "df-lang.txt").write_text(lang.group(1) + "\n", encoding="utf-8")
    print("Drop Finder: %d items, %d uniques, %d sets, %d treasure classes, %d monsters, %d zones"
          % (len(values["DF_DATA"]["items"]), len(values["DF_DATA"]["uniques"]), len(values["DF_DATA"]["sets"]),
             len(values["DF_DATA"]["tcs"]), len(values["DF_DATA"]["mons"]), len(values["DF_DATA"]["zones"])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
