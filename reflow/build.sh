#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Builds the reflow engine: Tectonic's XeTeX with the reflow patches, as WebAssembly, and its LaTeX format.
#   reflow/build.sh image     the build image (emsdk + Rust + the Emscripten ports), Docker tag reflow-tectonic-wasm
#   reflow/build.sh deps      the C dependencies, in reflow/deps/prefix (graphite2, the ICU data XeTeX needs,
#                             a fontconfig stub; zlib, libpng, freetype and ICU come from the image's ports)
#   reflow/build.sh wasm      the engine: reflow/dist/async/{tectonic.mjs,tectonic_wasm.wasm,STAMP}
#   reflow/build.sh format    reflow/dist/latex.fmt, made by that engine under Node (TeX files from Tectonic's
#                             bundle, cached in $TECTONIC_CACHE, else reflow/.cache/tectonic-bundle)
#   reflow/build.sh tree      reflow/dist/tree/texlive/2022/: the TeX Live tree the engine reads beside its page (the
#                             set in reflow/tree/starting-set.txt; packages from TeX Live 2022's final archive, checked
#                             against its sha512s, cached in $TEXLIVE_ARCHIVE, else reflow/.cache/texlive-archive)
#   reflow/build.sh all       the five in turn
# Needs Docker and git. Every step runs in a container with the repository at /work; the compile cache is the
# Docker volume reflow-tectonic-target.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=reflow-tectonic-wasm
NODE=node:24             # (not -slim: the tree step needs xz)
CACHE="${TECTONIC_CACHE:-$ROOT/reflow/.cache/tectonic-bundle}"         # Tectonic's bundle, read in chunks
ARCHIVE="${TEXLIVE_ARCHIVE:-$ROOT/reflow/.cache/texlive-archive}"       # TeX Live's archive files
in_image() { docker run --rm --platform linux/amd64 -v "$ROOT":/work -w /work -v reflow-tectonic-target:/target "$IMAGE" bash -lc "$*"; }
in_node() {
  mkdir -p "$CACHE" "$ARCHIVE"
  docker run --rm -v "$ROOT":/work -w /work -v "$CACHE":/cache -e TECTONIC_CACHE=/cache \
    -v "$ARCHIVE":/archive -e TEXLIVE_ARCHIVE=/archive "$NODE" "$@"
}

image() { docker build --platform linux/amd64 -t "$IMAGE" "$ROOT/reflow/docker"; }
deps() {
  # ICU data: the port ships only stubdata; the items XeTeX needs, cut from the full ICU 68 package in the port
  # cache, become a C array (reflow/deps/icu-data.ts)
  mkdir -p "$ROOT/reflow/deps/src"
  in_image cp /emsdk/upstream/emscripten/cache/ports/icu/icu/source/data/in/icudt68l.dat reflow/deps/src/icudt68l.dat
  in_node node reflow/deps/icu-data.ts c reflow/deps/src/icudt68l.dat reflow/deps/src/icudata.c \
    'brkitr/.*\.res$' 'brkitr/(line.*|char|word|sent|title)\.brk$' 'brkitr/(thai|lao)dict\.dict$' 'cnvalias\.icu$'
  in_image ./reflow/deps/build-deps.sh
}
wasm() {
  # HarfBuzz is a submodule of Tectonic (compiled from source by tectonic_bridge_harfbuzz)
  git -C "$ROOT" submodule update --init --depth 1 crates/bridge_harfbuzz/harfbuzz
  in_image ./reflow/scripts/build-wasm.sh
}
format() { in_node node reflow/node/tectonic-node.ts format; }
tree() { in_node node reflow/tree/build-tree.ts "$@"; }

case "${1:-}" in
  image) image ;;
  deps) deps ;;
  wasm) wasm ;;
  format) format ;;
  tree) shift; tree "$@" ;;
  all) image; deps; wasm; format; tree ;;
  *) sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
