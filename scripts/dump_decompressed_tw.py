#!/usr/bin/env python3
"""Decrypt/decompress TW bins to client/tw_decompressed/ for offline layout checks."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party" / "FrontierTextHandler"))

from src.common import load_file_data  # type: ignore  # noqa: E402

SRC = ROOT / "client" / "tw_extract" / "dat"
DST = ROOT / "client" / "tw_decompressed"


def main() -> int:
    DST.mkdir(parents=True, exist_ok=True)
    for name in ("mhfdat.bin", "mhfpac.bin", "mhfinf.bin", "mhfgao.bin", "mhfjmp.bin", "mhfrcc.bin"):
        p = SRC / name
        if not p.exists():
            print(f"missing {p}")
            continue
        data = load_file_data(str(p))
        (DST / name).write_bytes(data)
        print(f"{name}: disk={p.stat().st_size} decompressed={len(data)} (0x{len(data):X})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
