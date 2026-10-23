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
#   reflow/build.sh check     the engine and the format typeset reflow/tests/hello.tex
#   reflow/build.sh package   reflow/dist/release/: the release assets (the engine, latex.fmt.gz, the tree as
#                             texlive-tree.tar.gz, STAMP.txt, SHA256SUMS)
#   reflow/build.sh all       the seven in turn
# Needs Docker and git. Every step runs in a container with the repository at /work. The compile cache is the
# Docker volume reflow-tectonic-target, or the directory $REFLOW_TARGET (CI caches that one).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=reflow-tectonic-wasm
NODE=node:24             # (not -slim: the tree step needs xz)
CACHE="${TECTONIC_CACHE:-$ROOT/reflow/.cache/tectonic-bundle}"         # Tectonic's bundle, read in chunks
ARCHIVE="${TEXLIVE_ARCHIVE:-$ROOT/reflow/.cache/texlive-archive}"       # TeX Live's archive files
TARGET="${REFLOW_TARGET:-reflow-tectonic-target}"                         # a Docker volume, or an absolute path
in_image() { docker run --rm --platform linux/amd64 -v "$ROOT":/work -w /work -v "$TARGET":/target "$IMAGE" bash -lc "$*"; }
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
check() {
  # the engine and the format just built typeset a small document: status 0 (or 1, warnings), an .xdv and the
  # capture (hello.rtxc.jsonl)
  in_node bash -c 'set -e; rm -rf /tmp/out
    node reflow/node/tectonic-node.ts compile reflow/tests/hello.tex --out /tmp/out 2> /tmp/log || true
    cat /tmp/log; grep -Eq "^status [01];" /tmp/log; test -s /tmp/out/hello.xdv; test -s /tmp/out/hello.rtxc.jsonl
    echo "check: hello.tex typeset, $(wc -c < /tmp/out/hello.xdv) bytes of XDV, $(wc -l < /tmp/out/hello.rtxc.jsonl) capture records"'
}
package() {
  # the release assets, with their checksums: the engine, the format, the tree as one tarball
  in_node bash -c 'set -e; R=reflow/dist/release; rm -rf $R; mkdir -p $R
    cp reflow/dist/async/tectonic.mjs reflow/dist/async/tectonic_wasm.wasm $R/
    cp reflow/dist/async/STAMP $R/STAMP.txt
    gzip -9 -n -c reflow/dist/latex.fmt > $R/latex.fmt.gz
    tar -C reflow/dist/tree --sort=name --owner=0 --group=0 --numeric-owner --mtime=2023-03-01 -czf $R/texlive-tree.tar.gz texlive
    (cd $R && sha256sum * > SHA256SUMS)
    ls -la $R'
}

case "${1:-}" in
  image) image ;;
  deps) deps ;;
  wasm) wasm ;;
  format) format ;;
  tree) shift; tree "$@" ;;
  check) check ;;
  package) package ;;
  all) image; deps; wasm; format; tree; check; package ;;
  *) sed -n '3,19p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
