"""Item property text written only from the mod's own data.

Inputs, all from the mod:
  excel/properties.txt    property code -> up to 7 steps (func, stat)
  excel/itemstatcost.txt  stat -> how the game describes it (descfunc, descpriority, string keys, groups)
  excel/skills.txt, skilldesc.txt, charstats.txt   skill, class and skill-tab names
  strings/*.json          the game's own English strings (enUS)

No wording is typed here: every phrase is a string key found in those files. Lines come back in the
order the game lists them (descpriority, highest first). A line that has no colour code of its own is
blue (ÿc3), like the game's magic properties.
"""
import csv
import glob
import json
import re
from pathlib import Path

BLUE = "ÿc3"


def read_rows(path):
    with open(path, encoding="utf-8-sig", errors="replace", newline="") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def number(text):
    try:
        value = float(text)
    except (TypeError, ValueError):
        return None
    return int(value) if value == int(value) else value


def fmt(value):
    if isinstance(value, float):
        return ("%.4f" % value).rstrip("0").rstrip(".")
    return str(value)


class Strings:
    """All enUS strings of the mod, looked up without regard to case."""

    PRIORITY = ("item-modifiers.json", "skills.json", "item-names.json", "item-nameaffixes.json")

    def __init__(self, folder):
        self.table = {}
        files = sorted(glob.glob(str(Path(folder) / "*.json")))
        files.sort(key=lambda p: (Path(p).name not in self.PRIORITY, Path(p).name))
        for path in files:
            try:
                data = json.loads(Path(path).read_text(encoding="utf-8-sig"))
            except (ValueError, OSError):
                continue
            for entry in data:
                key = str(entry.get("Key", "")).lower()
                text = entry.get("enUS")
                if key and text is not None:
                    self.table.setdefault(key, text)

    def get(self, key):
        return self.table.get((key or "").lower())


class Stat:
    def __init__(self, name, param, lo, hi, order):
        self.name, self.param, self.lo, self.hi, self.order = name, param, lo, hi, order


def span(lo, hi, signed):
    """'+1-3' / '1-3' / '+5' / '-5' / '-20-40' for a min-max pair."""
    a, b = number(lo), number(hi)
    if a is None:
        return ""
    if b is None:
        b = a
    if a == b:
        return ("%+d" % a if isinstance(a, int) else "%+g" % a) if signed else fmt(a)
    if a < 0 and b < 0:
        return "-%s-%s" % (fmt(-a), fmt(-b))
    first = ("+" if signed and a >= 0 else "") + fmt(a)
    return "%s-%s" % (first, fmt(b))


def fill(text, args):
    """Puts args into the %d/%+d/%s/%i holes of a game string, in order. %% is a percent sign.
    A plain %d written after a literal '+' ("+%d to Skills") takes the number without its own sign."""
    values = list(args)

    def take(match):
        token = match.group(0)
        if token == "%%":
            return "%"
        if not values:
            return token
        value = str(values.pop(0))
        if token == "%d" and value[:1] == "+" and match.start() > 0 and text[match.start() - 1] == "+":
            value = value[1:]
        return value
    return re.sub(r"%%|%[+]?[dis]", take, text)


class ModTooltip:
    def __init__(self, excel_folder, strings_folder):
        folder = Path(excel_folder)
        self.strings = Strings(strings_folder)
        self.props = {r["code"].strip(): r for r in read_rows(folder / "properties.txt") if r.get("code")}
        self.stats = {r["Stat"].strip(): r for r in read_rows(folder / "itemstatcost.txt") if r.get("Stat")}
        self.groups = {}
        for name, row in self.stats.items():
            group = (row.get("dgrp") or "").strip()
            if group:
                self.groups.setdefault(group, []).append(name)
        self.classes = [r for r in read_rows(folder / "charstats.txt") if (r.get("class") or "").strip()]
        self.skill_by_name = {}
        self.skill_by_id = {}
        desc_name = {}
        for row in read_rows(folder / "skilldesc.txt"):
            key = (row.get("skilldesc") or "").strip()
            if key:
                desc_name[key] = (row.get("str name") or "").strip()
        for row in read_rows(folder / "skills.txt"):
            name = (row.get("skill") or "").strip()
            if not name:
                continue
            record = {"name": name, "id": (row.get("*Id") or "").strip(),
                      "class": (row.get("charclass") or "").strip(),
                      "shown": self.strings.get(desc_name.get((row.get("skilldesc") or "").strip(), "")) or name}
            self.skill_by_name[name.lower()] = record
            if record["id"]:
                self.skill_by_id[record["id"]] = record
        class_code = {}
        for row in self.classes:
            class_code[(row.get("class") or "").strip().lower()] = row
        self.class_rows = class_code
        self.problems = set()

    # -- lookups -----------------------------------------------------------------------------------------------
    def skill(self, param):
        param = (param or "").strip()
        return self.skill_by_name.get(param.lower()) or self.skill_by_id.get(param)

    def class_only(self, charclass):
        code = (charclass or "").strip().lower()[:3]
        for name, row in self.class_rows.items():
            if code and name.startswith(code):
                return self.strings.get(row.get("StrClassOnly", "")) or ""
        return ""

    def tab_string(self, param):
        """Skill tab string key: the param is class * 3 + tab, in charstats.txt order (the expansion row skipped)."""
        index = number(param)
        if index is None:
            return None
        real = [r for r in self.classes if (r.get("StrSkillTab1") or "").strip()]
        # The game's class ids are the order of the classes without the "Expansion" separator row.
        cls, tab = divmod(int(index), 3)
        if cls >= len(real):
            return None
        return (real[cls].get("StrSkillTab%d" % (tab + 1)) or "").strip()

    def class_skill_key(self, param):
        real = [r for r in self.classes if (r.get("StrSkillTab1") or "").strip()]
        index = number(param)
        if index is None or int(index) >= len(real):
            return None
        return (real[int(index)].get("StrAllSkills") or "").strip()

    # -- property -> stats -----------------------------------------------------------------------------------------
    def stats_of(self, code, par, lo, hi):
        prop = self.props.get(code)
        if prop is None:
            return None
        out = []
        for i in range(1, 8):
            func = (prop.get("func%d" % i) or "").strip()
            stat = (prop.get("stat%d" % i) or "").strip()
            if func and not stat:
                # Steps the game runs in code, not through a stat column of properties.txt.
                stat = {"5": "mindamage", "6": "maxdamage", "7": "damagepercent",
                        "20": "item_indesctructible", "23": "<ethereal>"}.get(func, "")
            if not func or not stat:
                continue
            value_lo, value_hi, param = lo, hi, par
            val = (prop.get("val%d" % i) or "").strip()
            if func == "17" and stat.endswith("perlevel"):
                param = par
                value_lo = value_hi = ""
            elif func == "17":                      # duration of a damage-over-time property
                value_lo = value_hi = par
                param = ""
            elif func in ("15", "16"):
                param = ""
            elif func == "21":                      # class skill levels: the class id is val
                param = val
            elif func in ("22", "24") and stat in ("item_singleskill", "item_nonclassskill", "item_nonclassskill_display"):
                param = par
            elif func == "36":                      # random class skill levels: val is the number of levels
                stat = "<randclass>"
                value_lo = value_hi = val
                param = ""
            out.append(Stat(stat, param, value_lo, value_hi, len(out)))
        return out

    # -- rendering ------------------------------------------------------------------------------------------------
    def text_of(self, key, why=""):
        text = self.strings.get(key)
        if text is None:
            self.problems.add("no string '%s' %s" % (key, why))
        return text

    def one(self, stat):
        """Text for one stat; None when the game does not describe it; False when it cannot be written."""
        if stat.name == "<ethereal>":
            return self.text_of("strethereal", "for ethereal")
        if stat.name == "<randclass>":
            text = self.text_of("ChronicleRandomClassSkillLevel", "for random class skills")
            return False if text is None else fill(text, [span(stat.lo, stat.hi, True)])
        row = self.stats.get(stat.name)
        if row is None:
            self.problems.add("no stat " + stat.name)
            return False
        func = (row.get("descfunc") or "").strip()
        if stat.name == "item_numsockets":
            text = self.text_of("Socketable", "for sockets")
            return False if text is None else fill(text, [span(stat.lo, stat.hi, False)])
        if stat.name in self.TOOLTIP_STATS:
            template = self.tooltip_of(stat.name)
            return template and template.replace("#", span(stat.lo, stat.hi, False))
        if not func:
            return None
        a, b = number(stat.lo), number(stat.hi)
        negative = a is not None and a < 0 and (b is None or b <= 0)
        key = (row.get("descstrneg") if negative else row.get("descstrpos")) or row.get("descstrpos") or ""
        text = self.text_of(key.strip(), "for " + stat.name)
        if text is None:
            return False
        str2 = (row.get("descstr2") or "").strip()
        tail = (" " + self.strings.get(str2)) if str2 and self.strings.get(str2) else ""
        value = span(stat.lo, stat.hi, True)
        plain = span(stat.lo, stat.hi, False)
        if stat.name.endswith("perlevel"):
            per = number(stat.param)
            if per is None:
                return False
            value = span(per / 8, per / 8, True)
            plain = span(per / 8, per / 8, False)
        if func == "13":                            # class skill levels (the class is in the param)
            ckey = self.class_skill_key(stat.param)
            text = self.strings.get(ckey) if ckey else None
            return None if text is None else fill(text, [value])
        if func == "14":                            # skill tab
            tkey = self.tab_string(stat.param)
            text = self.strings.get(tkey) if tkey else None
            return None if text is None else fill(text, [value])
        if func == "15":                            # chance to cast a skill on an event
            skill = self.skill(stat.param)
            return False if skill is None else fill(text, [stat.lo, stat.hi, skill["shown"]])
        if func == "16":
            skill = self.skill(stat.param)
            return False if skill is None else fill(text, [plain, skill["shown"]])
        if func == "24":                            # charges: Level N Skill (c/c Charges)
            skill = self.skill(stat.param)
            return False if skill is None else fill(text, [stat.hi, skill["shown"], stat.lo, stat.lo])
        if func == "27":
            skill = self.skill(stat.param)
            if skill is None:
                return False
            return fill(text, [value, skill["shown"], self.class_only(skill["class"])]).strip()
        if func == "28":
            skill = self.skill(stat.param)
            return False if skill is None else fill(text, [value, skill["shown"]])
        if "%" not in text:
            descval = (row.get("descval") or "").strip()
            if func in ("11", "12", "20", "23") or not descval:
                return text + tail
            return ((value + " ") if descval == "1" else "") + text + ((" " + value) if descval == "2" else "") + tail
        if func == "20" and a is not None:
            return fill(text, [span(-a, -(b if b is not None else a), True)]) + tail
        return fill(text, [value if "%+d" in text else plain]) + tail

    # Stats the game shows with text that is not in a string table; properties.txt names the wording.
    TOOLTIP_STATS = {"item_noconsume"}

    def tooltip_of(self, stat_name):
        for prop in self.props.values():
            for i in range(1, 8):
                if (prop.get("stat%d" % i) or "").strip() == stat_name and (prop.get("*Tooltip") or "").strip():
                    return prop["*Tooltip"].strip()
        return None

    def priority(self, name):
        if name == "<randclass>":
            return 158          # with the other skill level bonuses
        if name == "<ethereal>":
            return 10000            # shown with the base item information, above the properties
        value = number((self.stats.get(name) or {}).get("descpriority", ""))
        return value if value is not None else 0

    DAMAGE_KINDS = (("firemindam", "firemaxdam", None, "strModFireDamageRange"),
                    ("lightmindam", "lightmaxdam", None, "strModLightningDamageRange"),
                    ("coldmindam", "coldmaxdam", "coldlength", "strModColdDamageRange"),
                    ("poisonmindam", "poisonmaxdam", "poisonlength", "strModPoisonDamageRange"),
                    ("bleedmindam", "bleedmaxdam", "bleedlength", "strModBleedDamageRange"),
                    ("magicmindam", "magicmaxdam", None, "strModMagicDamageRange"),
                    ("mindamage", "maxdamage", None, "strModMinDamageRange"))

    def damage(self, by_name, used):
        """Min, max and length stats of one damage kind read as one 'Adds a-b ... Damage' line."""
        out = []
        for lo_name, hi_name, len_name, key in self.DAMAGE_KINDS:
            lo, hi = by_name.get(lo_name), by_name.get(hi_name)
            if not lo or not hi or lo.paired is False:
                continue
            length = by_name.get(len_name) if len_name else None
            if len_name == "bleedlength" and length and number(length.lo) == 25:
                key = "bleedoveronesecond"            # the mod has its own wording for a one second bleed
            text = self.strings.get(key)
            if text is None:
                self.problems.add("no string '%s'" % key)
                continue
            used.update({lo_name, hi_name} | ({len_name} if length else set()))
            a, b = lo.lo, hi.hi
            if length:
                frames = number(length.lo) or 0
                if lo_name.startswith("poison"):
                    a = int((number(lo.lo) or 0) * frames / 256)
                    b = int((number(hi.hi) or 0) * frames / 256)
                args = [a, b] if key == "bleedoveronesecond" else [a, b, fmt(frames / 25)]
                out.append((self.priority(lo_name), min(lo.order, hi.order), fill(text, args)))
            else:
                out.append((self.priority(lo_name), min(lo.order, hi.order), fill(text, [a, b])))
        return out

    def item_lines(self, props):
        """props: [(code, par, min, max)] of one item, in row order. Returns the card's lines, or None
        (with self.problems filled) when a property cannot be written."""
        stats = []
        for code, par, lo, hi in props:
            got = self.stats_of(code, par, lo, hi)
            if got is None:
                self.problems.add("no property " + code)
                return None
            for s in got:
                s.order = len(stats)
                s.paired = True
                stats.append(s)
        # Equal stats add up (the game sums them); skill and event stats stay apart by param.
        merged = {}
        for s in stats:
            key = (s.name, s.param)
            if key in merged:
                m = merged[key]
                x, y = number(m.lo), number(s.lo)
                m.lo = fmt((x or 0) + (y or 0)) if x is not None or y is not None else m.lo
                x, y = number(m.hi), number(s.hi)
                m.hi = fmt((x or 0) + (y or 0)) if x is not None or y is not None else m.hi
            else:
                merged[key] = s
        stats = list(merged.values())
        by_name = {}
        for s in stats:
            by_name.setdefault(s.name, s)
        used = set()
        done = []
        for group, members in self.groups.items():
            present = [by_name.get(m) for m in members]
            if len(members) > 1 and all(present) and len({(p.lo, p.hi, p.param) for p in present}) == 1:
                row = self.stats[members[0]]
                text = self.strings.get((row.get("dgrpstrpos") or "").strip())
                if text is None:
                    self.problems.add("no group string for group " + group)
                    return None
                first = present[0]
                value = span(first.lo, first.hi, True)
                plain = span(first.lo, first.hi, False)
                str2 = (row.get("dgrpstr2") or "").strip()
                tail = (" " + self.strings.get(str2)) if str2 and self.strings.get(str2) else ""
                done.append((self.priority(members[0]), min(p.order for p in present),
                             fill(text, [value if "%+d" in text else plain]) + tail))
                used.update(members)
        done += self.damage({k: v for k, v in by_name.items() if k not in used}, used)
        for s in stats:
            if s.name in used:
                continue
            got = self.one(s)
            if got is False:
                return None
            if got is None:
                continue
            done.append((self.priority(s.name), s.order, got))
        done.sort(key=lambda x: (-x[0], x[1]))
        return [text for _, _, text in done]
