#!/usr/bin/env python3
"""Build content-verified TW patch pairs: exact TW source bytes -> EN bytes.

The DLL (ContentPatcher) never trusts header offsets at runtime. It finds the
decompressed bins in memory by matching these source strings at their known
file offsets, re-checks every string's bytes before touching it, then writes
in place when the English fits or redirects only the pointers that reference
that verified string.

Only sections whose TW order was checked against JP are emitted (see
VERIFIED). TW mixes encodings: mhfdat/mhfpac are Big5 (cp950) while
mhfgao/mhfjmp/mhfrcc store Chinese as CP932 kanji, so targets are encoded with
each file's own codec.

Usage:
  python scripts/build_tw_pairs.py
"""
from __future__ import annotations

import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party" / "FrontierTextHandler"))

from src.common import read_extraction_config  # type: ignore  # noqa: E402
from src.pointer_tables import extract_text_data_from_bytes  # type: ignore  # noqa: E402

DEC = ROOT / "client" / "tw_decompressed"
FILES = {
    "dat": ("mhfdat.bin", "cp950"),
    "pac": ("mhfpac.bin", "cp950"),
    "gao": ("mhfgao.bin", "cp932"),
    "jmp": ("mhfjmp.bin", "cp932"),
    "rcc": ("mhfrcc.bin", "cp932"),
}

# xpath -> how TW order was verified against JP (index i == JP index i).
VERIFIED = {
    "dat/armors/head": "Big5 spot checks at 0/1/3/100/1000/5000/14000 match JP meaning",
    "dat/armors/body": "same table family/count as head (13462 == JP)",
    "dat/armors/arms": "same table family/count as head (13452 == JP)",
    "dat/armors/waist": "same table family as head",
    "dat/armors/legs": "same table family as head",
    "gao/armor_desc": "byte-identical to JP source 514/514",
    "gao/weapon_desc": "colour/{j} structure 1.00 at k=0",
    "gao/weapon_names": "meaning matches at k=0 (無裝備/骨製猫尖刃刀/肉球猫拳)",
    "gao/skill_names_zenith": "meaning matches at k=0",
    "gao/skill_text": "meaning matches at k=0",
    "jmp/menu/title": "meaning matches at k=0",
    "jmp/menu/description": "same table as title",
    "jmp/strings": "meaning matches at k=0",
    "rcc/events_en": "codes match at k=0",
    "pac/text_18": "structure 1.00 at k=0 (丟棄/交付/交換/Ｆ鍵登錄)",
    "pac/text_60": "meaning matches at k=0",
    "pac/text_68": "meaning matches at k=0",
    "pac/text_6c": "meaning matches at k=0 for shared prefix",
}

COLOR_RE = re.compile(r"\{/c\}|\{c(\d\d)\}")
FALLBACK = {
    "\u3000": " ", "・": ".", "…": "...", "～": "~", "〜": "~", "－": "-",
    "“": '"', "”": '"', "‘": "'", "’": "'", "—": "-", "–": "-",
}


def u32(data: bytes, off: int) -> int:
    return int.from_bytes(data[off:off + 4], "little")


def cstr(data: bytes, off: int) -> bytes:
    end = data.index(b"\0", off)
    return data[off:end]


def encode_target(text: str, codec: str) -> bytes:
    text = COLOR_RE.sub(lambda m: "~C00" if m.group(0) == "{/c}" else "~C" + m.group(1), text)
    out = bytearray()
    for ch in text:
        try:
            out += ch.encode(codec)
        except UnicodeEncodeError:
            alt = FALLBACK.get(ch, "?")
            out += alt.encode(codec, errors="replace")
    return bytes(out)


def load_en(path: Path) -> dict[str, dict[int, str]]:
    doc = json.loads(path.read_text(encoding="utf-8"))
    root = doc.get("en", doc)
    out: dict[str, dict[int, str]] = {}
    for xpath, rows in root.items():
        if not isinstance(rows, list):
            continue
        out[xpath] = {int(r["index"]): r.get("target") or "" for r in rows if "index" in r}
    return out


def main() -> int:
    headers = str(ROOT / "data" / "headers.tw.json")
    en = load_en(ROOT / "data" / "translations-en.json")
    bins = {k: (DEC / f).read_bytes() for k, (f, _) in FILES.items()}

    merged: dict[tuple[str, bytes], dict] = {}
    report: dict[str, dict] = {}
    samples: list[str] = []
    conflicts = 0

    for xpath, why in VERIFIED.items():
        key = xpath.split("/")[0]
        data = bins[key]
        codec = FILES[key][1]
        stats = defaultdict(int)
        try:
            ents = extract_text_data_from_bytes(data, read_extraction_config(xpath, headers), "tw")
        except Exception as ex:
            report[xpath] = {"error": f"{type(ex).__name__}: {ex}"}
            continue
        targets = en.get(xpath, {})
        stats["tw_entries"] = len(ents)
        for idx, ent in enumerate(ents):
            tgt = targets.get(idx, "")
            if not tgt:
                stats["no_target"] += 1
                continue
            slots = ent.get("sub_offsets") or [ent["offset"]]
            parts = tgt.split("{j}")
            if len(parts) != len(slots):
                stats["part_count_mismatch"] += 1
                continue
            for slot, part in zip(slots, parts):
                str_off = u32(data, slot)
                src = cstr(data, str_off)
                if not src or max(src) < 0x80:
                    stats["ascii_or_empty_source"] += 1
                    continue
                tbytes = encode_target(part, codec)
                if not tbytes or tbytes == src:
                    stats["empty_or_same_target"] += 1
                    continue
                rec = merged.get((key, src))
                if rec is None:
                    rec = {"k": key, "x": xpath, "s": src, "t": tbytes, "o": set(), "p": set()}
                    merged[(key, src)] = rec
                elif rec["t"] != tbytes:
                    conflicts += 1
                    stats["conflict_kept_first"] += 1
                rec["o"].add(str_off)
                rec["p"].add(slot)
                stats["parts"] += 1
            if idx in (0, 1, 3, len(ents) // 2, len(ents) - 1):
                tw_txt = " | ".join(cstr(data, u32(data, s)).decode(codec, "replace") for s in slots)
                samples.append(f"{xpath}[{idx}] TW: {tw_txt[:70]}  ->  EN: {tgt[:70]}")
        stats["why"] = why
        report[xpath] = dict(stats)

    pairs = []
    for rec in merged.values():
        pairs.append({
            "k": rec["k"], "x": rec["x"], "s": rec["s"].hex(), "t": rec["t"].hex(),
            "o": sorted(rec["o"]), "p": sorted(rec["p"]),
        })
    pairs.sort(key=lambda r: (r["k"], r["o"][0]))
    out = {
        "version": 1,
        "profile": "tw",
        "files": {k: len(v) for k, v in bins.items()},
        "pairs": pairs,
    }
    (ROOT / "data" / "pairs-tw.json").write_text(json.dumps(out, separators=(",", ":")), encoding="utf-8")
    (ROOT / "data" / "pairs-tw-report.json").write_text(
        json.dumps({"conflicts": conflicts, "pairs": len(pairs), "sections": report},
                   ensure_ascii=False, indent=2), encoding="utf-8")
    (ROOT / "data" / "pairs-tw-samples.txt").write_text("\n".join(samples), encoding="utf-8")
    fits = sum(1 for p in pairs if len(p["t"]) <= len(p["s"]))
    print(f"pairs={len(pairs)} fit_in_place={fits} need_redirect={len(pairs) - fits} conflicts={conflicts}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
