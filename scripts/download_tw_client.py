#!/usr/bin/env python3
"""Download Taiwan PC client split zip parts from the shared Drive folder."""
from __future__ import annotations

import os
import sys
import time

import gdown

OUT = os.path.join(os.path.dirname(__file__), "..", "client", "pc_parts")
FILES = [
    ("1Y-IYshGF-e0a-uZ0Ic1QsRHxMAc_kj8h", "client.zip.001"),
    ("1JuYaHWIcMGcwX46xEjs0IPDQApCxQWcS", "client.zip.002"),
    ("1UJOMHCxItaDployvwgYlH07C7ApJqFcR", "client.zip.003"),
]


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    out = os.path.abspath(OUT)
    os.makedirs(out, exist_ok=True)

    for fid, name in FILES:
        dest = os.path.join(out, name)
        if os.path.exists(dest) and os.path.getsize(dest) > 10 * 1024 * 1024:
            print(f"skip existing {name} {os.path.getsize(dest)}", flush=True)
            continue
        url = f"https://drive.google.com/uc?id={fid}"
        for attempt in range(1, 8):
            print(f"downloading {name} attempt {attempt}", flush=True)
            try:
                path = gdown.download(url, output=dest, quiet=False)
                if not path or not os.path.exists(path):
                    raise RuntimeError("no file written")
                sz = os.path.getsize(path)
                print(f"got {path} {sz}", flush=True)
                if sz < 1024 * 1024:
                    raise RuntimeError(f"too small {sz}")
                break
            except Exception as e:
                print(f"ERR {type(e).__name__}: {e}", flush=True)
                if os.path.exists(dest) and os.path.getsize(dest) < 1024 * 1024:
                    try:
                        os.remove(dest)
                    except OSError:
                        pass
                time.sleep(3 * attempt)
        else:
            print(f"failed {name}", flush=True)
            return 1

    print("ALL_DONE", flush=True)
    for f in sorted(os.listdir(out)):
        p = os.path.join(out, f)
        if os.path.isfile(p):
            print(f, os.path.getsize(p), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
