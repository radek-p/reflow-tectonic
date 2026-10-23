#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Builds the engine to WebAssembly: reflow/driver against this repository's crates (Tectonic with the reflowtex
# patches). Runs inside the build image (reflow/build.sh image) with the repository mounted at /work; the C
# dependencies are in reflow/deps/prefix (reflow/build.sh deps), whose pkg-config files name that path.
#   reflow/dist/async/   -sASYNCIFY, ttw_js_block the async import (a warm snapshot per block)
#   VARIANTS="async plain" reflow/scripts/build-wasm.sh    (plain: no Asyncify, for comparisons)
# Each variant gets a STAMP: the commit it was built from (with "+" when the tree had changes), the wasm's sha256.
set -euo pipefail
R=/work/reflow
export PKG_CONFIG_PATH="$R/deps/prefix/lib/pkgconfig" PKG_CONFIG_ALLOW_CROSS=1
cd /work
git config --global --add safe.directory /work 2>/dev/null || true
COMMIT=$(git rev-parse HEAD)$(git diff --quiet HEAD -- . ':!reflow/dist' ':!reflow/deps/prefix' ':!reflow/deps/src' 2>/dev/null || echo +)
EXPORTS="_main,_malloc,_free,_ttw_add_file,_ttw_clear_files,_ttw_file_list,_ttw_file_data,_ttw_compile"
for V in ${VARIANTS:-async}; do
  LINK=(
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=${INITIAL_MEMORY:-448MB} -sMAXIMUM_MEMORY=2GB
    -sSTACK_SIZE=8MB
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createTectonic
    -sENVIRONMENT=web,worker,node -sEXIT_RUNTIME=0
    "-sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8,HEAPU32,stackSave,stackRestore"
    "-sEXPORTED_FUNCTIONS=$EXPORTS"
    "--js-library=$R/js/library.js"
  )
  case "$V" in
    async|async-*) LINK+=(-sASYNCIFY=1 "-sASYNCIFY_IMPORTS=['ttw_js_block']") ;;
  esac
  # async-names: function names kept (to read the pause's call stack for ASYNCIFY_ONLY)
  [ "$V" = async-names ] && LINK+=(--profiling-funcs)
  # the functions Asyncify instruments: only the call path of the warm pause (asyncify-only.txt); instrumenting
  # everything bloats TeX's largest functions, and Safari's optimizing wasm compiler then takes gigabytes
  if [ "$V" = async ] && [ -s "$R/scripts/asyncify-only.txt" ] && [ -z "${ASYNCIFY_ALL:-}" ]; then
    LINK+=("-sASYNCIFY_ONLY=@$R/scripts/asyncify-only.txt")
  fi
  RUSTFLAGS=""
  for a in "${LINK[@]}"; do RUSTFLAGS+=" -C link-arg=$a"; done
  export RUSTFLAGS CARGO_TARGET_DIR="/target/$V"
  (cd "$R/driver" && cargo build --release --target wasm32-unknown-emscripten)
  OUT="$CARGO_TARGET_DIR/wasm32-unknown-emscripten/release"
  mkdir -p "$R/dist/$V"
  cp "$OUT/tectonic_wasm.js" "$R/dist/$V/tectonic.mjs"
  cp "$OUT/tectonic_wasm.wasm" "$R/dist/$V/tectonic_wasm.wasm"
  WASM_SHA=$(sha256sum "$R/dist/$V/tectonic_wasm.wasm" | cut -d" " -f1)
  printf "commit %s\nwasm %s\n" "$COMMIT" "$WASM_SHA" > "$R/dist/$V/STAMP"
  echo "engine dist/$V: wasm sha256 $WASM_SHA, commit $COMMIT"
done
ls -la "$R"/dist/*
