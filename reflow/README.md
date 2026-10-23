# reflow-tectonic

The TeX engine of the reflow projects ([reflowtex](https://github.com/radek-p/reflowtex),
[ReflowEd](https://github.com/radek-p/reflowed)): [Tectonic](https://github.com/tectonic-typesetting/tectonic)'s
XeTeX with a capture API, built to WebAssembly for the browser.

Copyright 2026 Radosław Piórkowski. The changes on this branch and everything in `reflow/` are licensed under the
GNU Affero General Public License, version 3 or later (AGPL-3.0-or-later, [reflow/LICENSE](LICENSE)). Tectonic
itself is under the MIT licence ([LICENSE](../LICENSE) at the top, copyright the Tectonic Project); its files keep
that licence and notice. The libraries built into the engine keep their own licences
([reflow/licences/](licences/)).

## What the branch adds to Tectonic

The `reflow` branch is upstream Tectonic at `d2224d9` plus a series of commits to `xetex_layout` and
`engine_xetex`. Everything they add is off unless the driver switches it on; with it off, the XDV and the log are
byte-identical to upstream's.

- **The capture API** (`xetex-rtxcapture.c`): while TeX works, it writes every paragraph as `line_break` receives
  it with TeX's own breaks, the main vertical list in document order, displays, fonts and glyph metrics, with
  HarfBuzz's raw output (clusters, font units). This is what lets a page break the lines again for any width.
- **Pageless typesetting:** the page builder gets zero-height originals, so the document is never cut into pages.
- **Math lists:** formulas as TeX's noads, before they become boxes, so their spacing can be worked out again.
- **Display samples:** a display typeset again at other widths, in forked copies of the engine.
- **Source positions:** every typeset character, glyph and ligature carries its file, line, column and extent,
  also through 8-bit (TFM) fonts, ligatures, hyphenation, accents, maths constructions, macro calls, `\futurelet`
  peeks and active Unicode characters.
- **Attributes:** LuaTeX-style attributes (`\attribute909`–`917`) on every node.
- **Hyphenation data:** every used font's hyphen character with its protrusion codes, and the hyphenation points
  of 8-bit words.
- **A fix:** XeTeX's per-font-number caches are cleared at the start of each run.

## Building

Needs Docker and git.

```
reflow/build.sh image    # the build image: emsdk 3.1.74, Rust 1.89, the Emscripten ports
reflow/build.sh deps     # the C dependencies (graphite2, ICU data, a fontconfig stub) in reflow/deps/prefix
reflow/build.sh wasm     # reflow/dist/async/: tectonic.mjs, tectonic_wasm.wasm, STAMP
reflow/build.sh format   # reflow/dist/latex.fmt, made by that engine under Node
reflow/build.sh all      # the four in turn
```

The format step reads TeX files from Tectonic's bundle (v33, TeX Live 2022), cached in `$TECTONIC_CACHE`, else
`reflow/.cache/tectonic-bundle`. `STAMP` records the commit the engine was built from (`+` when the tree had
changes) and the wasm's sha256.

## Layout

| Path | What |
|---|---|
| `reflow/driver/` | the Rust driver: a C ABI for JavaScript, a warm-snapshot hook for per-block compiles |
| `reflow/js/library.js` | the Emscripten JavaScript library linked into the engine |
| `reflow/docker/` | the build image |
| `reflow/deps/` | the C dependencies' build and the ICU data cut |
| `reflow/scripts/build-wasm.sh` | the engine build, run in the image |
| `reflow/node/` | a small Node host: builds the format, compiles a document |
