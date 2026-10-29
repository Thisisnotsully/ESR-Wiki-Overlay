# ESR Wiki Overlay 1.3.0 (source)

Source for the ESR Wiki overlay plugin for Diablo II Resurrected (Eastern Sun Resurrected),
loaded by D2RLoader. It contains the C++ overlay, the Python tools that generate its data,
and the generated data files.

Author: Sully

AI disclosure: see [AI_DISCLOSURE.md](AI_DISCLOSURE.md).

Credits: [CREDITS.md](CREDITS.md). Third-party licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Layout

```
CMakeLists.txt        build (Visual Studio 2022, x64)
deploy.cmake          copies the built files into a mod's d2rloader folder
pages.json            page list and tab order; add categories here
src/
  wiki_model.*        portable parser, search, paging and color codes (no Win32)
  overlay.*           Win32 window and GDI+ art rendering
  cursor_art.hpp      generated cursor image used by overlay.cpp
  plugin.cpp          D2RLoader entry points, the eswiki command and bindable action
tests/
  wiki_model_test.cpp portable model tests
  cube_live_test.cpp  checks the C++ cube page reader against tests/cube_live/ (a saved copy of the page and the
                      output the Python tools give for it)
  test_extract.py      extractor tests (unittest)
tools/
  extract_wiki.py      builds data/wiki-data.txt from the mod's layout JSON and Excel tables
  build_cube.py        builds data/cube-data.txt from the cube recipes web page (saved HTML, or the live page)
  build_cube_wiki.py   builds data/cube-wiki-data.txt from cube-data.txt
  web_uniques.py       builds the Uniques page from the website pages in data/website-uniques/ (the C++ twin is src/web_uniques_live.cpp)
  runeword_gen.py, runeword_desc.py   stat and description lines for runeword and gemword cards (from runes.txt, gems.txt)
  make_cursor.py       builds src/cursor_art.hpp (the fallback gauntlet cursor, from ohand; the overlay uses the game's own cursor when it can)
  make_runeword_art.py one-time export of the runeword card art to data/runeword-art.json
  sprites_to_png.py    converts D2R .sprite files to PNG art
  excel_items.py, wiki_layout.py, _probe_util.py   helpers used by the tools
data/
  wiki-data.txt, cube-wiki-data.txt, cube-data.txt   generated data files
  runeword-art.json    card art, titles and order for the Runewords page (the art varies within each item
                       category so the cards do not all look alike); the extractor reads it
```

## Inputs the tools need

The generators read files from the Eastern Sun Resurrected mod and the base game, which
are not included here. Point the tools at your own copies:

- the mod's layout JSON files (`*hd.json`) and string table
- the mod's Excel tables (uniqueitems, setitems, runes, gems, gamble, armor, weapons, misc, itemtypes, cubemain)
- the mod's `hd/items/uniques.json` for unique artwork
- the mod's and the base game's `hd/global/ui` sprite folders for art

Example:

```powershell
python tools/extract_wiki.py --excel "<mod>\data\global\excel" `
  --sprite-root "<mod>\Eastern_Sun_Resurrected.mpq\data\hd\global\ui" `
  --base-ui-root "<base game>\data\data\hd\global\ui" `
  --uniques-json "<mod>\Eastern_Sun_Resurrected.mpq\data\hd\items\uniques.json"
python tools/sprites_to_png.py --source "<sprite folder>"
```

Run the generators from the `tools` folder. The image output folder (`data/images`) is not
part of this source package; `sprites_to_png.py` creates it.

## Build (Windows, Visual Studio 2022 C++ tools)

The build needs the D2RLoader Plugin SDK headers. Point CMake at them with
`-DESR_PLUGIN_SDK_INCLUDE=<path>`.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DESR_PLUGIN_SDK_INCLUDE=<path>
cmake --build build --config Release
```

Then copy `d2rl-esr-wiki-overlay.dll`, `wiki-data.txt`, `cube-wiki-data.txt` and `images/`
into your mod's `d2rloader/plugins` folder (see the standalone package for the layout).

## Tests

```powershell
python -m unittest discover -s tests -p "test_*.py"
```

`test_extract.py` needs the generator inputs above, so run it where they are available.

## Adding a category

1. Add the layout JSON to the inputs.
2. Add an entry to `pages.json`. `"mode": "guide"` makes each heading an entry;
   `"mode": "cards"` makes each matching node a card.
3. Run the generators, then rebuild. No C++ change is needed for a new page.

## Where card data comes from

Runeword, unique and set cards take their orange description lines and stat lines from the mod's
own Excel tables and string files, not from the website. The Runewords page is built from
`runes.txt`; gemwords are kept on the Gemwords page only. Unique cards show a "Gamble Item" line
(the Normal base item and the unique's item level) when `gamble.txt` lists its base; Coupon,
Mythical and non-spawnable uniques never get it.

The Hireling category of the Set Items page is built from setitems.txt and sets.txt (the rows whose set
ends in -N, -NM or -H), laid out like the Warlock sets: a header for each set and difficulty, its pieces,
then its set bonus card, in Normal, Nightmare, Hell order. It can be filtered by difficulty.

## Cube recipes from the website
`src/cube_live.cpp` reads the cube recipes page (HTML) and writes the same data as `tools/build_cube.py` plus
`tools/build_cube_wiki.py`; `src/cube_updater.cpp` downloads it (WinHTTP, conditional request, cached next to the data,
rechecked every six hours) and the plugin swaps the result into the Horadric Cube window. The grouping rules (which site
tabs make which page, class crafting left out of Legacy Craft) exist in both places; if the page changes, update both and
refresh `tests/cube_live/` (save the page there as cube-recipes-page.html and rerun the Python tools to rewrite
cube-wiki-data.expected.txt). `scripts/publish-overlay.ps1` rebuilds the shipped cube data from the live page
(skip with -SkipCubeRefresh). A file named `cube-live-disabled.txt` in the data folder turns the in-game check off.

## Uniques from the website
The Uniques page is built from unique_others/weapons/armors/mythicals.htm. `tools/web_uniques.py` does it at publish time
(`scripts/publish-overlay.ps1` downloads the pages into data/website-uniques/; skip with -SkipWebsiteUniques), and
`src/web_uniques_live.cpp` with `src/web_uniques_updater.cpp` redo it in the game (rechecked every six hours, cached next to
the data). Art, tier and weapon family come from data/web-unique-art.txt. A file named `uniques-live-disabled.txt` in the
data folder turns the in-game check off. Both parsers must agree; `web_uniques_live_test` checks that.

## Known limits

- One gem icon (`items/misc/jewel/jewel`) is missing from the mod and base game; the
  Gemwords card that uses it shows the Smiiiff Stone art instead.
- Colors are approximations of the D2 palette. Stat lines use the first color code on each line.
