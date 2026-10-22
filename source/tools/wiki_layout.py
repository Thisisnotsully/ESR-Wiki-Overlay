"""Readers for the ESR wiki's D2R layout JSON and string table.

The shipped layout files are not strict JSON (trailing commas), so the loader
strips them before parsing. Everything here is read-only.
"""
import json
import re
from pathlib import Path

_TRAILING_COMMA = re.compile(r',(\s*[\]}])')


def load_layout(path):
    text = Path(path).read_text(encoding="utf-8")
    return json.loads(_TRAILING_COMMA.sub(r"\1", text))


def walk(node, depth=0):
    """Yield (node, depth) in document order."""
    yield node, depth
    for child in node.get("children") or []:
        yield from walk(child, depth + 1)


def fields(node):
    return node.get("fields") or {}


def load_string_table(path):
    """Return {Key: enUS text} from esr-wiki-strings.json."""
    rows = json.loads(Path(path).read_text(encoding="utf-8"))
    return {row["Key"]: row.get("enUS", "") for row in rows}


TOOLTIP_REF = re.compile(r"^@esr-wiki:(.+)$")


def tooltip_key(node):
    """Return the string-table key a node's tooltip points at, or None."""
    ref = fields(node).get("tooltipString")
    if not isinstance(ref, str):
        return None
    match = TOOLTIP_REF.match(ref)
    return match.group(1) if match else None
