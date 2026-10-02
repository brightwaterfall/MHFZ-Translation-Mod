#!/usr/bin/env python3
"""Probe a Taiwan/CN client and emit headers.tw.json with measured entry counts.

JP Mogapedia indexes assume zz layouts. Taiwan private-server clients often
keep the same begin_pointer slots but different table lengths/order. This
script copies headers.json and rewrites entry_count as:

  {"zz": <original>, "tw": <live count from TW bins>}

Usage:
  py -3 scripts/probe_client_structure.py ^
      --game-dir "C:\\Users\\Nicho\\Desktop\\MHFRONT\\MHFCNClient" ^
      --out data/headers.tw.json

Put headers.tw.json next to the DLL and set in mhfz_text_patch.ini:
  clientProfile=tw
"""
from __future__ import annotations

import argparse
import json
import sys
from copy import deepcopy
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TH = ROOT / "third_party" / "FrontierTextHandler"
sys.path.insert(0, str(TH))

FILE_MAP = {
    "dat": "mhfdat.bin",
    "pac": "mhfpac.bin",
    "inf": "mhfinf.bin",
    "jmp": "mhfjmp.bin",
    "gao": "mhfgao.bin",
    "sqd": "mhfsqd.bin",
    "rcc": "mhfrcc.bin",
    "msx": "mhfmsx.bin",
    "mfd": "mhfmfd.bin",
}


def find_bin(game_dir: Path, name: str) -> Path | None:
    candidates = [
        game_dir / name,
        game_dir / "dat" / name,
        game_dir / "client" / "pc" / "dat" / name,
        game_dir / "pc" / "dat" / name,
    ]
    for c in candidates:
        if c.exists():
            return c
    # shallow search
    hits = list(game_dir.rglob(name))
    return hits[0] if hits else None


def set_entry_count(node: dict, tw_count: int) -> None:
    ec = node.get("entry_count")
    if isinstance(ec, int):
        node["entry_count"] = {"zz": ec, "tw": tw_count, "cn": tw_count}
    elif isinstance(ec, dict):
        node["entry_count"] = dict(ec)
        node["entry_count"]["tw"] = tw_count
        node["entry_count"]["cn"] = tw_count
    else:
        node["entry_count"] = {"tw": tw_count, "cn": tw_count}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game-dir", type=Path, required=True)
    ap.add_argument("--headers", type=Path, default=ROOT / "data" / "headers.json")
    ap.add_argument("--translations", type=Path, default=ROOT / "data" / "translations-en.json")
    ap.add_argument("--out", type=Path, default=ROOT / "data" / "headers.tw.json")
    ap.add_argument("--profile", default="tw")
    args = ap.parse_args()

    from src.common import load_file_data, read_extraction_config  # type: ignore
    from src.pointer_tables import extract_text_data_from_bytes  # type: ignore

    headers = json.loads(args.headers.read_text(encoding="utf-8"))
    out_doc = deepcopy(headers)

    xpaths: list[str] = []
    if args.translations.exists():
        doc = json.loads(args.translations.read_text(encoding="utf-8"))
        root = doc.get("en") or doc.get("translations") or doc
        if isinstance(root, dict):
            xpaths = [k for k, v in root.items() if isinstance(v, list)]
    if not xpaths:
        # fall back: walk all leaves
        def walk(n, path, acc):
            if not isinstance(n, dict):
                return
            if "begin_pointer" in n:
                acc.append("/".join(path))
                return
            for k, v in n.items():
                if k.startswith("_"):
                    continue
                walk(v, path + [k], acc)
        walk(headers, [], xpaths)

    bin_cache: dict[str, bytes] = {}
    updated = 0
    failed = 0
    for xpath in sorted(xpaths):
        file_key = xpath.split("/")[0]
        bin_name = FILE_MAP.get(file_key)
        if not bin_name:
            continue
        if file_key not in bin_cache:
            p = find_bin(args.game_dir, bin_name)
            if not p:
                print(f"missing bin for {xpath}: {bin_name}")
                failed += 1
                continue
            print(f"loading {p}")
            bin_cache[file_key] = load_file_data(str(p))
        try:
            cfg = read_extraction_config(xpath, str(args.headers))
        except Exception as ex:
            print(f"skip headers {xpath}: {ex}")
            failed += 1
            continue
        try:
            entries = extract_text_data_from_bytes(bin_cache[file_key], cfg, args.profile)
        except Exception as ex:
            print(f"extract fail {xpath}: {ex}")
            failed += 1
            continue

        # write back into out_doc leaf
        node = out_doc
        parts = xpath.split("/")
        try:
            for p in parts:
                node = node[p]
        except Exception:
            failed += 1
            continue
        set_entry_count(node, len(entries))
        updated += 1
        print(f"OK {xpath}: tw_count={len(entries)}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    meta = {
        "_mhfz_text_patch": {
            "profile": args.profile,
            "source_headers": str(args.headers),
            "game_dir": str(args.game_dir),
            "sections_updated": updated,
            "sections_failed": failed,
            "note": "entry_count maps include tw/cn measured from this client",
        }
    }
    # Keep meta outside game trees by nesting under a private key if possible
    if isinstance(out_doc, dict):
        out_doc = {**meta, **out_doc}
    args.out.write_text(json.dumps(out_doc, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Wrote {args.out} (updated={updated} failed={failed})")
    return 0 if updated else 1


if __name__ == "__main__":
    raise SystemExit(main())
