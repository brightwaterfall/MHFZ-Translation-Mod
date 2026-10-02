#!/usr/bin/env python3
"""Generate data/skills-en.json: English skill tree / active skill names.

Mogapedia's translations-en.json has no rows for pac/skills/name and
pac/skills/effect, so the status screen stayed untranslated. The ID-indexed
lists in ezemania2/mhf-file-editor (src/utils/skills.rs, automatic_skills.rs)
follow the same order as those pac tables; they are fetched into
data/upstream/ and converted here.

    gh api repos/ezemania2/mhf-file-editor/contents/src/utils/skills.rs \
        -H "Accept: application/vnd.github.raw" > data/upstream/skills.rs
    (same for automatic_skills.rs)
    python scripts/import_skill_names.py
"""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UP = ROOT / "data" / "upstream"

# Entries the upstream lists leave blank, untranslated, or label "(Dupe)".
OVERRIDES = {
    "pac/skills/name": {
        19: "Wide-Area Heal",
        20: "Wide-Area Antidote",
        33: "Seed Wide-Area",
        48: "Meat",
        119: "Power Sword",
        168: "Counter",
        177: "Combo (Removed)",
        193: "Gathering Mastery",
    },
    "pac/skills/effect": {
        238: "Poison Coating Add",
        239: "Para Coating Add",
        240: "Sleep Coating Add",
        372: "SnS Tech: Gr.Sword Saint",
        373: "DS Tech: Gr.Dual Dragon",
        374: "GS Tech: Gr.Sword King",
        375: "LS Tech: Gr.Katana God",
        376: "Hammer Tech: Gr.B.Beast",
        377: "HH Tech: Gr.F.Emperor",
        378: "Lance Tech: Gr.H.Spear",
        379: "GL Tech: Gr.Cannon Ruler",
        380: "HBG Tech: Gr.Gun Sage",
        381: "LBG Tech: Gr.Gun Prodigy",
        382: "Bow Tech: Gr.Bow Demon",
        427: "Counter+1",
        428: "Counter+2",
        470: "Tonfa Tech: Gr.P.Phoenix",
        479: "Gathering Mastery",
        534: "Magspike Tech: Gr.M.Star",
    },
}


def is_english(text: str) -> bool:
    return bool(text) and text != "Unknown" and text.isascii()


def main() -> int:
    names_src = (UP / "skills.rs").read_text(encoding="utf-8-sig")
    auto_src = (UP / "automatic_skills.rs").read_text(encoding="utf-8-sig")
    sections = {
        "pac/skills/name": {int(m[1], 16): m[2] for m in re.finditer(r'0x([0-9A-Fa-f]+)\s*=>\s*"([^"]*)"', names_src)},
        "pac/skills/effect": {int(m[1]): m[2] for m in re.finditer(r'\((\d+),\s*"([^"]*)"\)', auto_src)},
    }
    out = {}
    for xpath, rows in sections.items():
        rows = {i: t for i, t in rows.items() if is_english(t)}
        rows.update(OVERRIDES.get(xpath, {}))
        out[xpath] = {str(i): rows[i] for i in sorted(rows)}
        print(f"{xpath}: {len(rows)} names")
    (ROOT / "data" / "skills-en.json").write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
