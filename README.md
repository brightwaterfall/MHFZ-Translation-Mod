# MHFZ Translation Mod (Pax)

Runtime English text patch for Monster Hunter Frontier via **Pax-Mhfz-Loader**.
Applies Mogapedia / FrontierTextHandler translation payloads **in memory** so
Chinese-server clients that reject on-disk file edits can still show English UI.

## Status

v0.2.6 — live TextHandler-style apply + **TW-aligned payload**:

1. Prefer `translations-tw.json` when `clientProfile=tw` (built by align script).
2. Load `headers.tw.json` for Taiwan entry counts / pointers.
3. Scan process memory for decompressed `mhfdat` / `mhfpac` / … images.
4. Walk pointer tables per xpath and write CP932 strings (with `{cNN}` / `{j}`).
5. Overflow strings relocate into trailing image zeros or a private pool.

## Install (Taiwan / CN)

1. Copy `result.zip` → `<game>\mods\` (DLL, `headers.tw.json`, `translations-tw.json`).
2. Delete old `mhfz_text_patch.ini` once so defaults refresh.
3. Launch with Pax. Log should show payload `translations-tw.json`.

Rebuild the TW payload after client updates:

```
py -3 scripts\align_translations_to_client.py --game-dir <MHFCNClient> --out data\translations-tw.json
```

See `docs/TAIWAN-CLIENT.md`.

## Config

`mods\mhfz_text_patch.ini` (created on first run):

```
enabled=1
lang=en
payload=
clientProfile=tw
safeIndexApply=1
inPlaceOnly=0
```

Empty `payload=` auto-picks `translations-tw.json` for tw/cn.

## Resources

- [FrontierTextHandler](https://github.com/Houmgaor/FrontierTextHandler)
- [MHFrontier-Translation releases](https://github.com/Mogapedia/MHFrontier-Translation/releases)

## Dev notes

See `docs/ARCHITECTURE.md`.
