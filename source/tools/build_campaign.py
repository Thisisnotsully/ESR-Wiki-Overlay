#!/usr/bin/env python3
"""Builds overlay/data/campaign-map.txt: the data of the Campaign Progression map (Welcome page).

Usage:
    python3 tools/build_campaign.py [--excel DIR] [--strings DIR] [--out FILE] [--check]

    --excel    folder with levels.txt                    (default ~/mnt/excel)
    --strings  folder with levels.json, item-names.json  (default ~/mnt/Eastern_Sun_Resurrected/
               Eastern_Sun_Resurrected.mpq/data/local/lng/strings)
    --out      file to write                             (default ../data/campaign-map.txt next to this tool)
    --check    build and validate only, write nothing; exit 1 when the written file would change

What it reads
    levels.txt    ids, act, waypoint, monster level (MonLvlEx per difficulty, only for rows that have monsters),
                  terrain (LevelType + *IsInside), Teleport (0 off, 2 restricted), PreventTownPortal, *Map Boss,
                  and the Vis0-7 links. A Vis link counts only when both rows list each other (a "passage").
    levels.json   the area names in all 13 languages, keyed by levels.txt LevelName; item-names.json is the second
                  place to look (ESR keeps some new area names there). A name missing everywhere stays English only.
                  Names ending in " (Endgame Zone)" lose that suffix (in every language) and get the flag "zone".
    item-names.json  the quest item names, keyed by item code.
    The curated tables below: what the txt files cannot give (outdoor borders, portals, act travel, bosses, quest
    steps, caveats, the hand-placed act grids, notes). They are ported from the campaign map of
    d2r-esr-runeword-browser (MIT, Copyright (c) 2025 Istvan Panczel; see THIRD_PARTY_NOTICES.md), with the note
    wording rewritten. Every curated fact keeps the reference's source as one letter:
        O = the official ESR Maps page    T = read from the txt files    V = unchanged Diablo II behaviour
        W = confirmed in game (the reference's owner)
    A lowercase letter (o, t, v, w) means the reference marks the fact "to verify" (not confirmed in game yet).

Which areas
    levels.txt rows of the campaign: ids 1-138 and 214 (Naraku Level 2), plus 213 (the unique map, listed on the
    Ubers & portals tab). Left out: the null row, the endgame map rows (139-212) and the levels ESR removed but kept
    in levels.txt (30 old Jail Level 2, 36 old Catacombs Level 3: nothing links to them any more, and their LevelName
    is taken by the next row, so the clash disappears with them).

Output format (UTF-8, LF, one record per line, fields separated by TAB; '#' lines are comments)
    #ESRCAMPAIGN<TAB>1                                   header, the format version
    LANGS  enUS deDE ...                                  the language codes of NAME / ITEM records
    TAB    key kind act title                             kind: grid (Act I-V) or cards; act 0-4 or -1
    GRID   tab text                                       one grid row: tokens separated by spaces (see below)
    REGION tab x0 y0 x1 y1 label                          background box in cell units (col, row)
    ROUTE  tab a b mode                                   route hint for the edge a-b, computed from a; mode d|s
                                                          (d = starts with the 45 degree segment, s = straight)
    CARD   tab id                                         card order of a cards tab
    SNOTE  tab src flags text                             tab note; flags: - or letters w (warning) s (spoiler)
    AREA   id tab act terrain wp lvN lvNM lvH flags twin  wp 0/1; level 0 = no monsters; flags comma list or -:
                                                          town special zone notp teleoff telelimit walls
                                                          entryunknown; twin = id of the folded row it joins, or 0
    NAME   id lang text                                   area name (enUS always present)
    OFFICIAL id text                                      the official Maps page's name when it differs
    BOSS   id text
    QUEST  id src item quest how                          item = quest item code or -
    NOTE   id src flags text
    ENTRY  id src text                                    how to get in when it is not a plain walk
    EDGE   from to kind oneway src via                    kind border|passage|portal|cube|act; oneway 0/1
    ITEM   code lang text                                 quest item names

Grid tokens: '.' empty, '12' station, '21..25' or '9+13' dungeon stack (one dot per id, top to bottom),
'@40' ghost of an area drawn on another tab. Twin rows (the seven Tal Rasha's Tombs) need one id only.

The output is deterministic: same inputs, same bytes.
"""
import argparse
import csv
import json
import os
import re
import sys

LANGS = ["enUS", "deDE", "esES", "esMX", "frFR", "itIT", "jaJP", "koKR", "plPL", "ptBR", "ruRU", "zhCN", "zhTW"]
REMOVED_LEVELS = {30, 36}
EXTRA_IDS = {213, 214}
UBER_TAB = "portals"
HARROGATH = 109
# Vis links only one row lists, dropped without a warning (ESR copied these rows from others)
EXPECTED_ONE_WAY_VIS = {(133, 17)}

# levels.txt LevelType -> terrain (outside, inside)
TERRAIN = {}
for _t, _types in (("town", (1, 12, 20, 26, 29)), ("outdoor", (5, 6, 11, 16, 21, 27, 28, 30, 35)),
                   ("cave", (3, 18, 23, 33)), ("tomb", (4, 10, 17)), ("dungeon", (7, 8, 13, 14, 15, 24, 25)),
                   ("temple", (9, 32, 34)), ("special", (19,))):
    for _n in _types:
        TERRAIN[_n] = (_t, _t)
TERRAIN[2] = ("outdoor", "tower")
TERRAIN[22] = ("outdoor", "temple")
TERRAIN[31] = ("outdoor", "temple")

# Names neither string file has (English only)
NAME_FALLBACK = {137: "Colossal Summit"}
# Quest items the string files do not name by code
ITEM_FALLBACK = {"ass": "Book of Skill", "ice": "Malah's Potion"}

# ---- curated tables (ported from d2r-esr-runeword-browser src/features/campaign-map/data, MIT) -------------------

TABS = [  # key, kind, act, title
    ("act1", "grid", 0, "Act I"), ("act2", "grid", 1, "Act II"), ("act3", "grid", 2, "Act III"),
    ("act4", "grid", 3, "Act IV"), ("act5", "grid", 4, "Act V"),
    ("special", "cards", -1, "Special areas"), ("portals", "cards", -1, "Ubers & portals"),
]

GRIDS = {
    "act1": dict(grid="""
      .     18       .     19    .       .     .
      39    .        17    .     38      .     .
      1     2        3     4     5       6     11+15
      .     8        9+13  .     10+14   .     20
      .     .        .     .     .       .     21..25
      .     .        .     .     .       .     .
      32    29+31    28    27    26      7     12+16
      33    34+35+37 .     .     .       .     .
      .     .        @40   .     .       .     .
    """, regions=[("Monastery", -0.45, 5.5, 4.45, 7.85)],
        routes={(4, 10): "d", (4, 38): "d", (6, 20): "d", (37, 40): "d"}),
    "act2": dict(grid="""
      .     52..54   74    46    66      73    @75
      50    .        55+59 .     .       .     .
      .     48+49    .     .     .       .     .
      40    41       42    43    44      45    .
      .     .        56+60 62    65      58    .
      .     .        .     .     .       .     .
      .     .        .     51    47      57    61
      .     64       63    .     .       .     .
    """, regions=[],
        routes={(50, 52): "d", (40, 48): "d", (41, 55): "d", (40, 51): "d", (51, 63): "d"}),
    "act3": dict(grid="""
      75    76       77    78    79      90    .
      .     85       88    .     80      86+94 .
      .     .        98    92    .       95    .
      .     .        93    .     81      96    .
      .     .        .     97    .       89+91 .
      .     .        .     99    82      87+84 .
      .     .        .     .     83      .     .
      .     .        .     @103  100..102 .    .
    """, regions=[("Kurast", 2.55, 0.45, 5.6, 7.85)],
        routes={(76, 88): "d", (88, 78): "s", (92, 80): "d", (92, 81): "d", (92, 93): "d", (80, 90): "d",
                (80, 95): "d", (81, 89): "d", (81, 97): "d"}),
    "act4": dict(grid="""
      103   104      105   106   107     108   @109
    """, regions=[], routes={}),
    "act5": dict(grid="""
      137   .        120   128   129     131   132
      127   117      118   .     .       .     .
      .     .        .     .     .       115   119
      .     121      .     .     .       214   116
      .     .        .     .     .       130   138
      109   110      111   112   114     122   .
      .     .        125   126   .       113   .
      .     .        .     .     .       123+124 .
    """, regions=[("Worldstone Keep", 2.55, -0.4, 6.45, 0.55), ("Naraku depths", 4.55, 1.55, 6.45, 7.85)],
        routes={(118, 115): "s", (109, 121): "s"}),
}

# (from, to, kind, oneWay, via, source[, "missing"]) ; "missing" = a passage the txt files do not list
LINKS = [
    (1, 2, "border", 0, "", "W"), (2, 3, "border", 0, "", "W"), (3, 4, "border", 0, "", "W"),
    (3, 17, "border", 0, "", "W"), (5, 6, "border", 0, "", "W"), (6, 7, "border", 0, "", "W"),
    (7, 26, "border", 0, "", "W"), (4, 38, "portal", 0, "Cairn Stones", "W"),
    (1, 39, "cube", 0, "Cow Portal", "T"), (37, 40, "act", 1, "Warriv, after Andariel", "W"),
    (40, 41, "border", 0, "", "W"), (41, 42, "border", 0, "", "W"), (42, 43, "border", 0, "", "W"),
    (43, 44, "border", 0, "", "W"), (44, 45, "border", 0, "", "W"),
    (54, 74, "portal", 0, "Arcane portal", "W"), (74, 46, "portal", 0, "Summoner's portal", "W"),
    (66, 73, "portal", 1, "Horadric Orifice (true tomb)", "W"), (73, 75, "act", 1, "Meshif, after Duriel", "W"),
    (75, 76, "border", 0, "", "W"), (76, 77, "border", 0, "", "W"), (77, 78, "border", 0, "", "W"),
    (78, 79, "border", 0, "", "W"), (79, 80, "border", 0, "", "W"), (80, 81, "border", 0, "", "O"),
    (81, 82, "border", 0, "", "W"), (82, 83, "border", 0, "", "W"),
    (78, 92, "passage", 1, "one way", "o"),
    (102, 103, "act", 1, "Red portal, after Mephisto", "W"),
    (103, 104, "border", 0, "", "W"), (104, 105, "border", 0, "", "W"), (105, 106, "border", 0, "", "W"),
    (108, 109, "act", 1, "Tyrael's portal, after Diablo", "W"),
    (109, 110, "border", 0, "", "W"), (110, 111, "border", 0, "", "W"), (111, 112, "border", 0, "", "W"),
    (109, 121, "portal", 0, "Red portal, after Anya", "W"),
    (111, 125, "portal", 0, "Red portal", "W"), (112, 126, "portal", 0, "Red portal", "W"),
    (117, 127, "portal", 0, "Red portal", "W"),
    (214, 119, "passage", 0, "", "W", "missing"),
    (109, 133, "cube", 0, "Pandemonium Portal", "W"), (109, 134, "cube", 0, "Pandemonium Portal", "W"),
    (109, 135, "cube", 0, "Pandemonium Portal", "W"), (109, 136, "cube", 0, "Pandemonium Finale Portal", "W"),
]

# The official Maps page lists these five as "No Teleport across the walls"
OFFICIAL_WALLS = [25, 37, 60, 102, 108]
WALLS_NOTE = ("W", "", "Teleporting across the walls works here in game, although the official page says it does not")
PANDEMONIUM = "Cube the Keys of Terror, Hate and Destruction in Harrogath: one of three portals opens"

# id -> dict(official=, special=, tab=, bosses=[], quests=[(src, item, quest, how)], notes=[(src, flags, text)],
#            entry=(src, text), entryunknown=True)
AREAS = {
    4: dict(bosses=["Rakanishu"], quests=[("W", "bkd", "The Search for Cain", "Touch the five stones in the order the key shows")]),
    5: dict(bosses=["Treehead WoodFist"], quests=[("W", "bks", "The Search for Cain", "Take the scroll from the Tree of Inifuss")]),
    8: dict(bosses=["Corpsefire"], quests=[("W", "", "Den of Evil", "Clear every monster in the cave")]),
    12: dict(official="Blood Raven's Workshop", notes=[("O", "", "Special area, entered from Tamoe Highland")]),
    17: dict(bosses=["Blood Raven"], quests=[("W", "", "Sisters' Burial Grounds", "Kill Blood Raven")]),
    25: dict(official="Forgotten Tower Level 5", bosses=["The Countess"],
             quests=[("W", "", "The Forgotten Tower", "Kill the Countess")], notes=[WALLS_NOTE]),
    28: dict(bosses=["The Smith"], quests=[("W", "hdm", "Tools of the Trade", "The Smith guards the Malus")]),
    31: dict(notes=[("O", "", "This is the old Jail Level 3; ESR took out the old Level 2")]),
    37: dict(official="Catacomb Level 4", bosses=["Andariel"],
             quests=[("W", "", "Sisters to the Slaughter", "Kill Andariel")],
             notes=[("O", "", "This is the old Catacombs Level 4; ESR took out the old Level 3"), WALLS_NOTE]),
    38: dict(bosses=["Griswold"], quests=[("W", "", "The Search for Cain", "Free Deckard Cain from his cage")],
             entry=("W", "Red portal at the Cairn Stones in Stony Field")),
    39: dict(special=True, bosses=["The Cow King"],
             notes=[("O", "", "Special area: top experience and drops once the difficulty is beaten"),
                    ("T", "", "It has a waypoint in ESR")],
             entry=("T", "Cube a Club with a Tome of Town Portal in the Rogue Encampment")),
    40: dict(official="Lut Gohlein", notes=[("O", "", "The official page marks the south sewer entrance here")]),
    44: dict(official="Lost Farm (formely Lost City)",
             notes=[("O", "", "Special area, in place of the old Lost City"),
                    ("O", "", "The Viper Amulet can be had without coming here")]),
    49: dict(bosses=["Radament"], quests=[("W", "ass", "Radament's Lair", "Radament drops the book")]),
    51: dict(notes=[("T", "", "Joins Lut Gholein, the Maggot Tunnels, the Maggot Passage and Temple Passage Level 1")]),
    54: dict(quests=[("W", "", "Arcane Sanctuary", "The portal to the Arcane Sanctuary is here")]),
    55: dict(official="Dark Magician's Chamber", notes=[("O", "", "Special area, entered from the Rocky Waste")]),
    60: dict(official="Hall of the Dead Level 2", bosses=["Bloodwitch the Wild"], notes=[WALLS_NOTE]),
    61: dict(official="Unholy Altar", bosses=["Fangskin"],
             quests=[("O", "vip", "The Horadric Staff", "Take the top of the staff from the altar (the official page calls it the Viper Amulet)"),
                     ("W", "", "The Tainted Sun", "Break the altar and the sun comes back")],
             notes=[("T", "", "Reached from Temple Passage Level 2, not from Claw Viper Temple Level 1")]),
    64: dict(bosses=["Coldworm the Burrower"],
             quests=[("O", "msf", "The Horadric Staff", "The shaft is in the chest (the official page: Staff of Kings)")]),
    65: dict(notes=[("O", "", "Special area, off the old Lost City (now Lost Farm)")]),
    66: dict(quests=[("W", "hst", "The Seven Tombs", "Only one tomb is the true one: put the staff in its Horadric Orifice")]),
    73: dict(bosses=["Duriel"], quests=[("W", "", "The Seven Tombs", "Kill Duriel")],
             entry=("W", "Portal from the Horadric Orifice of the true tomb")),
    74: dict(bosses=["The Summoner"], quests=[("W", "", "The Summoner", "Kill the Summoner, then read Horazon's Journal")],
             entry=("W", "Portal in Palace Cellar Level 3")),
    77: dict(official="Marsh of Pain (formely Great Marsh)", notes=[("O", "", "Special area, in place of the old Great Marsh")]),
    78: dict(official="Flayer Jungle (Gidbinn Village)",
             quests=[("O", "g33", "Blade of the Old Religion", "The Gidbinn lies on the altar")],
             notes=[("o", "w", "The tunnel down to Sewers Level 1 (new in ESR) has no way back up to the jungle")]),
    79: dict(notes=[("O", "", "The official page skips it: its route runs from the Flayer Jungle through Sewers Level 1")]),
    83: dict(bosses=["High Council"],
             quests=[("W", "qf1", "Khalim's Will", "One of the High Council drops the flail"),
                     ("W", "qf2", "Khalim's Will", "Cube the flail with the eye, brain and heart, then smash the Compelling Orb")]),
    85: dict(quests=[("W", "qey", "Khalim's Will", "The eye is in a chest")]),
    86: dict(official="Bookstore Sarina (formely Ruined Temple)", notes=[("O", "", "Special area, entered from the Kurast Bazaar")]),
    87: dict(official="Harpie's Nest", notes=[("O", "", "Special area, entered from the Kurast Causeway")]),
    91: dict(quests=[("O", "qbr", "Khalim's Will", "The brain is in a chest")]),
    92: dict(official="The Sewer Level 1",
             notes=[("W", "", "Only the way in from the Flayer Jungle is one way; the Kurast entrances work both ways")]),
    93: dict(official="The Sewer Level 2", quests=[("O", "qhr", "Khalim's Will", "The heart is in a chest")]),
    94: dict(quests=[("O", "bbb", "Lam Esen's Tome", "The tome sits on a bookshelf")],
             notes=[("O", "", "The book can still be fetched after Baal is dead")]),
    102: dict(bosses=["Mephisto"], quests=[("W", "mss", "The Guardian", "Mephisto drops his Soulstone")], notes=[WALLS_NOTE]),
    105: dict(bosses=["Izual"], quests=[("W", "", "The Fallen Angel", "Kill Izual")]),
    107: dict(bosses=["Hephasto the Armorer"],
              quests=[("W", "hfh", "Hell's Forge", "Hephasto drops the hammer: smash the Soulstone on the Hellforge")]),
    108: dict(official="Chaos Sanctuary", bosses=["Diablo"],
              quests=[("W", "", "Terror's End", "Open the five seals, then kill Diablo")], notes=[WALLS_NOTE]),
    110: dict(bosses=["Shenk the Overseer"], quests=[("W", "", "Siege on Harrogath", "Kill Shenk the Overseer")],
              notes=[("W", "", "There is no waypoint here, though the official page shows one")]),
    111: dict(official="Frigid Highland", quests=[("W", "", "Rescue on Mount Arreat", "Free the captured barbarians")]),
    113: dict(special=True, notes=[("O", "", "Special area, below the Dead End")]),
    114: dict(quests=[("O", "ice", "Prison of Ice", "Free Anya with the potion")],
              notes=[("T", "", "Dead end: go on through the red portal in Harrogath once Anya is free")]),
    115: dict(official="Naraku", bosses=["Treant King"],
              notes=[("O", "", "Special area, entered from the Ancients' Way"),
                     ("O", "w", "Naraku and everything below it is very hard, meant for very well geared players")]),
    116: dict(official="Bill Roper Memorial Ballpark", special=True, bosses=["Reporb"],
              notes=[("O", "", "Special area, off Naraku Level 2")]),
    119: dict(official="GFraizer Dome", special=True, bosses=["Reziarfg"],
              notes=[("W", "", "Reached from Naraku Level 1 and Level 2 (the txt files link Level 1 only)")]),
    120: dict(bosses=["Ancient Barbarians"], quests=[("O", "", "Rite of Passage", "Beat the three Ancients")]),
    121: dict(bosses=["Pindleskin"],
              notes=[("W", "", "The red portal in Harrogath stays and works both ways, even after you take this waypoint")],
              entry=("W", "Red portal in Harrogath once Anya is free")),
    122: dict(official="Dead End", special=True,
              notes=[("O", "", "Special area, below Naraku Level 3"),
                     ("O", "w", "The stairs down are hidden, but a landmark always marks them"),
                     ("O", "s", "Hint: Sp***ling C***t")]),
    123: dict(special=True, notes=[("O", "", "Special area, entered from Nowhere")]),
    124: dict(special=True, bosses=["Nihlathak"], notes=[("O", "", "Killing Nihlathak is not needed to clear a difficulty")]),
    125: dict(notes=[("O", "", "Special area, off the Frigid Highlands")], entry=("W", "Red portal in the Frigid Highlands")),
    126: dict(notes=[("O", "", "Special area, off the Arreat Plateau")], entry=("W", "Red portal on the Arreat Plateau")),
    127: dict(notes=[("O", "", "Special area, off the Frozen Tundra")], entry=("W", "Red portal in the Frozen Tundra")),
    129: dict(notes=[("O", "", "Leads straight to the Throne of Destruction; ESR took out Level 3")]),
    130: dict(special=True),
    132: dict(official="Worldstone Chamber", bosses=["Baal"], quests=[("W", "", "Eve of Destruction", "Kill Baal")]),
    137: dict(special=True, entryunknown=True,
              notes=[("V", "", "Added to Diablo II: Resurrected by Reign of the Warlock: five Colossal Ancient Statues "
                                "cubed in Act V Hell open a red portal here"),
                     ("t", "", "ESR has the Eternal Ancients, but neither the txt files nor the official pages say how "
                                "to get here: no statue item, no cube recipe")]),
    138: dict(special=True, bosses=["Nosliwyaj"], notes=[("t", "", "Boss name taken from the string table")]),
    214: dict(special=True),
    133: dict(tab=UBER_TAB, special=True, bosses=["Lilith"],
              quests=[("W", "dhn", "Pandemonium", "Lilith drops the horn")], entry=("W", PANDEMONIUM)),
    134: dict(tab=UBER_TAB, special=True, bosses=["Uber Duriel"],
              quests=[("W", "bey", "Pandemonium", "Uber Duriel drops the eye")], entry=("W", PANDEMONIUM)),
    135: dict(tab=UBER_TAB, special=True, bosses=["Uber Izual"],
              quests=[("W", "mbr", "Pandemonium", "Uber Izual drops the brain")], entry=("W", PANDEMONIUM)),
    136: dict(tab=UBER_TAB, special=True, official="Uber Tristram", bosses=["Uber Mephisto", "Uber Diablo", "Uber Baal"],
              entry=("W", "Cube Diablo's Horn, Baal's Eye and Mephisto's Brain in Harrogath")),
    213: dict(tab=UBER_TAB, notes=[("T", "", "The unique map (Goblin Realm)")],
              entry=("T", "Transmute its map item by itself in Act V Hell: a red portal opens here")),
}

SECTION_NOTES = {
    "act1": [("O", "", "Removed: Jail Level 2. Jail Level 1 goes straight on to the old Level 3, now called Level 2"),
             ("O", "", "Removed: Catacombs Level 3. Level 2 goes straight on to the old Level 4 (Andariel), now called Level 3")],
    "act2": [("O", "", "Removed: the old Sewers Level 1 and Palace Level 1"),
             ("O", "", "The Viper Amulet can be had without visiting Lost Farm (formely Lost City)"),
             ("W", "", "Every character starts with the Horadric Cube; Radament does not drop the Horadric Scroll and it is not needed")],
    "act3": [("o", "w", "The tunnel from the Flayer Jungle into Sewers Level 1 is one way; the Kurast entrances work both ways"),
             ("O", "", "Bookstore Sarina (formely Ruined Temple) can be visited for the book after Baal")],
    "act4": [],
    "act5": [("T", "", "Frozen River is a dead end: once Anya is free, a red portal in Harrogath leads to the Dimensional Gate"),
             ("W", "", "That red portal works both ways and stays after you take the Dimensional Gate waypoint"),
             ("O", "", "Removed: Worldstone Keep Level 3. Level 2 goes straight on to the Throne of Destruction"),
             ("O", "w", "Naraku and below are extremely hard; Nihlathak does not have to die to clear a difficulty")],
    "special": [("O", "", "Special areas such as the Secret Cow Level give the best experience and drops: go after "
                          "beating the difficulty, or earlier with a strong character"),
                ("O", "w", "Naraku and below are for very well geared players only")],
    "portals": [("T", "", "Cow Portal: cube a Club and a Tome of Town Portal in the Rogue Encampment"),
                ("T", "", "Pandemonium Portal: cube the Keys of Terror, Hate and Destruction in Harrogath; it opens "
                          "to Matron's Den, Forgotten Sands or Furnace of Pain at random"),
                ("T", "", "Pandemonium Finale Portal: cube Diablo's Horn, Baal's Eye and Mephisto's Brain in Harrogath"),
                ("O", "", "Endgame maps: transmute a map item by itself in Act V Hell and a red portal opens to the map")],
}

# ---- reading ------------------------------------------------------------------------------------------------------

COLOR_CODE = re.compile("ÿc.")
ZONE_SUFFIX = " (Endgame Zone)"
TRAILING_PAREN = re.compile(r"\s*[(（][^()（）]*[)）]\s*$")


def fail(msg):
    sys.exit("build_campaign: " + msg)


def read_strings(path):
    with open(path, encoding="utf-8-sig") as f:
        entries = json.load(f)
    table = {}
    for e in entries:
        table.setdefault(e["Key"], e)  # first entry wins, like the game
    return table


def clean(text):
    return COLOR_CODE.sub("", text).replace("\t", " ").replace("\n", " ").strip()


def field(text):
    if "\t" in text or "\n" in text:
        fail("tab or newline in a field: %r" % text)
    return text


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--excel", default=os.path.expanduser("~/mnt/excel"))
    ap.add_argument("--strings", default=os.path.expanduser(
        "~/mnt/Eastern_Sun_Resurrected/Eastern_Sun_Resurrected.mpq/data/local/lng/strings"))
    ap.add_argument("--out", default=os.path.join(here, "..", "data", "campaign-map.txt"))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    with open(os.path.join(args.excel, "levels.txt"), encoding="utf-8", errors="replace", newline="") as f:
        rows = list(csv.reader(f, delimiter="\t", quoting=csv.QUOTE_NONE))
    head = rows[0]
    ix = {name: i for i, name in enumerate(head)}
    for need in ("Id", "Act", "LevelName", "*StringName", "Waypoint", "Teleport", "PreventTownPortal", "LevelType",
                 "*IsInside", "*Map Boss", "MonLvlEx", "MonLvlEx(N)", "MonLvlEx(H)", "Vis0"):
        if need not in ix:
            fail("levels.txt has no column " + need)
    levels_json = read_strings(os.path.join(args.strings, "levels.json"))
    item_json = read_strings(os.path.join(args.strings, "item-names.json"))

    def num(row, name):
        v = row[ix[name]].strip() if ix[name] < len(row) else ""
        try:
            return int(v)
        except ValueError:
            return 0

    def text(row, name):
        return row[ix[name]].strip() if ix[name] < len(row) else ""

    mon_cols = [c for c in head if re.fullmatch(r"(mon|nmon|umon)\d+", c)]
    raw = {}
    for row in rows[1:]:
        if len(row) < len(head) // 2 or text(row, "Id") == "":
            continue
        raw[num(row, "Id")] = row
    vis = {}
    for i, row in raw.items():
        targets = []
        for k in range(8):
            t = num(row, "Vis%d" % k)
            if t and t != i and t not in targets:
                targets.append(t)
        vis[i] = targets

    ids = sorted(i for i in raw if (1 <= i <= 138 or i in EXTRA_IDS) and i not in REMOVED_LEVELS)
    idset = set(ids)
    for k in AREAS:
        if k not in idset:
            fail("curated area %d is not a campaign row" % k)

    warnings = []
    areas = {}
    for i in ids:
        row = raw[i]
        key = text(row, "LevelName")
        strname = text(row, "*StringName")
        entry = levels_json.get(key) or item_json.get(key)
        names = {}
        if entry is not None:
            for lang in LANGS:
                v = clean(entry.get(lang) or "")
                if v:
                    names[lang] = v
        if "enUS" not in names:
            names["enUS"] = NAME_FALLBACK.get(i) or strname
            if i not in NAME_FALLBACK:
                warnings.append("area %d (%s): no string, English *StringName used" % (i, key))
        zone = names["enUS"].endswith(ZONE_SUFFIX)
        if zone:
            names = {lang: TRAILING_PAREN.sub("", v) for lang, v in names.items()}
        cur = AREAS.get(i, {})
        act = num(row, "Act")
        lt = num(row, "LevelType")
        inside = text(row, "*IsInside") == "1"
        terrain = TERRAIN.get(lt, ("dungeon", "dungeon") if inside else ("outdoor", "outdoor"))[1 if inside else 0]
        if lt not in TERRAIN:
            warnings.append("area %d: unknown LevelType %d" % (i, lt))
        lvl = [num(row, "MonLvlEx"), num(row, "MonLvlEx(N)"), num(row, "MonLvlEx(H)")]
        has_mon = any(text(row, c) for c in mon_cols) or any(
            num(row, c) for c in ("MonDen", "MonDen(N)", "MonDen(H)", "MonUMin", "MonUMax", "MonUMin(N)",
                                  "MonUMax(N)", "MonUMin(H)", "MonUMax(H)") if c in ix) or text(row, "*Map Boss")
        if not has_mon and not cur.get("bosses"):  # a boss alone (Pindleskin) still gives the area a level
            lvl = [0, 0, 0]
        wp = text(row, "Waypoint")
        flags = []
        if terrain == "town":
            flags.append("town")
        special = zone or cur.get("special", False)
        if special:
            flags.append("special")
        if zone:
            flags.append("zone")
        if num(row, "PreventTownPortal"):
            flags.append("notp")
        tele = num(row, "Teleport")
        if tele == 0:
            flags.append("teleoff")
        elif tele == 2:
            flags.append("telelimit")
        if i in OFFICIAL_WALLS:
            flags.append("walls")
        if cur.get("entryunknown"):
            flags.append("entryunknown")
        bosses = list(cur.get("bosses", []))
        mb = text(row, "*Map Boss")
        if mb and mb not in bosses:
            bosses.append(mb)
        tab = cur.get("tab") or TABS[act][0]
        areas[i] = dict(id=i, act=act, tab=tab, terrain=terrain, wp=1 if wp not in ("", "255") else 0, lvl=lvl,
                        flags=flags, names=names, bosses=bosses, cur=cur, twin=0)

    # passages: mutual Vis links between campaign rows
    edges = []
    passage_at = {}
    for i in ids:
        for t in vis[i]:
            if t not in vis:
                warnings.append("area %d: Vis names unknown level %d" % (i, t))
                continue
            if i not in vis[t]:
                if (i, t) not in EXPECTED_ONE_WAY_VIS and t not in REMOVED_LEVELS and i not in REMOVED_LEVELS:
                    warnings.append("area %d: Vis %d does not link back; dropped" % (i, t))
                continue
            if t in idset and t > i:
                passage_at[(i, t)] = len(edges)
                edges.append([i, t, "passage", 0, "T", ""])
    for link in LINKS:
        a, b, kind, one, via, src = link[:6]
        missing = len(link) > 6
        for x in (a, b):
            if x not in idset:
                fail("link %d-%d names %d, not a campaign row" % (a, b, x))
        k = (min(a, b), max(a, b))
        e = [a, b, kind, one, src, via]
        if k in passage_at:
            if kind != "passage":
                fail("link %d-%d (%s) is already a passage in levels.txt" % (a, b, kind))
            edges[passage_at[k]] = e
        elif kind == "passage" and not missing:
            fail("link %d-%d restyles a passage levels.txt no longer has" % (a, b))
        else:
            edges.append(e)

    # twins: same act, name and passage neighbours (Tal Rasha's Tomb x7) fold into the lowest id
    def mutual(i):
        return tuple(sorted(t for t in vis[i] if t in vis and i in vis[t]))
    groups = {}
    for i in ids:
        groups.setdefault((areas[i]["act"], areas[i]["names"]["enUS"], mutual(i)), []).append(i)
    for g in groups.values():
        for i in g[1:]:
            areas[i]["twin"] = g[0]

    # grids: tokens, placement checks
    tab_keys = [t[0] for t in TABS]
    placed = {}
    reading = {}
    grid_rows = {}
    for key, g in GRIDS.items():
        lines = [ln.split("#")[0].split() for ln in g["grid"].split("\n")]
        lines = [ln for ln in lines if ln]
        grid_rows[key] = lines
        order = []
        for r, toks in enumerate(lines):
            for c, tok in enumerate(toks):
                if tok == ".":
                    continue
                ghost = tok.startswith("@")
                if ghost:
                    cell = [int(tok[1:])]
                elif ".." in tok:
                    lo, hi = tok.split("..")
                    cell = list(range(int(lo), int(hi) + 1))
                else:
                    cell = [int(x) for x in tok.split("+")]
                for x in cell:
                    if x not in idset:
                        fail("%s: grid id %d is not a campaign row" % (key, x))
                if ghost:
                    if areas[cell[0]]["tab"] == key:
                        fail("%s: ghost @%d is drawn on this tab" % (key, cell[0]))
                    continue
                for x in cell:
                    if areas[x]["tab"] != key:
                        fail("%s: area %d belongs to %s" % (key, x, areas[x]["tab"]))
                    if x in placed:
                        fail("%s: area %d placed twice" % (key, x))
                    placed[x] = key
                    order.append(x)
        reading[key] = order
    unplaced = [i for i in ids if areas[i]["tab"] in GRIDS and i not in placed
                and not (areas[i]["twin"] and areas[i]["twin"] in placed)]
    for i in unplaced:
        warnings.append("area %d (%s) is on %s but not in its grid" % (i, areas[i]["names"]["enUS"], areas[i]["tab"]))
    for key, g in GRIDS.items():
        for (a, b) in g["routes"]:
            if not any((e[0], e[1]) in ((a, b), (b, a)) for e in edges):
                fail("%s: route hint %d-%d has no edge" % (key, a, b))

    # cards
    rank = {}
    for key in tab_keys:
        for x in reading.get(key, []):
            rank.setdefault(x, len(rank))
    special_cards = sorted((i for i in ids if "special" in areas[i]["flags"] and areas[i]["tab"] != UBER_TAB),
                           key=lambda i: (rank.get(i, 1 << 30), i))
    portal_cards = []
    for e in edges:
        if e[2] == "cube" and e[1] not in portal_cards:
            portal_cards.append(e[1])
    for i in ids:
        if (areas[i]["tab"] == UBER_TAB or "entryunknown" in areas[i]["flags"]) and i not in portal_cards:
            portal_cards.append(i)
    cards = {"special": special_cards, "portals": portal_cards}

    # quest items
    items = {}
    for i in ids:
        for (_s, code, _q, _h) in areas[i]["cur"].get("quests", []):
            if not code or code in items:
                continue
            e = item_json.get(code)
            names = {}
            if e is not None:
                for lang in LANGS:
                    v = clean(e.get(lang) or "")
                    if v:
                        names[lang] = v
            if "enUS" not in names:
                if code not in ITEM_FALLBACK:
                    fail("quest item %s has no name" % code)
                names = {"enUS": ITEM_FALLBACK[code]}
            items[code] = names

    # ---- write ----
    out = ["#ESRCAMPAIGN\t1",
           "# Campaign Progression map. Built by tools/build_campaign.py from levels.txt, levels.json, item-names.json",
           "# and curated tables ported from d2r-esr-runeword-browser (MIT, see THIRD_PARTY_NOTICES.md). Do not edit.",
           "# Sources: O official Maps page, T txt files, V vanilla Diablo II, W confirmed in game; lowercase = unverified.",
           "LANGS\t" + "\t".join(LANGS)]

    def flags_of(f):
        return f if f else "-"
    for key, kind, act, title in TABS:
        out.append("\t".join(["TAB", key, kind, str(act), title]))
        if key in GRIDS:
            for toks in grid_rows[key]:
                out.append("\t".join(["GRID", key, " ".join(toks)]))
            for (label, x0, y0, x1, y1) in GRIDS[key]["regions"]:
                out.append("\t".join(["REGION", key, repr(x0), repr(y0), repr(x1), repr(y1), field(label)]))
            for (a, b), mode in sorted(GRIDS[key]["routes"].items()):
                out.append("\t".join(["ROUTE", key, str(a), str(b), mode]))
        for i in cards.get(key, []):
            out.append("\t".join(["CARD", key, str(i)]))
        for (src, fl, txt) in SECTION_NOTES.get(key, []):
            out.append("\t".join(["SNOTE", key, src, flags_of(fl), field(txt)]))
    for i in ids:
        a = areas[i]
        cur = a["cur"]
        out.append("\t".join(["AREA", str(i), a["tab"], str(a["act"]), a["terrain"], str(a["wp"])]
                             + [str(v) for v in a["lvl"]] + [",".join(a["flags"]) or "-", str(a["twin"])]))
        for lang in LANGS:
            if lang in a["names"]:
                out.append("\t".join(["NAME", str(i), lang, field(a["names"][lang])]))
        if cur.get("official") and cur["official"] != a["names"]["enUS"]:
            out.append("\t".join(["OFFICIAL", str(i), field(cur["official"])]))
        for b in a["bosses"]:
            out.append("\t".join(["BOSS", str(i), field(b)]))
        for (src, code, quest, how) in cur.get("quests", []):
            out.append("\t".join(["QUEST", str(i), src, code or "-", field(quest), field(how)]))
        for (src, fl, txt) in cur.get("notes", []):
            out.append("\t".join(["NOTE", str(i), src, flags_of(fl), field(txt)]))
        if cur.get("entry"):
            out.append("\t".join(["ENTRY", str(i), cur["entry"][0], field(cur["entry"][1])]))
    for e in edges:
        out.append("\t".join(["EDGE", str(e[0]), str(e[1]), e[2], str(e[3]), e[4], field(e[5])]))
    for code in sorted(items):
        for lang in LANGS:
            if lang in items[code]:
                out.append("\t".join(["ITEM", code, lang, field(items[code][lang])]))
    data = ("\n".join(out) + "\n").encode("utf-8")

    for w in warnings:
        print("warning: " + w, file=sys.stderr)
    missing = sum(1 for i in ids for lang in LANGS if lang not in areas[i]["names"])
    print("areas %d, edges %d, names %d/%d, unplaced %d, %d bytes" % (
        len(ids), len(edges), len(ids) * len(LANGS) - missing, len(ids) * len(LANGS), len(unplaced), len(data)),
        file=sys.stderr)
    if len(data) > 300 * 1024:
        fail("output is over 300 KB")
    path = os.path.normpath(args.out)
    if args.check:
        try:
            with open(path, "rb") as f:
                same = f.read() == data
        except OSError:
            same = False
        print("up to date" if same else "would change", file=sys.stderr)
        sys.exit(0 if same else 1)
    with open(path, "wb") as f:
        f.write(data)
    print("wrote " + path, file=sys.stderr)


if __name__ == "__main__":
    main()
