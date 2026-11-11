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
    skill-icons.txt
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
skills that items grant to any class. It is laid out like the Skills page of the ESR runeword browser. On the left are the
class's three skill trees as tabs and the chosen tree as a grid: three columns, each skill a tile with its icon, its name
and the level it needs (**Lvl N**), with lines from each skill to the skills that need it. Click a tile to see that skill
on the right (the tree's first skill is shown at first). Item Skills has a list of the skills instead of a tree.

The right side shows the skill's icon and name, its class and tree, **Req** (the level it needs), **Max** (its highest
level) and **Mana** (and how much more per level), Passive or Aura when it is one, the description, the skills it
requires, whether it triggers "on striking" and "on casting" effects and why, the bonus it gives with a partner skill at
20 levels each, and **Hard points only** when +skills from items do not count for it. Below that are three tabs:

- **Values**: the **Skill level** (1-110; click - or +, turn the mouse wheel over the value, or pick 1, 20, 30, 50 or 110)
  and the **Character level** (some skills scale with it). **Receives bonuses from** lists the skill's synergies, each with
  a box for the points in that skill (**Max all** fills them, **Reset** empties them); the numbers below include them.
  **At level N** has the lines that do not change with the level, then a table with each value at the chosen level (gold)
  and at levels 1, 20, 30, 50 and 110. **All levels** adds a table of every level from 1 to 110. Levels marked * are beyond
  what a character can reach; their numbers are still worked out.
- **Items**: every item and recipe that grants the skill, grouped by kind (uniques, set items, runewords, affixes and so on)
  with what each gives.
- **About**: the notes on how the values are worked out.

The right side scrolls on its own (mouse wheel over it); in a narrow window it goes under the tree and the whole pane
scrolls. The chosen tree and skill of each class, the levels and the synergy points stay while the game runs. With a
controller, go right from the class list into the tree: the D-pad moves between the tiles, LB / RB switch the tree, A
shows the highlighted skill, Right from the last column goes into the details (the level boxes, the quick levels, the
synergy boxes, the tabs), where Left / Right change the highlighted value (hold to go faster); B goes back to the tree.

Skills are found by the normal search and the page filter: by name, description, class, tree, Passive, Aura and the names
of the items that grant them. A search hit opens the skill in its tree (its tile is framed for a moment), and while you
type, the tiles that do not match are dimmed; Enter steps through the matching skills. The values assume no items and no
points in other skills, except the synergy points you set. The skill icons are `images\skill_*.png`, named in
`skill-icons.txt` in the plugin folder. The data is `skills-data.json` in the plugin folder
(`locales\<code>\skills-data.json` for the other languages); skill names, descriptions and values are in the game's
language, while some item names and notes are still English. Without the file the tab is left out.

## What is new in 1.7.0

- **Welcome > What's worth looting?** ends with a link to the ESR loot filter editor (edit an existing filter or build one from scratch); click it with the mouse to open it in your browser. The sentence is in all thirteen languages.
- **New Skills tab** (after Kill Ledger): every class's skill trees as on the ESR runeword browser's Skills page, with the tree's skills as tiles (icon, name, required level, prerequisite lines) and the chosen skill's details beside them: its facts, a Values tab that works the numbers out like the game for any skill level, character level and synergy points (with levels 1, 20, 30, 50 and 110 side by side, and every level on request), an Items tab with every item and recipe that grants it, and an About tab. The item skills have a list. Searchable like the other tabs, usable with the controller, in all thirteen languages. See **Skills** above. The skill data and the formulas come from [esr-docs](https://github.com/dotsyzygy/esr-docs) by Syzygy (MIT).
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
