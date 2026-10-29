# ESR Wiki Overlay 1.3.0

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
    web-unique-art.txt
    images/*.png
```

## Open it
- Press pause or escape and press the ESR Wiki button, or in the Horadric Cube there is a recipe button.
- Type `eswiki` in the in-game console, or
- Bind a key to "Open ESR Wiki" in the game's controls (no key is bound by default).

## What is new in 1.3.0

- The Uniques page is now built from the Eastern Sun Resurrected website (unique weapons, armors, others and mythicals), and refreshes like the cube recipes: when the game starts and every six hours while it runs, with the last copy kept in the plugin folder. A file named `uniques-live-disabled.txt` in the plugin folder turns the check off.
- Unique cards can be sorted by Normal, Exceptional and Elite bases, and filtered by the website's weapon and armor categories.
- Mythical cards show the full colours of their property lines.
- Charms no longer show gamble or coupon tags.
- The Cube Recipes and close buttons sit lower in the window frame.
- New `web-unique-art.txt` in the plugin folder gives the Uniques page its images, tiers and weapon families.

## What is new in 1.2.0

- Runeword, unique and set description text now comes from the mod's own files.
- The Runewords page lists every runeword in the mod; gemwords are on the Gemwords page only.
- Rune tooltips show the rune's stat lines instead of the "Socketed in" line.
- Unique cards that can be gambled show "Gamble Item:" with the base item and item level.
- The overlay uses the game's own mouse cursor (a drawn copy of the gauntlet is the fallback).
- The Hireling set items are rebuilt from the mod's setitems.txt: each piece shows its own base item and required level. Each set has its own header (laid out like the Warlock sets) in Normal, Nightmare, Hell order, with a difficulty filter.
- Gemword cards and set item names were corrected against the mod files.

## Cube recipes follow the website
The Horadric Cube Recipes window reads the recipes from the Cube Recipes page on easternsunresurrected.com when the
game starts, and again every six hours while it runs. The page is only downloaded when it has changed, and the last
copy is kept in the plugin folder (`cube-recipes-page.html`), so the recipes still show when you are offline. If the
page cannot be reached or read, the recipes shipped with the plugin are shown. This is the only network use of the
plugin; it sends an ordinary web request (so the site sees your IP address and the plugin name) and nothing else. To turn it off, create an empty file named `cube-live-disabled.txt` in the
`esr-wiki-overlay` folder.

## Notes

- The overlay only reads the data files above. It does not change the game.
- Colors and stat lines follow the game's text color codes.
