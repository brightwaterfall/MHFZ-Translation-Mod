# Taiwan / CN client structure

Monster Hunter Frontier **Taiwan / CN private-server** clients often keep the
same `begin_pointer` header slots as JP (`zz`) but change **table lengths and
string order**. Mogapedia EN indexes are JP-keyed, so blind apply corrupts UI
(e.g. NPC named `OFF`).

## What the mod expects

In `mods\mhfz_text_patch.ini`:

```
clientProfile=tw
safeIndexApply=1
inPlaceOnly=0
```

`inPlaceOnly` must stay **0** on TW/CN — English almost never fits Chinese string
lengths, so the mod relocates pointers into a small heap pool.
Headers resolution order:

1. `headersFile=` if set
2. `headers.tw.json` when `clientProfile=tw`
3. else `headers.json` (JP)

## Build Taiwan headers from the client

On a machine that has the TW/CN game folder (with `mhfdat.bin` etc.):

```bat
py -3 scripts\probe_client_structure.py --game-dir "C:\path\to\MHFCNClient" --out data\headers.tw.json
```

Copy `headers.tw.json` into `<game>\mods\` next to the DLL.

## Align EN onto TW indices (required for correct text)

Mogapedia EN is JP-indexed. Rebuild a TW-indexed payload:

```bat
py -3 scripts\align_translations_to_client.py ^
  --game-dir "C:\path\to\MHFCNClient" ^
  --headers data\headers.tw.json ^
  --en data\translations-en.json ^
  --full data\translations-full.json ^
  --out data\translations-tw.json
```

`--full` is optional Mogapedia `translations.json` (has JP `source`). When TW
bins still contain JP text, rows match by source; otherwise index fallback is
used. Ship `translations-tw.json` in `mods\`. With `clientProfile=tw` the mod
loads it automatically.

Optionally also:

```bat
py -3 scripts\build_runtime_patch.py --game-dir "C:\path\to\MHFCNClient\dat" --translations data\translations-tw.json --out data\runtime_patch.jsonl
```

## Notes

- Profile aliases: `tw` and `cn` both read the `tw` entry_count bucket.
- Until `headers.tw.json` exists, Safe index apply will skip drifted sections
  rather than write the wrong English into the wrong slots.
- Prefer `translations-tw.json` over raw `translations-en.json` on TW/CN.
