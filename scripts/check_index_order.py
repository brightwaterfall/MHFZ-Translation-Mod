#!/usr/bin/env python3
"""Measure whether TW table order matches JP order, per xpath.

TW bins are Big5 (cp950); FrontierTextHandler decodes them as CP932, so we
re-read raw bytes from the slot offsets and decode as cp950. JP sources come
from translations-full.json. We compare a structural signature (number of
{j} parts and the colour-code sequence) of TW[i] vs JP[i+k] for shifts k.
A clear peak at k=0 means index-based alignment lands on the right slot.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party" / "FrontierTextHandler"))

from src.common import read_extraction_config  # type: ignore  # noqa: E402
from src.pointer_tables import extract_text_data_from_bytes  # type: ignore  # noqa: E402

DEC = ROOT / "client" / "tw_decompressed"
FILE_MAP = {"dat": "mhfdat.bin", "pac": "mhfpac.bin", "inf": "mhfinf.bin",
            "gao": "mhfgao.bin", "jmp": "mhfjmp.bin", "rcc": "mhfrcc.bin"}
GAME_CODE_RE = re.compile(r"~C(\d\d)")
CSV_CODE_RE = re.compile(r"\{c(\d\d)\}|\{/c\}")


def raw_parts(data: bytes, entry: dict) -> list[bytes]:
    out = []
    for slot in entry.get("sub_offsets") or [entry["offset"]]:
        p = int.from_bytes(data[slot:slot + 4], "little")
        out.append(data[p:data.index(b"\0", p)])
    return out


def tw_sig(parts: list[bytes]) -> tuple:
    codes = []
    for b in parts:
        codes += ["00" if c == "00" else c for c in GAME_CODE_RE.findall(b.decode("cp950", "replace"))]
    return len(parts), tuple(codes)


def jp_sig(src: str) -> tuple:
    parts = src.split("{j}")
    codes = []
    for m in CSV_CODE_RE.finditer(src):
        codes.append(m.group(1) if m.group(1) else "00")
    return len(parts), tuple(codes)


def main() -> int:
    full = json.loads((ROOT / "data" / "translations-full.json").read_text(encoding="utf-8"))["en"]
    payload = json.loads((ROOT / "data" / "translations-tw.json").read_text(encoding="utf-8"))["en"]
    headers = str(ROOT / "data" / "headers.tw.json")
    bins = {k: (DEC / v).read_bytes() for k, v in FILE_MAP.items() if (DEC / v).exists()}
    lines = []
    for xpath in sorted(payload):
        data = bins.get(xpath.split("/")[0])
        jp_rows = full.get(xpath)
        if data is None or not jp_rows:
            continue
        ents = extract_text_data_from_bytes(data, read_extraction_config(xpath, headers), "tw")
        tw = [tw_sig(raw_parts(data, e)) for e in ents]
        jp = {int(r["index"]): jp_sig(r.get("source") or "") for r in jp_rows}
        best = []
        for k in range(-8, 9):
            hit = tot = 0
            for i, ts in enumerate(tw):
                js = jp.get(i + k)
                if js is None or (ts[0] == 1 and not ts[1] and js[0] == 1 and not js[1]):
                    continue
                tot += 1
                hit += ts == js
            if tot:
                best.append((hit / tot, k, tot))
        best.sort(reverse=True)
        k0 = next((b for b in best if b[1] == 0), None)
        lines.append(f"{xpath}: tw={len(tw)} jp={len(jp)} "
                     f"k0={'n/a' if not k0 else f'{k0[0]:.2f}(n={k0[2]})'} "
                     f"best={'n/a' if not best else f'k={best[0][1]} {best[0][0]:.2f}'}")
        if xpath.startswith(("pac/", "gao/weapon_names", "gao/skill_names", "jmp/menu/title")):
            for i, e in enumerate(ents[:4]):
                tw_txt = " | ".join(p.decode("cp950", "replace") for p in raw_parts(data, e))
                r = next((r for r in jp_rows if int(r["index"]) == i), {})
                lines.append(f"    [{i}] TW: {tw_txt[:90]}")
                lines.append(f"        JP: {(r.get('source') or '')[:90]}")
                lines.append(f"        EN: {(r.get('target') or '')[:90]}")
    out = ROOT / "scripts" / "_order_report.txt"
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
