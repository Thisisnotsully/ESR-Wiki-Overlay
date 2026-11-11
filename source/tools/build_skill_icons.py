#!/usr/bin/env python3
"""Build the Skills-tab icons from the ESR mod's own D2R art.

Usage (from esr-wiki-plugin/overlay):
    python3 tools/build_skill_icons.py --mod "<...>/Eastern_Sun_Resurrected.mpq"
    python3 tools/build_skill_icons.py --mod "<...>/Eastern_Sun_Resurrected.mpq" --check
    python3 tools/build_skill_icons.py --check          (no mod: only checks the saved files agree with each other)

Writes (all deterministic; running it twice gives byte-identical files):
    data/images/skill_<classcode>_<cel>.png   one icon per (class sheet, IconCel), e.g. skill_ama_12.png
    data/skill-icons.txt                      <skill key><TAB><image name without .png>, sorted by key

Where the art comes from:
  * data/skills-data.json  -> the 473 skills; `k` is the stable lower-case English skill name.
  * <mod>/data/global/excel/skills.txt      -> `skill` (matches `k`), `charclass`, `skilldesc`
  * <mod>/data/global/excel/skilldesc.txt   -> `IconCel` for that skilldesc
  * <mod>/data/hd/global/ui/spells/<class>/<xx>skillicon.sprite -> the icon sheet for the class.
    Class skills (charclass ama/sor/nec/pal/bar/dru/ass/war) use their own class sheet; skills with no
    class (item skills, charges, monster-style "other") use submenu/skillicon.sprite.
    (skillicon2/3 are other states of the tree buttons and are not used.)

Sprite sheet layout (D2R 'SpA1'/'SPa1', 40-byte header, then RGBA8 pixels): u16 version at 4, u16 cell width
at 6, u32 sheet width at 8, u32 height at 12, u32 frame count at 20. The sheet is one horizontal strip of
frames; frame N is columns [N*cellW, (N+1)*cellW). IconCel is the frame index (the odd frame after each even
one is the pressed state and is not used).

Each frame is downscaled (Lanczos) so its longest side is at most 96 px, keeping the cell's own padding so every
icon sits in the same place. A skill with no IconCel, or a frame that is blank, gets no entry.
--check regenerates everything in memory and fails (exit 1) if any file differs or is missing/extra.
"""
import argparse
import csv
import io
import json
import os
import struct
import sys
from pathlib import Path

from PIL import Image

OVERLAY_DIR = Path(__file__).resolve().parent.parent
IMAGES = OVERLAY_DIR / "data" / "images"
MAP_FILE = OVERLAY_DIR / "data" / "skill-icons.txt"
SKILLS_JSON = OVERLAY_DIR / "data" / "skills-data.json"
MAX_SIZE = 96
SHEETS = {
    "ama": "amazon/amskillicon.sprite",
    "sor": "sorceress/soskillicon.sprite",
    "nec": "necromancer/neskillicon.sprite",
    "pal": "paladin/paskillicon.sprite",
    "bar": "barbarian/baskillicon.sprite",
    "dru": "druid/drskillicon.sprite",
    "ass": "assassin/asskillicon.sprite",
    "war": "warlock/waskillicon.sprite",
    "other": "submenu/skillicon.sprite",
}
SHEET_DIR = "data/hd/global/ui/spells/"


def read_sheet(path):
    data = Path(path).read_bytes()
    if data[:4] not in (b"SpA1", b"SPa1"):
        raise ValueError(f"{path}: unexpected magic {data[:4]!r}")
    cell_w = struct.unpack_from("<H", data, 6)[0]
    width, height, _zero, frames = struct.unpack_from("<IIII", data, 8)
    need = width * height * 4
    if len(data) - 40 < need or cell_w * frames != width:
        raise ValueError(f"{path}: bad layout cell={cell_w} width={width} height={height} frames={frames}")
    return Image.frombytes("RGBA", (width, height), data[40:40 + need]), cell_w, frames


def read_table(path):
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def png_bytes(img):
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def build(mod):
    excel = Path(mod) / "data" / "global" / "excel"
    desc = {r["skilldesc"]: r for r in read_table(excel / "skilldesc.txt")}
    rows = {}
    for r in read_table(excel / "skills.txt"):
        rows.setdefault(r["skill"].lower(), r)
    skills = json.loads(SKILLS_JSON.read_text(encoding="utf-8"))["skills"]
    sheets = {}
    images, mapping, missing = {}, {}, []
    for s in skills:
        key = s["k"]
        r = rows.get(key)
        cls = (r["charclass"].strip() if r else "") or "other"
        d = desc.get(r["skilldesc"]) if r else None
        cel_text = (d["IconCel"].strip() if d else "")
        if cls not in SHEETS or not cel_text.lstrip("-").isdigit() or int(cel_text) < 0:
            missing.append((key, f"no IconCel (class={cls})"))
            continue
        cel = int(cel_text)
        if cls not in sheets:
            sheets[cls] = read_sheet(Path(mod) / SHEET_DIR / SHEETS[cls])
        sheet, cell_w, frames = sheets[cls]
        if cel >= frames:
            missing.append((key, f"IconCel {cel} beyond sheet ({frames} frames)"))
            continue
        name = f"skill_{cls}_{cel}"
        if name not in images:
            cell = sheet.crop((cel * cell_w, 0, (cel + 1) * cell_w, sheet.height))
            if cell.getchannel("A").getbbox() is None:
                missing.append((key, f"frame {cel} is blank"))
                continue
            scale = min(1.0, MAX_SIZE / max(cell.size))
            if scale < 1.0:
                cell = cell.resize((max(1, round(cell.width * scale)), max(1, round(cell.height * scale))), Image.LANCZOS)
            images[name] = png_bytes(cell)
        mapping[key] = name
    text = "".join(f"{k}\t{mapping[k]}\n" for k in sorted(mapping))
    return images, text, missing, skills


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--mod", default=os.environ.get("ESR_MOD"), help="the Eastern_Sun_Resurrected.mpq folder (or env ESR_MOD)")
    p.add_argument("--check", action="store_true", help="compare instead of writing")
    a = p.parse_args()

    if not a.mod:
        if not a.check:
            p.error("--mod is required unless --check is used alone")
        names = [l.split("\t")[1] for l in MAP_FILE.read_text(encoding="utf-8").splitlines()]
        bad = [n for n in sorted(set(names)) if not (IMAGES / f"{n}.png").is_file()]
        stray = sorted(f.stem for f in IMAGES.glob("skill_*.png") if f.stem not in set(names))
        print(f"{len(names)} mappings, {len(set(names))} images, missing={len(bad)} stray={len(stray)}")
        sys.exit(1 if bad or stray else 0)

    images, text, missing, skills = build(a.mod)
    if a.check:
        problems = []
        if not MAP_FILE.is_file() or MAP_FILE.read_text(encoding="utf-8") != text:
            problems.append("skill-icons.txt differs")
        for n, b in images.items():
            f = IMAGES / f"{n}.png"
            if not f.is_file() or f.read_bytes() != b:
                problems.append(f"{n}.png differs or is missing")
        for f in IMAGES.glob("skill_*.png"):
            if f.stem not in images:
                problems.append(f"{f.name} is stray")
        print("\n".join(problems) or "up to date")
        sys.exit(1 if problems else 0)

    IMAGES.mkdir(parents=True, exist_ok=True)
    for n, b in images.items():
        (IMAGES / f"{n}.png").write_bytes(b)
    with open(MAP_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"{len(images)} images, {text.count(chr(10))}/{len(skills)} skills mapped, {len(missing)} without an icon")
    for k, why in missing:
        print(f"  no icon: {k} ({why})")


if __name__ == "__main__":
    main()
