#!/usr/bin/env python3
"""Align Mogapedia EN targets onto a Taiwan/CN client's live string indices.

Why this exists
---------------
Mogapedia payloads are keyed to **JP (zz) indices**. Taiwan/CN private-server
bins often keep a similar table layout but different lengths / order / text
(Chinese). Blind XP+index apply then writes English into the wrong slots.

This script:
  1. Extracts live TW/CN source strings per xpath (via FrontierTextHandler).
  2. Loads Mogapedia EN rows (optionally full ``translations.json`` with JP
     ``source`` for content matching).
  3. Realigns EN targets onto **TW indices** using, in order:
       a. exact normalized source match (JP source == TW source) — rare on CN
       b. fuzzy source match (optional)
       c. index fallback when both sides share that index
  4. Writes ``translations-tw.json`` in the same shape the mod already loads.

Usage
-----
  py -3 scripts/align_translations_to_client.py ^
      --game-dir client\\tw_extract ^
      --headers data\\headers.tw.json ^
      --en data\\translations-en.json ^
      --full data\\translations-full.json ^
      --out data\\translations-tw.json

Then ship ``translations-tw.json`` next to the DLL and either:
  - set payload=translations-tw.json in mhfz_text_patch.ini, or
  - leave clientProfile=tw (mod prefers translations-tw.json automatically).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from copy import deepcopy
from difflib import SequenceMatcher
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

# Color / control noise for matching.
_COLOR_RE = re.compile(r"\{/?c\d*\}|~/[Cc]\d{0,2}|~[Cc]\d{2}")
_WS_RE = re.compile(r"\s+")


def normalize(s: str) -> str:
    if not s:
        return ""
    s = _COLOR_RE.sub("", s)
    s = s.replace("{j}", "\n")
    s = _WS_RE.sub("", s)
    return s.casefold()


def find_bin(game_dir: Path, name: str) -> Path | None:
    for c in (
        game_dir / name,
        game_dir / "dat" / name,
        game_dir / "client" / "pc" / "dat" / name,
        game_dir / "pc" / "dat" / name,
    ):
        if c.exists():
            return c
    hits = list(game_dir.rglob(name))
    return hits[0] if hits else None


def load_en_targets(path: Path) -> dict[str, dict[int, str]]:
    """xpath -> {index: target} from translations-en.json shape."""
    doc = json.loads(path.read_text(encoding="utf-8"))
    root = doc.get("en") or doc.get("translations") or doc
    out: dict[str, dict[int, str]] = {}
    if not isinstance(root, dict):
        raise SystemExit(f"unexpected EN shape in {path}")
    for xpath, rows in root.items():
        if xpath in ("metadata", "meta") or not isinstance(rows, list):
            continue
        bucket: dict[int, str] = {}
        for row in rows:
            if not isinstance(row, dict):
                continue
            idx = row.get("index")
            target = row.get("target") or ""
            if idx is None or not target:
                continue
            bucket[int(idx)] = target
        if bucket:
            out[xpath] = bucket
    return out


def load_jp_sources(path: Path | None) -> dict[str, dict[int, str]]:
    """xpath -> {index: source} from full translations.json if present."""
    if not path or not path.exists():
        return {}
    doc = json.loads(path.read_text(encoding="utf-8"))
    # Common shapes: {xpath: [{index,source,en|target|translations.en}]} or
    # {en: ...} without source; or {xpath: {strings: [...]}}.
    out: dict[str, dict[int, str]] = {}

    def ingest_rows(xpath: str, rows: list) -> None:
        bucket = out.setdefault(xpath, {})
        for row in rows:
            if not isinstance(row, dict):
                continue
            idx = row.get("index")
            if idx is None:
                continue
            src = row.get("source") or row.get("jp") or ""
            if not src:
                continue
            bucket[int(idx)] = src

    if isinstance(doc, dict) and "en" in doc and isinstance(doc["en"], dict):
        # May still carry source alongside target in some exports.
        for xpath, rows in doc["en"].items():
            if isinstance(rows, list):
                ingest_rows(xpath, rows)
        # Sibling language-neutral block
        for key in ("source", "sources", "jp", "zz"):
            block = doc.get(key)
            if isinstance(block, dict):
                for xpath, rows in block.items():
                    if isinstance(rows, list):
                        ingest_rows(xpath, rows)

    # Flat xpath map with source field
    if isinstance(doc, dict):
        for xpath, rows in doc.items():
            if xpath in ("en", "fr", "metadata", "meta", "source", "sources", "jp", "zz"):
                continue
            if isinstance(rows, list):
                ingest_rows(xpath, rows)
            elif isinstance(rows, dict) and isinstance(rows.get("strings"), list):
                ingest_rows(xpath, rows["strings"])

    # Drop empty
    return {k: v for k, v in out.items() if v}


def extract_tw_section(
    data: bytes,
    xpath: str,
    headers_path: str,
    profile: str,
) -> list[str] | None:
    from src.common import read_extraction_config  # type: ignore
    from src.pointer_tables import extract_text_data_from_bytes  # type: ignore

    try:
        cfg = read_extraction_config(xpath, headers_path)
    except Exception as ex:
        print(f"  skip headers {xpath}: {ex}")
        return None
    try:
        entries = extract_text_data_from_bytes(data, cfg, profile)
    except Exception as ex:
        print(f"  extract fail {xpath}: {ex}")
        return None
    # TextHandler returns a dense list; index == position.
    return [str(e.get("text") or "") for e in entries]


def align_section(
    tw_sources: list[str],
    en_by_index: dict[int, str],
    jp_by_index: dict[int, str],
    *,
    fuzzy: float,
) -> tuple[list[dict], dict[str, int]]:
    """Return Mogapedia-shaped rows for TW indices + stats."""
    stats = {
        "tw_slots": len(tw_sources),
        "en_rows": len(en_by_index),
        "matched_source": 0,
        "matched_fuzzy": 0,
        "matched_index": 0,
        "unmatched_en": 0,
        "emitted": 0,
    }

    # Build JP-source → list of EN indices (handle duplicates).
    src_to_en: dict[str, list[int]] = defaultdict(list)
    for jp_idx, src in jp_by_index.items():
        key = normalize(src)
        if not key:
            continue
        if jp_idx in en_by_index:
            src_to_en[key].append(jp_idx)

    used_en: set[int] = set()
    out_rows: list[dict] = []

    # Pass 1: exact source match.
    for tw_idx, tw_text in enumerate(tw_sources):
        key = normalize(tw_text)
        if not key or key not in src_to_en:
            continue
        candidates = [i for i in src_to_en[key] if i not in used_en]
        if not candidates:
            continue
        jp_idx = candidates[0]
        target = en_by_index.get(jp_idx)
        if not target:
            continue
        used_en.add(jp_idx)
        out_rows.append({"index": str(tw_idx), "target": target, "via": "source"})
        stats["matched_source"] += 1

    # Pass 2: fuzzy (only if enabled, JP sources exist, and table is small).
    if fuzzy > 0 and jp_by_index and len(tw_sources) <= 800:
        jp_norm = [
            (idx, normalize(src), en_by_index[idx])
            for idx, src in jp_by_index.items()
            if idx in en_by_index and idx not in used_en and normalize(src)
        ]
        claimed_tw = {int(r["index"]) for r in out_rows}
        for tw_idx, tw_text in enumerate(tw_sources):
            if tw_idx in claimed_tw:
                continue
            key = normalize(tw_text)
            if not key or len(key) < 2:
                continue
            best_i = -1
            best_score = 0.0
            best_target = ""
            for jp_idx, jpn, target in jp_norm:
                if jp_idx in used_en:
                    continue
                score = SequenceMatcher(None, key, jpn).ratio()
                if score > best_score:
                    best_score = score
                    best_i = jp_idx
                    best_target = target
            if best_i >= 0 and best_score >= fuzzy:
                used_en.add(best_i)
                out_rows.append(
                    {
                        "index": str(tw_idx),
                        "target": best_target,
                        "via": f"fuzzy:{best_score:.2f}",
                    }
                )
                stats["matched_fuzzy"] += 1

    # Pass 3: index fallback for remaining EN rows that fit TW length.
    claimed_tw = {int(r["index"]) for r in out_rows}
    for jp_idx, target in en_by_index.items():
        if jp_idx in used_en:
            continue
        if jp_idx < 0 or jp_idx >= len(tw_sources):
            stats["unmatched_en"] += 1
            continue
        if jp_idx in claimed_tw:
            stats["unmatched_en"] += 1
            continue
        # Prefer index only when TW slot looks like a real string or empty EN-able.
        out_rows.append({"index": str(jp_idx), "target": target, "via": "index"})
        claimed_tw.add(jp_idx)
        used_en.add(jp_idx)
        stats["matched_index"] += 1

    # Strip debug "via" for ship payload; keep stats.
    ship = [{"index": r["index"], "target": r["target"]} for r in out_rows]
    stats["emitted"] = len(ship)
    return ship, stats


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game-dir", type=Path, required=True)
    ap.add_argument("--headers", type=Path, default=ROOT / "data" / "headers.tw.json")
    ap.add_argument("--en", type=Path, default=ROOT / "data" / "translations-en.json")
    ap.add_argument(
        "--full",
        type=Path,
        default=ROOT / "data" / "translations-full.json",
        help="Optional Mogapedia translations.json with JP source fields",
    )
    ap.add_argument("--out", type=Path, default=ROOT / "data" / "translations-tw.json")
    ap.add_argument("--report", type=Path, default=ROOT / "data" / "align-report.json")
    ap.add_argument("--profile", default="tw")
    ap.add_argument(
        "--fuzzy",
        type=float,
        default=0.0,
        help="Min SequenceMatcher ratio for fuzzy source match (0 disables; avoid on huge tables)",
    )
    ap.add_argument(
        "--keep-via",
        action="store_true",
        help="Keep alignment method tags in output (debug)",
    )
    args = ap.parse_args()

    from src.common import load_file_data  # type: ignore

    en = load_en_targets(args.en)
    jp_sources = load_jp_sources(args.full if args.full.exists() else None)
    print(f"EN xpaths={len(en)}  JP-source xpaths={len(jp_sources)}")

    bin_cache: dict[str, bytes] = {}
    out_doc: dict = {
        "en": {},
        "metadata": {
            "profile": args.profile,
            "aligned_from": str(args.en),
            "game_dir": str(args.game_dir),
            "headers": str(args.headers),
            "full_sources": str(args.full) if args.full.exists() else None,
            "note": "TW/CN indices; targets from Mogapedia EN after align",
        },
    }
    report: dict = {"sections": {}, "totals": defaultdict(int)}

    for xpath in sorted(en.keys()):
        file_key = xpath.split("/")[0]
        bin_name = FILE_MAP.get(file_key)
        if not bin_name:
            print(f"skip unknown file key: {xpath}")
            continue
        if file_key not in bin_cache:
            p = find_bin(args.game_dir, bin_name)
            if not p:
                print(f"missing bin {bin_name} for {xpath}")
                continue
            print(f"loading {p}")
            bin_cache[file_key] = load_file_data(str(p))

        print(f"align {xpath} ...")
        tw_list = extract_tw_section(
            bin_cache[file_key], xpath, str(args.headers), args.profile
        )
        if tw_list is None:
            # Do NOT passthrough JP indices — that causes misaligned writes.
            report["sections"][xpath] = {
                "mode": "skipped",
                "emitted": 0,
                "reason": "tw extract failed — needs TW begin_pointer",
            }
            report["totals"]["skipped_extract"] += 1
            print("  skipped (TW extract failed — no JP passthrough)")
            continue

        # Require either source matches or similar table length vs EN.
        en_max = max(en[xpath].keys()) if en[xpath] else 0
        len_ratio = len(tw_list) / max(en_max + 1, 1)

        ship, stats = align_section(
            tw_list,
            en[xpath],
            jp_sources.get(xpath, {}),
            fuzzy=args.fuzzy,
        )

        # If almost nothing matched by source and lengths diverge badly, skip.
        if (
            stats["matched_source"] == 0
            and stats["matched_fuzzy"] == 0
            and (len_ratio < 0.5 or len_ratio > 2.0)
        ):
            report["sections"][xpath] = {
                **stats,
                "mode": "skipped_drift",
                "len_ratio": len_ratio,
            }
            report["totals"]["skipped_drift"] += 1
            print(
                f"  skipped drift tw={len(tw_list)} en_max={en_max} "
                f"ratio={len_ratio:.2f}"
            )
            continue

        out_doc["en"][xpath] = ship
        report["sections"][xpath] = stats
        for k, v in stats.items():
            report["totals"][k] += v
        print(
            f"  tw={stats['tw_slots']} en={stats['en_rows']} "
            f"src={stats['matched_source']} fuzzy={stats['matched_fuzzy']} "
            f"idx={stats['matched_index']} emit={stats['emitted']} "
            f"drop={stats['unmatched_en']}"
        )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    # Convert totals
    report["totals"] = dict(report["totals"])
    args.out.write_text(
        json.dumps(out_doc, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"Wrote {args.out}")
    print(f"Wrote {args.report}")
    print("totals", report["totals"])
    return 0 if out_doc["en"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
