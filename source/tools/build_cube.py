import sys, re
from html.parser import HTMLParser

COLOR = {"4850b8": "3", "yellow": "9", "orange": "8", "red": "1", "cyan": "3"}
# The site's tabs, as the overlay shows them: several site tabs are merged into one page.
CATS = [  # id, title, group  (page order)
 ("general","General","Items"),("mat","Material","Items"),("reroll","Item Reroll","Items"),
 ("old","Legacy Craft","Items"),("for","Forging","Modify"),("dst","D-Stone","Modify"),
 ("tin","Tinker","Modify"),("bas","Base Upgrade","Modify"),("soc","Sockets","Modify"),
 ("sec","Secret","Modify"),
]
# Site anchor (tab) -> overlay page. The "new" tab (NewCraft) is not shown.
SITE_TABS = {
 "special":"general","uber":"general","legendary":"general","mis":"general","tyrael":"general",
 "gem":"mat","rel":"mat","mat":"mat",
 "nor":"reroll","mag":"reroll","uni":"reroll","set":"reroll","rin":"reroll","cha":"reroll","jew":"reroll","qui":"reroll",
 "old":"old","for":"for","dst":"dst","tin":"tin","bas":"bas","soc":"soc","sec":"sec",
}
# Class crafting has its own page in the main wiki, so its sections are left out of Legacy Craft.
CLASS_CRAFT_HEADINGS = {"Class Craft"} | {c + " Craft" for c in (
    "Amazon", "Assassin", "Barbarian", "Druid", "Necromancer", "Paladin", "Sorceress", "Warlock")}
CAT_IDS = set(SITE_TABS) | {"new"}

class P(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.cat = "special"; self.tables = []; self.table = None
        self.row = None; self.cell = None; self.depth = 0; self.fonts = []
    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "a" and a.get("name") in CAT_IDS and a.get("name") != "special":
            self.cat = a["name"]
        if tag == "table":
            self.depth += 1
            if self.depth == 1: self.table = {"cat": self.cat, "rows": []}
        elif tag == "tr" and self.depth == 1 and self.table is not None:
            self.row = []
        elif tag in ("td","th") and self.depth == 1 and self.row is not None:
            self.cell = {"bg": (a.get("bgcolor") or "").lower(), "parts": []}
            self.fonts = []
        elif tag == "font" and self.cell is not None:
            col = (a.get("color") or "").lower()
            code = COLOR.get(col)
            self.fonts.append(code)
            if code: self.cell["parts"].append("ÿc" + code)
        elif tag == "br" and self.cell is not None:
            self.cell["parts"].append("\n")
    def handle_endtag(self, tag):
        if tag == "table":
            if self.depth == 1 and self.table is not None:
                self.tables.append(self.table); self.table = None
            self.depth -= 1
        elif tag == "font" and self.cell is not None and self.fonts:
            if self.fonts.pop(): self.cell["parts"].append("ÿc0")
        elif tag == "tr" and self.depth == 1 and self.row is not None:
            self.table["rows"].append(self.row); self.row = None
        elif tag in ("td","th") and self.cell is not None and self.row is not None:
            for code in reversed(self.fonts):
                if code: self.cell["parts"].append("ÿc0")
            self.fonts = []
            txt = "".join(self.cell["parts"])
            lines = [re.sub(r"\s+", " ", l).strip() for l in txt.split("\n")]
            # tidy: a colour code left at line start/end with nothing after it
            clean = []
            for l in lines:
                l = re.sub(r"(ÿc\d)+$", "", l).strip()
                if l and l != "ÿc0": clean.append(l)
            self.row.append({"bg": self.cell["bg"], "lines": clean})
            self.cell = None
    def handle_data(self, data):
        if self.cell is not None: self.cell["parts"].append(data)

def cell_text(c):
    return " | ".join(c["lines"])

def main(src, out):
    p = P(); p.feed(open(src, encoding="utf-8", errors="replace").read())
    lines = ["#ESRCUBE\t1"]
    for cid, title, group in CATS:
        lines.append("\t".join(["CAT", cid, title, group]))
    counts = {"REC":0,"SEC":0,"DESC":0,"NOTE":0}
    skipped = []
    has_section = set()     # overlay pages that already have a section heading
    in_class_craft = False  # inside the class crafting sections of Legacy Craft
    def emit(kind, cat, *fields):
        nonlocal in_class_craft
        if cat == "old":
            if kind == "SEC":
                in_class_craft = fields[0] in CLASS_CRAFT_HEADINGS
            if in_class_craft:
                return
        if kind == "SEC":
            has_section.add(cat)
        elif cat == "general" and cat not in has_section:
            # Notes before the first heading of the General page get a heading of their own.
            lines.append("\t".join(["SEC", cat, "Notes"])); counts["SEC"] += 1
            has_section.add(cat)
        lines.append("\t".join([kind, cat, *fields])); counts[kind] += 1
    for t in p.tables:
        site_tab = t["cat"] or "special"
        if site_tab == "new":  # NewCraft tab was removed
            continue
        cat = SITE_TABS[site_tab]
        for r in t["rows"]:
            if not r: continue
            texts = [c["lines"] for c in r]
            if len(r) == 1 and r[0]["bg"] == "#300000":
                txt = " ".join(r[0]["lines"])
                if txt in ("Input","Output"): continue
                # A short line is a heading, unless it is a sentence; a long one is a description.
                kind = "SEC" if len(txt) <= 60 and not txt.endswith(".") else "NOTE" if len(txt) <= 60 else "DESC"
                emit(kind, cat, txt)
            elif len(r) == 2 and r[0]["bg"] == "#300000" and r[1]["bg"] == "#300000":
                if cell_text(r[0]) in ("Input","Output") and cell_text(r[1]) in ("Input","Output"): continue
                # sub-heading with a description pair
                txt = " ".join(r[0]["lines"]) if r[0]["lines"] else ""
                emit("SEC", cat, txt)
                if r[1]["lines"]:
                    emit("DESC", cat, " ".join(r[1]["lines"]))
            elif len(r) == 2 and r[0]["bg"] in ("#181818","") and r[1]["bg"] in ("#181818",""):
                a, b = cell_text(r[0]), cell_text(r[1])
                if not a and not b: continue
                emit("REC", cat, a, b)
            elif len(r) == 1 and r[0]["bg"] in ("#181818",""):
                emit("NOTE", cat, " ".join(r[0]["lines"]))
            else:
                skipped.append((cat, [ (c["bg"], c["lines"][:2]) for c in r ]))
    text = "\n".join(lines) + "\n"
    # Wording the overlay shows for the Tinkering cross-reference.
    text = text.replace("See also Supercharged Tinkering.", "See also Supercharged Tinkering (Secret Recipe 22).")
    open(out, "w", encoding="utf-8").write(text)
    print(counts, "skipped:", len(skipped))
    for s in skipped[:10]: print(" ", s)

main(sys.argv[1], sys.argv[2])
