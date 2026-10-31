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

# The words the builder looks for, per locale. "en" is the site's English page; the others are read from the
# translated pages (data/locales/<code>/). The tab titles are the overlay's own names for the site tabs.
LOCALES = {
 "en": {
  "cats": {c[0]: c[1] for c in CATS}, "input": "Input", "output": "Output", "notes": "Notes",
  "class_headings": CLASS_CRAFT_HEADINGS,
  "tinker_from": "See also Supercharged Tinkering.", "tinker_to": "See also Supercharged Tinkering (Secret Recipe 22).",
 },
 "frFR": {  # draft: the tab titles need a native check
  "cats": {"general": "Général", "mat": "Matériaux", "reroll": "Relance d'objet", "old": "Ancienne fabrication",
           "for": "Forge", "dst": "Pierre D", "tin": "Bricolage", "bas": "Amélioration de base",
           "soc": "Châsses", "sec": "Secret"},
  "input": "Entrée", "output": "Sortie", "notes": "Remarques",
  "class_headings": {"Fabrication de classe", "Fabrication d'Amazone", "Fabrication d'Assassin",
                     "Fabrication de Barbare", "Fabrication de Druide", "Fabrication de Nécromancien",
                     "Fabrication de Paladin", "Fabrication d'Ensorceleuse", "Fabrication de Démoniste"},
  "tinker_from": "Voir aussi Bricolage surchargé.", "tinker_to": "Voir aussi Bricolage surchargé (recette secrète 22).",
 },
 "deDE": {  # draft: tab titles and Notes need a native check
  "cats": {'general': 'Allgemein', 'mat': 'Material', 'reroll': 'Gegenstand neu würfeln', 'old': 'Altes Crafting', 'for': 'Schmieden', 'dst': 'D-Stein', 'tin': 'Basteln', 'bas': 'Basis-Upgrade', 'soc': 'Sockel', 'sec': 'Geheim'},
  "input": 'Eingabe', "output": 'Ausgabe', "notes": 'Hinweise',
  "class_headings": {'Fertigen: Amazone', 'Fertigen: Barbar', 'Fertigen: Assassine', 'Fertigen: Totenbeschwörer', 'Klassen-Fertigen', 'Fertigen: Paladin', 'Fertigen: Hexenmeister', 'Fertigen: Zauberin', 'Fertigen: Druide'},
  "tinker_from": None, "tinker_to": None,
 },
 "esES": {  # draft: tab titles and Notes need a native check
  "cats": {'general': 'General', 'mat': 'Materiales', 'reroll': 'Reroll de objeto', 'old': 'Crafteo heredado', 'for': 'Forjado', 'dst': 'Piedra D', 'tin': 'Tinkering', 'bas': 'Mejora base', 'soc': 'Engastes', 'sec': 'Secreto'},
  "input": 'Entrada', "output": 'Salida', "notes": 'Notas',
  "class_headings": {'Fabricación de druida', 'Fabricación de clase', 'Fabricación de nigromante', 'Fabricación de bárbaro', 'Fabricación de hechicera', 'Fabricación de paladín', 'Fabricación de amazona', 'Fabricación de brujo', 'Fabricación de asesina'},
  "tinker_from": None, "tinker_to": None,
 },
 "itIT": {  # draft: tab titles and Notes need a native check
  "cats": {'general': 'Generale', 'mat': 'Materiali', 'reroll': 'Reroll oggetti', 'old': 'Crafting legacy', 'for': 'Forgiatura', 'dst': 'Pietra D', 'tin': 'Tinkering', 'bas': 'Miglioramento base', 'soc': 'Incastonature', 'sec': 'Segreto'},
  "input": 'Ingresso', "output": 'Risultato', "notes": 'Note',
  "class_headings": {'Creazione di classe', 'Creazione Amazzone', 'Creazione Stregone', 'Creazione Incantatrice', 'Creazione Druido', 'Creazione Assassina', 'Creazione Negromante', 'Creazione Paladino', 'Creazione Barbaro'},
  "tinker_from": None, "tinker_to": None,
 },
 "koKR": {  # draft: tab titles and Notes need a native check
  "cats": {'general': '일반', 'mat': '재료', 'reroll': '아이템 리롤', 'old': '구 제작', 'for': '단조', 'dst': 'D석', 'tin': '팅커링', 'bas': '기본 강화', 'soc': '소켓', 'sec': '비밀'},
  "input": '입력', "output": '출력', "notes": '메모',
  "class_headings": {'성기사 제작', '어쌔신 제작', '드루이드 제작', '아마존 제작', '원소술사 제작', '워록 제작', '야만용사 제작', '직업 제작', '강령술사 제작'},
  "tinker_from": None, "tinker_to": None,
 },
 "plPL": {  # draft: tab titles and Notes need a native check
  "cats": {'general': 'Ogólne', 'mat': 'Materiały', 'reroll': 'Przerzucanie przedmiotów', 'old': 'Stare wytwarzanie', 'for': 'Kucie', 'dst': 'Kamień D', 'tin': 'Tinkering', 'bas': 'Ulepszenie bazy', 'soc': 'Gniazda', 'sec': 'Sekret'},
  "input": 'Wejście', "output": 'Wynik', "notes": 'Uwagi',
  "class_headings": {'Wytwarzanie klasowe', 'Wytwarzanie: Barbarzyńca', 'Wytwarzanie: Paladyn', 'Wytwarzanie: Amazonka', 'Wytwarzanie: Zabójczyni', 'Wytwarzanie: Czarnoksiężnik', 'Wytwarzanie: Nekromanta', 'Wytwarzanie: Czarodziejka', 'Wytwarzanie: Druid'},
  "tinker_from": None, "tinker_to": None,
 },
 "ruRU": {  # draft: tab titles and Notes need a native check
  "cats": {'general': 'Общее', 'mat': 'Материалы', 'reroll': 'Перебрасывание предметов', 'old': 'Старый крафт', 'for': 'Ковка', 'dst': 'Камень D', 'tin': 'Тинкеринг', 'bas': 'Улучшение базы', 'soc': 'Гнёзда', 'sec': 'Секрет'},
  "input": 'Вход', "output": 'Результат', "notes": 'Примечания',
  "class_headings": {'Создание: Друид', 'Создание: Варвар', 'Создание: Амазонка', 'Классовое создание', 'Создание: Волшебница', 'Создание: Некромант', 'Создание: Паладин', 'Создание: Ассасин', 'Создание: Чернокнижник'},
  "tinker_from": None, "tinker_to": None,
 },
 "zhCN": {  # draft: tab titles and Notes need a native check
  "cats": {'general': '通用', 'mat': '材料', 'reroll': '物品重骰', 'old': '旧版制作', 'for': '锻造', 'dst': 'D石', 'tin': '调校', 'bas': '基础升级', 'soc': '插槽', 'sec': '秘密'},
  "input": '输入', "output": '输出', "notes": '备注',
  "class_headings": {'魔法师合成', '亚马逊合成', '野蛮人合成', '刺客合成', '死灵法师合成', '圣骑士合成', '术士合成', '德鲁伊合成', '职业合成'},
  "tinker_from": None, "tinker_to": None,
 },
 "zhTW": {  # draft: tab titles and Notes need a native check
  "cats": {'general': '一般', 'mat': '材料', 'reroll': '物品重骰', 'old': '舊版製作', 'for': '鍛造', 'dst': 'D石', 'tin': '調校', 'bas': '基礎升級', 'soc': '插槽', 'sec': '秘密'},
  "input": '輸入', "output": '輸出', "notes": '備註',
  "class_headings": {'刺客合成', '德魯伊合成', '職業合成', '魔法師合成', '死靈法師合成', '亞馬遜合成', '術士合成', '野蠻人合成', '聖騎士合成'},
  "tinker_from": None, "tinker_to": None,
 },
 "esMX": {  # draft: tab titles and Notes need a native check
  "cats": {'general': 'General', 'mat': 'Materiales', 'reroll': 'Reroll de objeto', 'old': 'Crafteo heredado', 'for': 'Forjado', 'dst': 'Piedra D', 'tin': 'Tinkering', 'bas': 'Mejora base', 'soc': 'Engastes', 'sec': 'Secreto'},
  "input": 'Entrada', "output": 'Salida', "notes": 'Notas',
  "class_headings": {'Fabricación de druida', 'Fabricación de clase', 'Fabricación de nigromante', 'Fabricación de bárbaro', 'Fabricación de hechicera', 'Fabricación de paladín', 'Fabricación de amazona', 'Fabricación de brujo', 'Fabricación de asesina'},
  "tinker_from": None, "tinker_to": None,
 },
 "jaJP": {  # draft: tab titles and Notes need a native check
  "cats": {'general': '一般', 'mat': '素材', 'reroll': 'アイテムリロール', 'old': '旧クラフト', 'for': '鍛造', 'dst': 'Dストーン', 'tin': 'ティンカリング', 'bas': 'ベースアップグレード', 'soc': 'ソケット', 'sec': 'シークレット'},
  "input": '入力', "output": '出力', "notes": 'メモ',
  "class_headings": {'ネクロマンサークラフト', 'パラディンクラフト', 'アマゾンクラフト', 'ドルイドクラフト', 'バーバリアンクラフト', 'アサシンクラフト', 'ソーサレスクラフト', 'クラスクラフト', 'ウォーロッククラフト'},
  "tinker_from": None, "tinker_to": None,
 },
 "ptBR": {  # draft: tab titles and Notes need a native check
  "cats": {'general': 'Geral', 'mat': 'Materiais', 'reroll': 'Reroll de item', 'old': 'Crafting legado', 'for': 'Forja', 'dst': 'Pedra D', 'tin': 'Tinkering', 'bas': 'Melhoria de base', 'soc': 'Soquetes', 'sec': 'Secreto'},
  "input": 'Entrada', "output": 'Resultado', "notes": 'Notas',
  "class_headings": {'Criação de Amazona', 'Criação de Assassina', 'Criação de Druida', 'Criação de Bárbaro', 'Criação de Bruxo', 'Criação de Necromante', 'Criação de classe', 'Criação de Paladino', 'Criação de Feiticeira'},
  "tinker_from": None, "tinker_to": None,
 },
}

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

def main(src, out, locale="en"):
    L = LOCALES[locale]
    p = P(); p.feed(open(src, encoding="utf-8", errors="replace").read())
    lines = ["#ESRCUBE\t1"]
    for cid, title, group in CATS:
        lines.append("\t".join(["CAT", cid, L["cats"][cid], group]))
    counts = {"REC":0,"SEC":0,"DESC":0,"NOTE":0}
    skipped = []
    has_section = set()     # overlay pages that already have a section heading
    in_class_craft = False  # inside the class crafting sections of Legacy Craft
    def emit(kind, cat, *fields):
        nonlocal in_class_craft
        if cat == "old":
            if kind == "SEC":
                in_class_craft = fields[0] in L["class_headings"]
            if in_class_craft:
                return
        if kind == "SEC":
            has_section.add(cat)
        elif cat == "general" and cat not in has_section:
            # Notes before the first heading of the General page get a heading of their own.
            lines.append("\t".join(["SEC", cat, L["notes"]])); counts["SEC"] += 1
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
                if txt in (L["input"], L["output"]): continue
                # A short line is a heading, unless it is a sentence; a long one is a description.
                kind = "SEC" if len(txt) <= 60 and not txt.endswith(".") else "NOTE" if len(txt) <= 60 else "DESC"
                emit(kind, cat, txt)
            elif len(r) == 2 and r[0]["bg"] == "#300000" and r[1]["bg"] == "#300000":
                if cell_text(r[0]) in (L["input"], L["output"]) and cell_text(r[1]) in (L["input"], L["output"]): continue
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
    if L["tinker_from"]:
        text = text.replace(L["tinker_from"], L["tinker_to"])
    open(out, "w", encoding="utf-8").write(text)
    print(counts, "skipped:", len(skipped))
    for s in skipped[:10]: print(" ", s)

main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "en")
