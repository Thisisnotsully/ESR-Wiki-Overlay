#!/usr/bin/env python3
"""Convert the D2R .sprite images the wiki references into PNG files.

Usage (from esr-wiki-plugin/overlay):
    python3 tools/sprites_to_png.py --source "<D2R install>/data/data" [--source <more roots>]

The D2R .sprite format (see scripts/REFERENCE.md in the plugin): a 40-byte header
('SpA1' or 'SPa1' magic, uint16 width at byte 6, uint16 height at byte 12), then
width*height RGBA8 pixels with no mip chain.

Each sprite is looked up case-insensitively under every --source root using the
relative path from data/image-manifest.txt. The project's own portraits
(d2rloader/esr-wiki/wiki/*.sprite) are read from data/sprites/wiki/ automatically.
Icons are cropped to their visible pixels so they scale predictably in the overlay.
Output: data/images/<key>.png, plus data/image-report.txt listing anything missing.
"""
import argparse
import os
import struct
import sys
from pathlib import Path

from PIL import Image

OVERLAY_DIR = Path(__file__).resolve().parent.parent
PLUGIN_DIR = OVERLAY_DIR.parent
HEADER_BYTES = 40
PROJECT_SPRITE_PREFIX = "d2rloader/esr-wiki/wiki/"
PROJECT_SPRITE_DIR = PLUGIN_DIR / "data" / "sprites" / "wiki"


def read_sprite(path):
    """Return the RGBA image for a .sprite file.

    The header size is the image size. Some sprites (e.g. Chi_rune) carry extra
    trailing bytes after the pixel rows; those are padding and are ignored.
    """
    data = Path(path).read_bytes()
    magic = data[0:4]
    if magic not in (b"SpA1", b"SPa1"):
        raise ValueError(f"unexpected magic {magic!r}")
    _version, width, _dup_width, _unused, height = struct.unpack_from("<HHHHH", data, 4)
    expected = width * height * 4
    payload = len(data) - HEADER_BYTES
    if payload < expected:
        raise ValueError(f"short pixel data: {width}x{height} needs {expected} bytes, file has {payload}")
    return Image.frombytes("RGBA", (width, height), data[HEADER_BYTES:HEADER_BYTES + expected])


def index_roots(roots):
    """Map lower-cased relative sprite path -> absolute path, first root wins."""
    index = {}
    for root in roots:
        for dirpath, _dirs, files in os.walk(root):
            for name in files:
                if not name.lower().endswith(".sprite"):
                    continue
                full = Path(dirpath) / name
                rel = full.relative_to(root).as_posix().lower()
                index.setdefault(rel, full)
    return index


def load_manifest():
    entries = []
    for line in (OVERLAY_DIR / "data" / "image-manifest.txt").read_text(encoding="utf-8").splitlines():
        if line.strip():
            key, rel = line.split("\t")
            entries.append((key, rel))
    return entries


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", action="append", default=[],
                        help="folder that contains the D2R 'hd/global/ui' tree, or the 'data' folder above it")
    args = parser.parse_args()

    roots = [Path(p) for p in args.source if Path(p).is_dir()]
    if not roots:
        print("warning: no --source folder found; only project portraits will be converted", file=sys.stderr)
    # Direct path checks are fast on mounted folders; the case-insensitive index is
    # only built if a file is not found under its exact manifest spelling.
    index_cache = {}

    def fallback_index():
        if "all" not in index_cache:
            index_cache["all"] = index_roots(roots)
        return index_cache["all"]

    project_index_cache = {}

    def project_fallback():
        if "all" not in project_index_cache:
            project_index_cache["all"] = index_roots([PROJECT_SPRITE_DIR]) if PROJECT_SPRITE_DIR.is_dir() else {}
        return project_index_cache["all"]

    out_dir = OVERLAY_DIR / "data" / "images"
    out_dir.mkdir(parents=True, exist_ok=True)
    converted, missing, failed = 0, [], []
    for key, rel in load_manifest():
        lower = rel.lower()
        source = None
        if lower.startswith(PROJECT_SPRITE_PREFIX):
            leaf = lower[len(PROJECT_SPRITE_PREFIX):]
            direct = PROJECT_SPRITE_DIR / leaf
            source = direct if direct.is_file() else project_fallback().get(leaf)
        if source is None:
            for root in roots:
                for candidate in (root / rel, root / "hd" / "global" / "ui" / rel):
                    if candidate.is_file():
                        source = candidate
                        break
                if source is not None:
                    break
        if source is None:
            index = fallback_index()
            source = index.get(lower) or index.get("hd/global/ui/" + lower)
        if source is None:
            missing.append(f"{key}\t{rel}")
            continue
        try:
            image = read_sprite(source)
            bbox = image.getchannel("A").getbbox()
            if bbox:
                image = image.crop(bbox)
            image.save(out_dir / f"{key}.png", optimize=True)
            converted += 1
        except (ValueError, OSError, struct.error) as error:
            failed.append(f"{key}\t{error}")

    report = [f"converted\t{converted}", f"missing\t{len(missing)}", f"failed\t{len(failed)}", ""]
    report += ["# missing (key, D2R relative path)"] + missing + ["", "# failed (key, reason)"] + failed
    (OVERLAY_DIR / "data" / "image-report.txt").write_text("\n".join(report) + "\n", encoding="utf-8", newline="\n")
    print(f"converted={converted} missing={len(missing)} failed={len(failed)}")


if __name__ == "__main__":
    main()
