# Architecture

## Goal

FrontierTextHandler decrypts/decompresses game bins, walks pointer tables from
`headers.json`, and rewrites Shift-JIS strings from a translation JSON.
This mod does the **rewrite step live** against buffers already mapped by the
game, so disk files stay untouched.

## Pipeline

1. **OnAttach** — load `translations-en.json` + `headers.json`.
2. **Locate text images** — `BufferLocator` scans committed memory for
   `dat` / `pac` / `inf` / `gao` / `jmp` / `rcc` fingerprints (header
   begin-pointers resolve to SJIS string tables).
3. **Apply** — for each Mogapedia xpath, resolve the section config, collect
   live entries (flat / grouped / struct / quest), encode `{cNN}`→`~CNN` and
   UTF-8→CP932, write into existing slots or relocate on overflow.
4. **Maintain** — retry on lobby/quest ticks until at least one section applies
   (buffers may appear after first load). Optional `reapplyEachTick`.

## Encoding

- Game strings: CP932 (Shift_JIS).
- Color markers: `{cNN}` / `{/c}` ↔ `~CNN` / `~C00`.
- Join markers: `{j}` splits across sibling pointer slots.

## CN / TW layout drift

1. Probe headers: `scripts/probe_client_structure.py`
2. Align EN onto client indices: `scripts/align_translations_to_client.py`
   (uses Mogapedia `translations.json` sources when present; else index clamp)
3. Ship `translations-tw.json` + `headers.tw.json` with the DLL.

## Files

| Path | Role |
|------|------|
| `translation/TranslationStore.*` | Mogapedia JSON loader |
| `translation/HeadersStore.*` | `headers.json` section configs |
| `translation/BufferLocator.*` | In-process image discovery |
| `translation/TextApplicator.*` | Live + offset-map apply |
| `translation/GameCodec.*` | Color + SJIS helpers |
| `data/headers.json` | Copied from FrontierTextHandler |
| `scripts/build_runtime_patch.py` | Offline CN map builder |
