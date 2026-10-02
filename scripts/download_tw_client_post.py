#!/usr/bin/env python3
"""Download TW PC client split zips with GET + Range resume."""
from __future__ import annotations

import os
import re
import sys
import time

import requests

OUT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "client", "pc_parts"))
FILES = [
    ("1Y-IYshGF-e0a-uZ0Ic1QsRHxMAc_kj8h", "client.zip.001"),
    ("1JuYaHWIcMGcwX46xEjs0IPDQApCxQWcS", "client.zip.002"),
    ("1UJOMHCxItaDployvwgYlH07C7ApJqFcR", "client.zip.003"),
]
MIN_OK = 10 * 1024 * 1024


def open_download(sess: requests.Session, fid: str, existing: int):
    warn = sess.get(
        f"https://drive.google.com/uc?id={fid}&export=download",
        timeout=60,
    )
    warn.raise_for_status()
    uuid_m = re.search(r'name="uuid"\s+value="([^"]+)"', warn.text)
    confirm_m = re.search(r'name="confirm"\s+value="([^"]+)"', warn.text)
    params = {
        "id": fid,
        "export": "download",
        "confirm": confirm_m.group(1) if confirm_m else "t",
        "uuid": uuid_m.group(1) if uuid_m else "",
    }
    headers = {}
    if existing > 0:
        headers["Range"] = f"bytes={existing}-"
    r = sess.get(
        "https://drive.usercontent.google.com/download",
        params=params,
        stream=True,
        timeout=120,
        headers=headers,
    )
    return r, params


def download_one(fid: str, name: str) -> None:
    dest = os.path.join(OUT, name)
    part = dest + ".part"
    if os.path.exists(dest) and os.path.getsize(dest) > MIN_OK:
        print(f"skip {name} {os.path.getsize(dest)}", flush=True)
        return

    existing = os.path.getsize(part) if os.path.exists(part) else 0
    for attempt in range(1, 12):
        existing = os.path.getsize(part) if os.path.exists(part) else 0
        print(f"==> {name} attempt {attempt} resume_from={existing}", flush=True)
        try:
            sess = requests.Session()
            sess.headers["User-Agent"] = (
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/120.0.0.0 Safari/537.36"
            )
            r, params = open_download(sess, fid, existing)
            with r:
                ct = r.headers.get("Content-Type", "")
                cd = r.headers.get("Content-Disposition", "")
                cl = r.headers.get("Content-Length")
                cr = r.headers.get("Content-Range")
                print(
                    f"status={r.status_code} ct={ct} cl={cl} cr={cr} "
                    f"cd={(cd or '')[:80]} uuid={params.get('uuid')}",
                    flush=True,
                )
                if r.status_code not in (200, 206) or "text/html" in ct or not cd:
                    peek = next(r.iter_content(200), b"")
                    raise RuntimeError(f"not a file: {peek[:120]!r}")

                # Server ignored Range and resent full body
                if existing > 0 and r.status_code == 200 and not cr:
                    print("Range ignored; truncating and rewriting", flush=True)
                    existing = 0

                total = None
                if cr and "/" in cr:
                    total = int(cr.rsplit("/", 1)[-1])
                elif cl and r.status_code == 200:
                    total = int(cl)
                elif cl and existing:
                    total = existing + int(cl)

                mode = "ab" if existing > 0 and r.status_code == 206 else "wb"
                if mode == "wb":
                    existing = 0
                written = existing
                t0 = time.time()
                last = t0
                with open(part, mode) as f:
                    for chunk in r.iter_content(1024 * 1024):
                        if not chunk:
                            continue
                        f.write(chunk)
                        written += len(chunk)
                        now = time.time()
                        if now - last >= 5:
                            elapsed = max(now - t0, 1)
                            # speed of this session only
                            speed = (written - existing) / elapsed / 1e6
                            if total:
                                pct = 100.0 * written / total
                                print(
                                    f"{name}: {written/1e6:.0f}/{total/1e6:.0f} MB "
                                    f"({pct:.1f}%) {speed:.1f} MB/s",
                                    flush=True,
                                )
                            else:
                                print(
                                    f"{name}: {written/1e6:.0f} MB {speed:.1f} MB/s",
                                    flush=True,
                                )
                            last = now

            if total and written < total:
                raise RuntimeError(f"incomplete {written}/{total}")
            if written < MIN_OK:
                raise RuntimeError(f"too small {written}")
            os.replace(part, dest)
            print(f"DONE {name} {written}", flush=True)
            return
        except Exception as e:
            print(f"ERR {type(e).__name__}: {e}", flush=True)
            # keep .part for resume
            time.sleep(min(20, 2 * attempt))
    raise SystemExit(f"failed {name}")


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    os.makedirs(OUT, exist_ok=True)
    for fid, name in FILES:
        download_one(fid, name)
    print("ALL_DONE", flush=True)
    for f in sorted(os.listdir(OUT)):
        p = os.path.join(OUT, f)
        if os.path.isfile(p):
            print(f, os.path.getsize(p), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
