# ESR Wiki Overlay 1.7.0

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
    web-word-art.txt
    web-base-art.txt
    ias-calculator.html
    ias-calculator-data.js
    skills-data.json
    images/*.png
    locales/<language>/ui-strings.txt, wiki-data.txt, cube-wiki-data.txt, skills-data.json   (one folder per language)
```

## Open it
- Press pause or escape and press the ESR Wiki button, or in the Horadric Cube there is a recipe button.
- Type `eswiki` in the in-game console, or
- Bind a key to "Open ESR Wiki" in the game's controls (no key is bound by default).

## Search

The search box finds text in the language of the wiki data (the empty box says which). Case and accents do not matter
(`cafe` finds Café). The list on the left keeps the entries that match, the cards in an entry are narrowed to the matching
ones, and the matching words are highlighted.

- `frost poison`: every word must be on the card, in any order and on any line. A word of one or two letters must start a word.
- `"cold damage"`: the words together, in this order, on one line.
- `-fire`, `-"fire damage"`: leaves out what has the word or phrase. A search of only `-words` gives no suggestions.

The perks of the Specializations tab are searched too, by name and by what their tooltip says, in the language of the
Specializations page that was loaded. They show in the suggestions with a Perk tag ("Specializations › Alchemist").
Picking one opens the Specializations tab on that specialization and frames the perk on the board for a moment, with
its tooltip. While you search, the Specializations tab counts as a match when a perk matches, and on the board the perks
that do not match are dimmed (the specializations that have matching perks get a gold frame).

Keys in the search box:

- **Enter**: with the suggestions open, opens the picked row, or the top row when none is picked. With the list closed,
  Enter goes to the next match (Shift+Enter the previous one).
- **Up / Down**, **Tab / Shift+Tab**: pick a row (Tab wraps round). Moving the mouse over a row picks it too; Enter opens
  whichever you picked last.
- **Shift+Enter** (the previous match), **F3** / **Shift+F3** at any time: the next / previous match on the page, as the
  `<` `>` buttons do.
- **Escape**: closes the list (a second Escape closes the wiki). **Down** or a click on the search box brings the list back.

Controller in the search (Xbox names, PlayStation names in brackets):

- **D-pad down** from the on-screen keyboard's bottom row moves into the suggestions (the top row is picked); with no
  suggestion to open it goes round to the top row of keys as before. **View (Share / Create)** moves into the
  suggestions at any time, also with the keyboard put away, as long as something is typed (it brings the list back if
  it was closed); pressed again it goes back.
- In the suggestions: **D-pad up / down** pick a row (the "Did you mean" heading is skipped); up from the first row goes
  back to the keyboard (or to the page). **LB / RB (L1 / R1)** go a page of rows up / down. **A (Cross)** opens the row
  and puts the keyboard away. **B (Circle)** closes the list and goes back to the keyboard or the page. **Y (Triangle)**
  shows or hides the keyboard.
- Everywhere else LB / RB (L1 / R1) still open the previous / next tab, also while the keyboard is open.
- With the keyboard open the list goes beside it when the window is wide enough, otherwise under it with at least
  four rows (the keys get a little shorter when that is needed). It never covers the keyboard or the search box and its
  buttons. The help window and a line under the list show the buttons for where the focus is (page, keyboard or list).

## Campaign map

Welcome > Campaign Progression shows a map of the campaign under its text: one tab per act (Act I to Act V), plus
Special areas and Ubers & portals as cards. Each area is a dot coloured by terrain (a town is a square), with a blue ring
for a waypoint, its name, its Hell monster level and "WP". A red triangle marks a boss, an amber diamond a quest item and an
orange "!" something to watch out for (no teleport, no town portal, a caveat). Lines show how areas connect: walk across
(thick, faint), entrance (thin), portal (dashed purple), cube portal (dash-dot amber), next act (bold); an arrowhead marks a
one-way link. A dashed dot is an area drawn on another tab: clicking it opens that tab.

Hover an area for its tooltip (official name, terrain, waypoint, monster levels in Normal / Nightmare / Hell, quest items,
bosses, the first caveats). Click it to see its details under the map: quest items, heads up, boss, notes (spoilers stay
hidden until clicked), getting there with a button for each connected area, and the monster levels. Escape (or the X)
closes the details. Area names follow the game language; the notes are in English. The areas are searchable by name (in the
game language and in English), boss, quest and quest item; picking one opens the map on its tab, selects it and rings it
for a moment. With a controller, D-pad right from the list moves onto the map: the D-pad goes to the nearest area in that
direction, LB / RB (L1 / R1) change the map's tab, A (Cross) shows the details, B (Circle) closes them (pressed again it goes
back to the list). The map's data is `campaign-map.txt` in the plugin folder.

The Special areas cards and the teleport restrictions come from the website's Maps page, in the game language: a card that
the page lists shows the page's own size and location lines, an area on the page's Teleport Restrictions list shows the
page's wording in its heads up, and the page's closing caution is shown under the Special areas tab (with any row of the page
that matches no area on the map, so nothing it says is left out). The text is shown exactly as the website writes it. The
plugin ships a copy of the page (`maps.htm`, and `locales\<code>\maps.htm` for the other languages) and refreshes it like the
other website pages, at game start and every six hours, from the page in the game's language (`live-updates-disabled.txt`
turns every website check off). The rest of the map's notes (quest steps, caveats, how to get in) are in English for now.

## Skills

The Skills tab (after Kill Ledger) lists every skill: one entry per class (Amazon to Warlock) and one for Item Skills, the
skills that items grant to any class. The cards are grouped by skill tree. Each card shows the skill's name, Passive or
Aura when it is one, its required level and the skills it requires, and the first values at the chosen skill level. Hover
a card (or highlight it with the controller) for everything: the description, whether it triggers "on striking" and "on
casting" effects and why, the general lines, the values at the chosen level, the synergies, and every item and recipe
that grants it (kind, name and what it gives).

The strip above the cards sets what the numbers are worked out for: **Skill level** (1-110), **Character level** (1-110;
some skills scale with it) and **Hard points** (Default, or 0-20: the points put into the skill itself, which some
skills use). Click - or +, or turn the mouse wheel over a value. With a controller, go up from the cards to the strip;
Left / Right change the highlighted value (hold to go faster). The values stay while the game runs. Sort shows the cards
by skill tree (the default) or A-Z.

Skills are found by the normal search and the page filter: by name, description, class, tree, Passive, Aura and the names
of the items that grant them. The values assume no items and no points in other skills; synergy bonuses are listed under
each skill and are not included in the numbers. The data is `skills-data.json` in the plugin folder
(`locales\<code>\skills-data.json` for the other languages); skill names, descriptions and values are in the game's
language, while some item names and notes are still English. Without the file the tab is left out.

## What is new in 1.7.0

- **New Skills tab** (after Kill Ledger): every class's skills and the item skills as cards with tooltips, grouped by skill tree, with a picker for skill level, character level and hard points that works the values out like the game. Searchable like the other tabs, usable with the controller, in all thirteen languages. See **Skills** above. The skill data and the formulas come from [esr-docs](https://github.com/dotsyzygy/esr-docs) by Syzygy (MIT).
- **New Specializations tab** (between Endgame Maps and Weapon Mastery): the website's Specializations page as a working perk board. Click a tab (Alchemist, Blacksmith, Jeweler, Archaeologist) to switch specialization; hover a perk (or highlight it with the controller) to see its tooltip, with its effect and respec cost. The explanation above the board, the tab names, the perks, their order and their wording all come from the website page, so changes to it arrive without a new plugin version. It refreshes like the other website pages (at game start and every six hours, in the game's language from the localized page; the last copy is kept in the plugin folder) (`live-updates-disabled.txt` turns every website check off). The board art (perk icons, background, glow) comes from the game's own files and is shipped in the plugin's `images` folder as `spec_*.png`. The tab is named in all thirteen languages with the website's own word for specializations.
- **Campaign map** on Welcome > Campaign Progression: the act maps, the special areas and the uber portals, with a tooltip and a details panel for every area, searchable (an Area tag in the suggestions) and usable with the controller. See **Campaign map** above. The Special areas cards and the teleport restrictions follow the website's Maps page live, in the game language.
- **Search suggestions.** Typing in the search box opens a dropdown of the best matches: tabs, entries, cards and the Specializations perks, each with a small Tab / Entry / Card / Perk tag, where it is (tab and entry, the entry name is kept when the space is short), the matching letters in gold and, for a match inside a card's text (a stat, a rune, a note), the line that matched in bold gold. The list is looked up a moment after you stop typing, says how many results it has, and when nothing matches it says why ("Nothing has: frost", "No card has all of: frost, poison"). Hits found only by a typo or by letters in order come last, under "Did you mean". Enter opens the top row (it is shaded) or the row you picked; see **Search** above for the keys. Picking a suggestion keeps the text you typed, so the list on the left and the dropdown always agree; only a typo hit puts the card's name in the box. Picking a tab keeps the search when that tab has matching entries, and clears it (with a note) when it has none. If a filter button (sockets, tier, weapon type) would hide the card you picked, the filters are reset and "Filters cleared" shows under the search box for a moment. With a controller the list has its own focus; see **Controller in the search** above. The page filter follows the same rules as the suggestions, and the search highlight inside cards marks every word of the search (a "phrase" as one run, never an excluded word).
- **Fixed: tooltip text overlapping the search highlight.** When a search term was highlighted inside a tooltip, the highlighted words were measured differently from how they were drawn, so the tooltip's last line could be cut off or overlap the next one. Highlighted text is now measured the same way it is drawn, in tooltips and everywhere else.
- **Controller:** the left stick is smoother and the stick cursor hides when you go back to the D-pad; the filter and sort buttons and the IAS calculator's fields can be reached with the D-pad; the D-pad starts from the item under the cursor; cards scroll into view as you move between them; the help window fits the wiki window, stays put, and does not flicker.
- **Controller in the search suggestions:** D-pad down from the keyboard's bottom row, or View (Share / Create), moves into the suggestions; the D-pad picks a row, L1 / R1 (LB / RB) page, A (Cross) opens it, B (Circle) closes the list. The list sits beside the keyboard when there is room (under it, with at least four rows, when not), and the controller help changes with the focus. See **Controller in the search** above.
- **On-screen keyboard:** Square (X) deletes a character, Circle (B) clears the search (or closes the keyboard when the search is already empty), Triangle (Y) closes the keyboard, each language has its own letter layout, the highlight wraps around the edges and no longer lights the key you typed.
- The wiki ignores the controller while it is open, so the game no longer reacts to the buttons you press in it.
- The mouse wheel scrolls 50% faster, and the window is smaller at 1080p so it no longer fills the screen. Precision touchpads scroll too: their small steps add up instead of being dropped.
- **Item Bases, Automod tooltip:** an Automod value with several tiers shows one tier per line, indented under the label; when every tier ends with the same line (the shared Tinkering Points line), that line is shown once at the end instead of after every tier.

## What is new in 1.6.0

- **Game controller support.** The wiki can be used with a PlayStation controller (DualShock 4, DualSense) or an Xbox controller; it notices the controller by itself and the on-screen help appears to the right of the wiki, matching the controller in use. Using the mouse or keyboard hides it again. Controls (PlayStation names in brackets):
  - D-pad: highlight a tab (it only opens when you press A (Cross)). Pressing down with no tab below continues into the categories list on the left; up from the first category goes back to the tabs. In the list, up and down pick an entry and left and right turn the list page.
  - LB / RB (L1 / R1): open the previous / next tab at once. R3: switch between the wiki and the Horadric Cube Recipes window. Start (Options): close the wiki.
  - Right stick: scroll the entry. RT / LT (R2 / L2): scroll down / up much faster.
  - Y (Triangle): the on-screen keyboard appears under the search box and the search takes focus. A types the highlighted key, X (Square) clears the search, B (Circle) or Y or the Done key puts the keyboard away without closing the wiki. (In 1.7.0 the keyboard's Square and Circle changed, see above; with the keyboard closed, X (Square) still clears the search.) The keyboard has a second page for accented letters.
  - Left stick: moves a cursor over the wiki (for the item tooltips and filter buttons); while it was the last thing moved, A (Cross) clicks at the cursor. Clicking the search box with it opens the keyboard.
  - The help picture is `images\controller_playstation.png` or `images\controller_xbox.png` in the plugin folder; without those files a list of the buttons is drawn instead. A controller used through Steam Input or DS4Windows looks like an Xbox controller to Windows; a file `controller.txt` in the plugin folder containing `playstation` or `xbox` forces which help is shown.
- The mouse cursor drawn over the wiki now follows the pointer every millisecond instead of about 64 times a second, so it keeps up with the game's own cursor.
- The pause-menu ESR Wiki button now opens the panel you used last, like the wiki key and the `eswiki` command do. The first time it is the main wiki; after that, if you last used the Horadric Cube Recipes window, that is the one that opens. The page you were reading in each window is kept while the game runs.

## What is new in 1.5.1

- Charm runewords that also allow weapons or armor (Brandish, Machine, Hunt, Hades, Wisdom, Mist and others, 15 of the 33 on the Charms tab) now show their stats. Before, those cards showed only the name, runes and required level, because a charm card was filled only when all three website columns agreed and the columns a runeword does not use are empty. Charms now compare the filled columns; if those disagree nothing is shown, and charms still show no rune bonuses. The fix applies to English and to all twelve other languages, both in the shipped data and when the pages refresh from the website.

## What is new in 1.5.0

- **Thirteen languages.** The wiki and the Horadric Cube Recipes window now follow the game's language: English, German (deDE), Spanish (esES), Spanish Latin America (esMX), French (frFR), Italian (itIT), Portuguese (ptBR), Polish (plPL), Russian (ruRU), Korean (koKR), Japanese (jaJP), Simplified Chinese (zhCN) and Traditional Chinese (zhTW). The text comes from the matching language of the Eastern Sun Resurrected website, the same pages and counts as English, and refreshes the same way (at game start and every six hours; the last copies are kept in the plugin folder). The `locales` folder holds a shipped copy of every language for when the website cannot be reached.
- Only the labels the website does not have (buttons, tab titles, the Welcome page, the IAS calculator wording, table labels) are written by hand. **These hand-written texts are first drafts and have not been reviewed by native speakers**; corrections are welcome.
- **Not yet checked in the game:** Simplified Chinese, Traditional Chinese and Japanese. Selecting them crashed D2RLoader itself during our testing (the crash is inside D2RLoader.exe, not the overlay), so their layout, fonts and text could not be reviewed. Treat these three as unverified. The other nine non-English languages were checked in the game.
- A few long runeword effect lines that have no counterpart on the website stay in English, and some Spanish, Portuguese, Polish and Italian tab titles may be wider than their tabs.
- If the wiki shows English in a non-English game, check that `esr-wiki-overlay/locales/<language>/` exists in the plugin folder and that the log (`logs/esr-wiki-overlay.log`) does not say `UI strings loaded: 0`.

## What is new in 1.4.0

- The Runes page is now built from the Eastern Sun Resurrected website (the gems page) and refreshes the same way as the Uniques page: when the game starts and every six hours while it runs, with the last copy kept in the plugin folder. A file named `runes-live-disabled.txt` in the plugin folder turns the check off. Each rune shows the website's own item-kind labels and values; the "How to Acquire Elemental Runes" note stays at the end, and its recipe table follows the Cube Recipes page, so it updates too (the explanatory text above the table is fixed).
- The Runewords and Gemwords pages are now built from the website's runewords and gemwords pages, and refresh the same way (at game start and every six hours; the last copies are kept in the plugin folder). Files named `runewords-live-disabled.txt` and `gemwords-live-disabled.txt` turn the checks off. A runeword's required level is the highest level of the runes it uses, read from the gems page. Gemword cards are one per website row. `web-word-art.txt` gives the cards their pictures.
- The Item Bases page is now built from the website's armors and weapons pages and refreshes the same way (at game start and every six hours; the last copies are kept in the plugin folder). A file named `bases-live-disabled.txt` turns the check off. `web-base-art.txt` gives the items their pictures, tiers and weapon families.
- New IAS Calculator tab (second row): the website's Frame Table as a working calculator. Pick class, form, skill and weapons, type your attack speed and cast rate, and see the frames, attacks per second and the breakpoint tables. Its data and constants refresh from the website like the other pages (a file named `ias-live-disabled.txt` turns the check off).
- The Ascendancy tab now follows the website's Ascendancies page (introduction, challenges, gem recipes, tier stats and footnotes) and refreshes the same way as the other website pages. A file named `ascendancy-live-disabled.txt` turns the check off.
- The Set Items tab now follows the website's Sets page (every piece's stats and bonuses, each set's partial and full bonus) and refreshes the same way as the other website pages. Pictures stay as before. A file named `sets-live-disabled.txt` turns the check off.
- The Kill Ledger tab now follows the website's Kill Ledger page (overview, kill bonuses and boss bonuses) and refreshes the same way as the other website pages. A file named `kill-live-disabled.txt` turns the check off.
- The Weapon Mastery tab now follows the website's Weapon Mastery page (overview, general recipes and every specialization variant) and refreshes the same way as the other website pages. A file named `mastery-live-disabled.txt` turns the check off.
- The Vessel of Souls tab now follows the website's Vessel of Souls page and refreshes the same way as the other website pages. A file named `vessel-live-disabled.txt` turns the check off.
- The Mercenary/O-Skill tab now follows the website's Skill Information page and refreshes the same way as the other website pages. A file named `skills-live-disabled.txt` turns the check off.
- The Special Craft tab (corruptions, anointments, sanctifications, standard of heroes) now follows the website's Special Crafting Outcomes page and refreshes the same way as the other website pages. A file named `special-live-disabled.txt` turns the check off.
- The Endgame Maps tab now follows the website's End Game Maps page (overview, every map boss's abilities and rewards) and refreshes the same way as the other website pages. Boss pictures stay as before. A file named `endgame-live-disabled.txt` turns the check off.
- New Item Affixes tab: every prefix, suffix and quality affix from the website's prefixes, suffixes and quality pages, in three categories (Armor, Weapons, Other), each with an Affix filter (Prefix, Suffix, Superior) and a Type filter for the item type, like Uniques and Item Bases, with the website's levels, rarity, group and stats. It refreshes the same way as the other website pages (at game start and every six hours; the last copies are kept in the plugin folder). A file named `affixes-live-disabled.txt` turns the check off.
- The D-Stones and Forging tabs are gone from the main wiki; both are still on the Cube Recipes window.
- Cube recipe tables centre their text vertically, so a recipe with more inputs than results (or the reverse) no longer leaves the shorter side at the top of its row.

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
- The campaign map data (`campaign-map.txt`: area links, bosses, quest steps, caveats and the act layouts) is ported from the campaign map of [d2r-esr-runeword-browser](https://github.com/istvan-panczel/d2r-esr-runeword-browser) by István Pánczél (MIT license; see `THIRD_PARTY_NOTICES.md`).
- The Skills tab's data (`skills-data.json`) is made by the skills generator of [esr-docs](https://github.com/dotsyzygy/esr-docs) by Syzygy (dotsyzygy), and its values are worked out by a port of that project's skill calculator (MIT license; see `THIRD_PARTY_NOTICES.md`). Thanks to esr-docs for the reference Skills page this tab follows.
