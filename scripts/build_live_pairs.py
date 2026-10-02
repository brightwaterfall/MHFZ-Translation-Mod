#!/usr/bin/env python3
"""Build content-verified patch pairs from the bins the game really loads.

The 皓月 (axibug) launcher HY5.exe is an Enigma Virtual Box container: its
embedded dat\\mhf*.bin override the files on disk, so the client's own
dat\\mhfdat.bin / mhfpac.bin (2014, Big5) are never used. Unpack it first:

    python -m evbunpack client/hy5/HY5.exe client/hy5_unpacked

then decompress those bins to client/live_decompressed (see main()).

The embedded files are the JP ZZ layout with Chinese text stored as CP932
kanji, so data/headers.json (JP offsets) and translations-en.json (JP
indices) line up 1:1. Two item tables were relocated by the server team and
are read from explicit offsets instead.

Usage:
  python scripts/build_live_pairs.py
"""
from __future__ import annotations

import bisect
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party" / "FrontierTextHandler"))

from src.common import load_file_data, read_extraction_config  # type: ignore  # noqa: E402
from src.pointer_tables import extract_text_data_from_bytes  # type: ignore  # noqa: E402

PACKED = ROOT / "client" / "hy5_unpacked" / "dat"
LIVE = ROOT / "client" / "live_decompressed"
CODEC = "cp932"
# mhfinf.bin is left out: only 5 translated strings, and a file that never
# shows up in memory keeps the DLL in its 15 s full-search cycle.
KEYS = ("dat", "pac", "gao", "jmp", "rcc")

# Tables the server team moved; header pointers no longer lead to them.
# xpath -> list of (pointer table offset, entry count); every copy is patched.
RELOCATED = {
    "dat/items/name": [(0x193E16B, 16700)],
    "dat/items/description": [(0xB8BBA0, 16701), (0x1982DC9, 16699)],
}

# Files scanned for header-less pointer arrays (menus live in mhfpac.bin).
DISCOVER_KEYS = ("pac",)
DISCOVER_MIN_RUN = 4
DISCOVER_MAX_GAP = 4

# translations-en.json groups these tables into a few {j}-joined rows; the live
# table has one string per entry, in the same order as the joined parts.
FLATTEN = ("pac/skills/effect_z",)

# translations-en.json rows for these xpaths start with N unrelated UI strings.
INDEX_SHIFT = {"dat/items/description": 24}

COLOR_RE = re.compile(r"\{/c\}|\{c(\d\d)\}")
FALLBACK = {
    "\u3000": " ", "…": "...", "～": "~", "〜": "~",
    "“": '"', "”": '"', "‘": "'", "’": "'", "—": "-", "–": "-",
}


def u32(data: bytes, off: int) -> int:
    return int.from_bytes(data[off:off + 4], "little")


def cstr(data: bytes, off: int) -> bytes:
    return data[off:data.index(b"\0", off)]


def encode_target(text: str) -> bytes:
    text = COLOR_RE.sub(lambda m: "~C00" if m.group(0) == "{/c}" else "~C" + m.group(1), text)
    out = bytearray()
    for ch in text:
        try:
            out += ch.encode(CODEC)
        except UnicodeEncodeError:
            out += FALLBACK.get(ch, "?").encode(CODEC, errors="replace")
    return bytes(out)


def load_en() -> dict[str, dict[int, str]]:
    doc = json.loads((ROOT / "data" / "translations-en.json").read_text(encoding="utf-8"))
    out: dict[str, dict[int, str]] = {}
    for xpath, rows in doc.get("en", doc).items():
        if isinstance(rows, list):
            out[xpath] = {int(r["index"]): r.get("target") or "" for r in rows if "index" in r}
    for xpath in FLATTEN:
        rows = out.get(xpath, {})
        parts = [p for i in sorted(rows) for p in rows[i].split("{j}")]
        out[xpath] = dict(enumerate(parts))
    # Skill tables Mogapedia has not translated yet (scripts/import_skill_names.py).
    extra = json.loads((ROOT / "data" / "skills-en.json").read_text(encoding="utf-8"))
    for xpath, rows in extra.items():
        sec = out.setdefault(xpath, {})
        for idx, text in rows.items():
            if not sec.get(int(idx)):
                sec[int(idx)] = text
    return out


def decompress_live() -> dict[str, bytes]:
    LIVE.mkdir(parents=True, exist_ok=True)
    bins = {}
    for key in KEYS:
        packed = PACKED / f"mhf{key}.bin"
        out = LIVE / f"mhf{key}.bin"
        if not out.exists():
            out.write_bytes(load_file_data(str(packed)))
        bins[key] = out.read_bytes()
    return bins


def entries_for(xpath: str, data: bytes) -> list[list[int]]:
    """Per entry index, the pointer slots of its {j}-separated parts."""
    if xpath in RELOCATED:
        tables = RELOCATED[xpath]
        count = min(n for _, n in tables)
        return [[base + 4 * i for base, _ in tables] for i in range(count)]
    ents = extract_text_data_from_bytes(data, read_extraction_config(xpath, str(ROOT / "data" / "headers.json")), "zz")
    return [e.get("sub_offsets") or [e["offset"]] for e in ents]


def header_xpaths(node, prefix=""):
    if isinstance(node, dict):
        if "begin_pointer" in node:
            yield prefix
            return
        for k, v in node.items():
            yield from header_xpaths(v, f"{prefix}/{k}" if prefix else k)


def referenced_starts(bins: dict[str, bytes]) -> dict[str, set[int]]:
    """Every string offset any known table points at, per file."""
    headers = json.loads((ROOT / "data" / "headers.json").read_text(encoding="utf-8"))
    out: dict[str, set[int]] = defaultdict(set)
    for xpath in header_xpaths(headers):
        key = xpath.split("/")[0]
        if key not in bins:
            continue
        data = bins[key]
        try:
            ents = entries_for(xpath, data)
        except Exception:
            continue
        for slots in ents:
            for s in slots:
                v = u32(data, s)
                if 0 < v < len(data):
                    out[key].add(v)
    return out


def is_string_start(data: bytes, off: int) -> bool:
    if not 0 < off < len(data) or data[off - 1] != 0 or data[off] < 0x20:
        return False
    end = data.find(b"\0", off)
    if end < 0 or end - off > 512:
        return False
    try:
        data[off:end].decode(CODEC)
    except UnicodeDecodeError:
        return False
    return True


def discover_pointer_arrays(data: bytes) -> dict[int, list[int]]:
    """Aligned u32 arrays of string offsets that no header describes.

    UI menus reuse the same strings through many such arrays; the DLL only
    redirects slots recorded here, so every array a menu reads must be listed.
    A slot counts when it sits in a run (zero entries allowed) holding at
    least MIN_RUN distinct valid string offsets.
    Returns string offset -> slots.
    """
    out: dict[int, list[int]] = defaultdict(list)
    run: list[tuple[int, int]] = []
    zeros = 0

    def flush():
        if len({v for _, v in run}) >= DISCOVER_MIN_RUN:
            for s, v in run:
                out[v].append(s)
        run.clear()

    for slot in range(0, len(data) - 3, 4):
        v = u32(data, slot)
        if v == 0 and run and zeros < DISCOVER_MAX_GAP:
            zeros += 1
            continue
        if v and is_string_start(data, v):
            run.append((slot, v))
            zeros = 0
        else:
            flush()
            zeros = 0
    flush()
    return out


def load_menu_en() -> dict[str, dict[bytes, str]]:
    doc = json.loads((ROOT / "data" / "menu-en.json").read_text(encoding="utf-8"))
    return {key: {src.encode(CODEC): tgt for src, tgt in rows.items()} for key, rows in doc.items()}


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    bins = decompress_live()
    en = load_en()
    starts = referenced_starts(bins)

    merged: dict[tuple[str, bytes], dict] = {}
    report: dict[str, dict] = {}
    samples: list[str] = []
    conflicts = 0

    for xpath, targets in sorted(en.items()):
        key = xpath.split("/")[0]
        if key not in bins:
            report[xpath] = {"error": "no live file"}
            continue
        data = bins[key]
        stats = defaultdict(int)
        try:
            ents = entries_for(xpath, data)
        except Exception as ex:
            report[xpath] = {"error": f"{type(ex).__name__}: {ex}"}
            continue
        stats["live_entries"] = len(ents)
        copies = len(RELOCATED[xpath]) if xpath in RELOCATED else 1
        shift = INDEX_SHIFT.get(xpath, 0)
        for idx, slots in enumerate(ents):
            tgt = targets.get(idx + shift, "")
            if not tgt:
                stats["no_target"] += 1
                continue
            parts = tgt.split("{j}")
            # Relocated tables list one slot per copy, all holding the same text.
            part_slots = [[s] for s in slots] if copies == 1 else [slots]
            if len(parts) != len(part_slots):
                stats["part_count_mismatch"] += 1
                continue
            for group, part in zip(part_slots, parts):
                tbytes = encode_target(part)
                for slot in group:
                    str_off = u32(data, slot)
                    if not 0 < str_off < len(data):
                        stats["bad_pointer"] += 1
                        continue
                    src = cstr(data, str_off)
                    if not src or max(src) < 0x80:
                        stats["ascii_or_empty_source"] += 1
                        continue
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
            if idx in (1, 2, len(ents) // 3, len(ents) // 2, len(ents) - 1):
                cn = " | ".join(cstr(data, u32(data, s)).decode(CODEC, "replace") for s in slots[:3])
                samples.append(f"{xpath}[{idx}] CN: {cn[:60]}  ->  EN: {tgt[:60]}")
        report[xpath] = dict(stats)

    # Header-less menu strings, translated by source text (data/menu-en.json).
    menu_en = load_menu_en()
    discovered: dict[str, dict[int, list[int]]] = {}
    for key in DISCOVER_KEYS:
        data = bins[key]
        found = discover_pointer_arrays(data)
        discovered[key] = found
        starts[key] |= set(found)
        stats = defaultdict(int)
        for src, tgt in menu_en.get(key, {}).items():
            if (key, src) in merged:
                stats["already_paired"] += 1
                continue
            offs, i = [], data.find(src + b"\0")
            while i >= 0:
                if i > 0 and data[i - 1] == 0:
                    offs.append(i)
                i = data.find(src + b"\0", i + 1)
            if not offs:
                stats["not_found"] += 1
                continue
            merged[(key, src)] = {"k": key, "x": f"{key}/menu_extra", "s": src, "t": encode_target(tgt),
                                  "o": set(offs), "p": set()}
            stats["parts"] += 1
        added = 0
        for r in merged.values():
            if r["k"] == key:
                for o in r["o"]:
                    new = set(found.get(o, ())) - r["p"]
                    r["p"] |= new
                    added += len(new)
        stats["discovered_slots_added"] = added
        report[f"{key}/menu_extra"] = dict(stats)

    # Tail-merged strings: another table entry points inside this one, so an
    # in-place write would corrupt it. Such pairs may only be redirected.
    sorted_starts = {k: sorted(v | {o for r in merged.values() if r["k"] == k for o in r["o"]}) for k, v in starts.items()}
    shared = 0
    for r in merged.values():
        ss = sorted_starts.get(r["k"], [])
        data = bins[r["k"]]
        for o in r["o"]:
            i = bisect.bisect_right(ss, o)
            # Either another string starts inside this one, or this one is the
            # tail of a longer string (no NUL right before it).
            if (i < len(ss) and ss[i] < o + len(r["s"])) or (o > 0 and data[o - 1] != 0):
                r["r"] = True
                shared += 1
                break

    pairs = []
    for r in merged.values():
        row = {"k": r["k"], "x": r["x"], "s": r["s"].hex(), "t": r["t"].hex(),
               "o": sorted(r["o"]), "p": sorted(r["p"])}
        if r.get("r"):
            row["r"] = 1
        pairs.append(row)
    pairs.sort(key=lambda r: (r["k"], r["o"][0]))
    out = {"version": 2, "profile": "tw", "source": "HY5.exe embedded bins",
           "files": {k: len(v) for k, v in bins.items()}, "pairs": pairs}
    (ROOT / "data" / "pairs-tw.json").write_text(json.dumps(out, separators=(",", ":")), encoding="utf-8")
    (ROOT / "data" / "pairs-tw-report.json").write_text(
        json.dumps({"conflicts": conflicts, "pairs": len(pairs), "sections": report},
                   ensure_ascii=False, indent=2), encoding="utf-8")
    (ROOT / "data" / "pairs-tw-samples.txt").write_text("\n".join(samples), encoding="utf-8")
    per_key = defaultdict(int)
    for p in pairs:
        per_key[p["k"]] += 1
    fits = sum(1 for p in pairs if len(p["t"]) <= len(p["s"]) and not p.get("r"))
    errors = {x: r["error"] for x, r in report.items() if "error" in r}
    print(f"pairs={len(pairs)} {dict(per_key)} fit_in_place={fits} need_redirect={len(pairs) - fits} "
          f"shared_tail={shared} conflicts={conflicts}")
    for x, e in errors.items():
        print(f"  section error {x}: {e}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
