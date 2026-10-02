#!/usr/bin/env python3
"""Extract mhf*.bin from the TW PC split zip and run structure probe."""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PARTS = ROOT / "client" / "pc_parts"
EXTRACT = ROOT / "client" / "tw_extract"
SEVEN = Path(r"C:\Program Files\7-Zip\7z.exe")
BINS = [
    "mhfdat.bin",
    "mhfpac.bin",
    "mhfinf.bin",
    "mhfjmp.bin",
    "mhfgao.bin",
    "mhfsqd.bin",
    "mhfrcc.bin",
    "mhfmsx.bin",
    "mhfmfd.bin",
]


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    for i in (1, 2, 3):
        p = PARTS / f"client.zip.00{i}"
        if not p.exists() or p.stat().st_size < 1024 * 1024:
            print(f"missing/incomplete {p}")
            return 1
        print(f"ok {p.name} {p.stat().st_size}")

    if not SEVEN.exists():
        print("7z not found")
        return 1

    EXTRACT.mkdir(parents=True, exist_ok=True)
    # 7-Zip opens the first volume and reads the rest automatically.
    archive = PARTS / "client.zip.001"
    # List archive to find exact paths for mhf bins
    list_out = subprocess.check_output(
        [str(SEVEN), "l", "-slt", str(archive)],
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    wanted_paths: list[str] = []
    current_path = None
    for line in list_out.splitlines():
        if line.startswith("Path = "):
            current_path = line[7:]
        elif line.startswith("Attributes = ") and current_path:
            # skip dirs
            if "D" in line.split("=", 1)[1]:
                current_path = None
                continue
        elif line.startswith("Size = ") and current_path:
            base = current_path.replace("\\", "/").split("/")[-1].lower()
            if base in BINS:
                wanted_paths.append(current_path)
                print(f"found {current_path}")
            current_path = None

    if not wanted_paths:
        # fallback: extract anything named mhf*.bin
        print("no bin paths from listing; trying wildcard extract")
        cmd = [str(SEVEN), "x", f"-o{EXTRACT}", str(archive), "-y", "*mhf*.bin", "*\\mhf*.bin", "*\\dat\\mhf*.bin"]
        print(" ".join(cmd))
        subprocess.check_call(cmd)
    else:
        cmd = [str(SEVEN), "x", f"-o{EXTRACT}", str(archive), "-y", *wanted_paths]
        print("extracting", len(wanted_paths), "files")
        subprocess.check_call(cmd)

    # find game dir containing mhfdat.bin
    hits = list(EXTRACT.rglob("mhfdat.bin"))
    if not hits:
        print("mhfdat.bin not extracted")
        return 1
    game_dir = hits[0].parent
    print("game_dir", game_dir)

    probe = ROOT / "scripts" / "probe_client_structure.py"
    out = ROOT / "data" / "headers.tw.json"
    cmd = [
        sys.executable,
        str(probe),
        "--game-dir",
        str(game_dir),
        "--out",
        str(out),
    ]
    print("running", " ".join(cmd))
    return subprocess.call(cmd)


if __name__ == "__main__":
    raise SystemExit(main())
