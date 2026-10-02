#!/usr/bin/env python3
"""Replay the DLL's table walk against decompressed TW bins (offline).

For every xpath in the shipped payload, resolve begin_pointer -> table ->
strings in the decompressed file exactly like TextApplicator does, and compare
the live slot count / index layout with what FrontierTextHandler extracts
(which is what align_translations_to_client.py indexed the payload against).
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party" / "FrontierTextHandler"))

from src.common import read_extraction_config  # type: ignore  # noqa: E402
from src.pointer_tables import extract_text_data_from_bytes  # type: ignore  # noqa: E402

DEC = ROOT / "client" / "tw_decompressed"
FILE_MAP = {"dat": "mhfdat.bin", "pac": "mhfpac.bin", "inf": "mhfinf.bin",
            "gao": "mhfgao.bin", "jmp": "mhfjmp.bin", "rcc": "mhfrcc.bin"}


def u32(b: bytes, off: int) -> int | None:
    if off < 0 or off + 4 > len(b):
        return None
    return struct.unpack_from("<I", b, off)[0]


def main() -> int:
    headers_path = ROOT / "data" / "headers.tw.json"
    payload = json.loads((ROOT / "data" / "translations-tw.json").read_text(encoding="utf-8"))["en"]
    bins = {k: (DEC / v).read_bytes() for k, v in FILE_MAP.items() if (DEC / v).exists()}

    for xpath in sorted(payload):
        key = xpath.split("/")[0]
        data = bins.get(key)
        if data is None:
            continue
        try:
            cfg = read_extraction_config(xpath, str(headers_path))
        except Exception as ex:
            print(f"{xpath}: headers error {ex}")
            continue
        bp = cfg.get("begin_pointer")
        bp = int(bp, 16) if isinstance(bp, str) else bp
        stored = u32(data, bp) if bp is not None else None
        try:
            th = extract_text_data_from_bytes(data, cfg, "tw")
            th_n = len(th)
        except Exception as ex:
            th_n = f"ERR {type(ex).__name__}: {ex}"
        rows = payload[xpath]
        max_idx = max((r["index"] for r in rows), default=-1)
        print(f"{xpath}: begin_ptr=0x{bp:X} stored={'None' if stored is None else hex(stored)} "
              f"TH_entries={th_n} payload_rows={len(rows)} max_idx={max_idx}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
