# ESR Wiki Overlay 1.1.0 (source)

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
  plugin.cpp          D2RLoader entry points, the eswiki command and bindable action
tests/
  wiki_model_test.cpp portable model tests
  test_extract.py      extractor tests (unittest)
tools/
  extract_wiki.py      builds data/wiki-data.txt from the mod's layout JSON and Excel tables
  build_cube.py        builds data/cube-data.txt from the cube recipe source
  build_cube_wiki.py   builds data/cube-wiki-data.txt from cube-data.txt
  sprites_to_png.py    converts D2R .sprite files to PNG art
  excel_items.py, wiki_layout.py, _probe_util.py   helpers used by the tools
data/
  wiki-data.txt, cube-wiki-data.txt, cube-data.txt   generated data files
```

## Inputs the tools need

The generators read files from the Eastern Sun Resurrected mod and the base game, which
are not included here. Point the tools at your own copies:

- the mod's layout JSON files (`*hd.json`) and string table
- the mod's Excel tables (uniqueitems, setitems, runes, itemtypes)
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

## Known limits

- One gem icon (`items/misc/jewel/jewel`) is missing from the mod and base game; the
  Gemwords card that uses it shows the Smiiiff Stone art instead.
- Colors are approximations of the D2 palette. Stat lines use the first color code on each line.
