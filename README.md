# ESR Wiki Overlay 1.2.0

A read-only in-game wiki overlay for Eastern Sun Resurrected (ESR) on Diablo II
Resurrected, loaded by D2RLoader. It has two windows: the ESR Wiki and Horadric Cube Recipes.

Author: Sully

## Install

Copy these files into your mod's D2RLoader plugins folder:

```
Eastern_Sun_Resurrected/d2rloader/plugins/
  d2rl-esr-wiki-overlay.dll
  esr-wiki-overlay/
    wiki-data.txt
    cube-wiki-data.txt
    images/*.png
```

## Open it
- Press pause or escape and press the ESR Wiki button, or in the Horadric Cube there is a recipe button.
- Type `eswiki` in the in-game console, or
- Bind a key to "Open ESR Wiki" in the game's controls (no key is bound by default).

## What is new in 1.2.0

- Runeword, unique and set description text now comes from the mod's own files.
- The Runewords page lists every runeword in the mod; gemwords are on the Gemwords page only.
- Rune tooltips show the rune's stat lines instead of the "Socketed in" line.
- Unique cards that can be gambled show "Gamble Item:" with the base item and item level.
- The overlay uses the game's own mouse cursor (a drawn copy of the gauntlet is the fallback).
- The Hireling set items are rebuilt from the mod's setitems.txt: each piece shows its own base item and required level. Each set has its own header (laid out like the Warlock sets) in Normal, Nightmare, Hell order, with a difficulty filter.
- Gemword cards and set item names were corrected against the mod files.

## Notes

- The overlay only reads the data files above. It does not change the game.
- Colors and stat lines follow the game's text color codes.
