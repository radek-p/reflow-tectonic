#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Build the C libraries Tectonic's bridge crates want and describe them with
# pkg-config files, all for wasm32-emscripten. Runs inside the wasm build image.
#  - zlib, libpng, freetype, ICU common: emscripten ports
#  - ICU data: a filtered ICU 68 package (line breaking, converter aliases)
#  - graphite2 1.3.14: compiled from source here
#  - fontconfig: an empty stub (deps/fontconfig-stub), no fonts in a browser
#  - harfbuzz: Tectonic vendors it (tectonic_bridge_harfbuzz), nothing to do
set -euo pipefail
cd "$(dirname "$0")"
DEPS="$PWD"
PREFIX="$DEPS/prefix"
SYSROOT="$(em-config CACHE)/sysroot"
mkdir -p "$PREFIX/lib/pkgconfig" "$PREFIX/include" src

# graphite2
G2=graphite2-1.3.14
if [ ! -f "$PREFIX/lib/libgraphite2.a" ]; then
  [ -d "src/$G2" ] || curl -sSL "https://github.com/silnrsi/graphite/releases/download/1.3.14/$G2.tgz" | tar -xz -C src
  rm -rf src/g2-obj && mkdir -p src/g2-obj
  for f in src/$G2/src/*.cpp; do
    case "$(basename "$f")" in direct_machine.cpp) continue ;; esac
    em++ -O2 -std=c++11 -fno-rtti -fno-exceptions -DGRAPHITE2_STATIC -DGRAPHITE2_NTRACING \
      -I"src/$G2/include" -I"src/$G2/src" -c "$f" -o "src/g2-obj/$(basename "$f" .cpp).o"
  done
  emar rcs "$PREFIX/lib/libgraphite2.a" src/g2-obj/*.o
  cp -r "src/$G2/include/graphite2" "$PREFIX/include/"
fi

# ICU data (made by reflow/build.sh deps from the full ICU 68 package)
emcc -O2 -c src/icudata.c -o src/icudata.o
emar rcs "$PREFIX/lib/libicudata_tectonic.a" src/icudata.o

# fontconfig stub
emcc -O2 -I fontconfig-stub -c fontconfig-stub/fontconfig.c -o src/fontconfig.o
emar rcs "$PREFIX/lib/libfontconfig.a" src/fontconfig.o
rm -rf "$PREFIX/include/fontconfig" && cp -r fontconfig-stub/fontconfig "$PREFIX/include/"

pc() { # name version cflags libs
  cat > "$PREFIX/lib/pkgconfig/$1.pc" <<PC
Name: $1
Description: $1 for wasm32-emscripten (reflow-tectonic)
Version: $2
Cflags: $3
Libs: $4
PC
}
pc graphite2 1.3.14 "-I$PREFIX/include -DGRAPHITE2_STATIC" "-L$PREFIX/lib -lgraphite2"
pc fontconfig 2.15.0 "-I$PREFIX/include" "-L$PREFIX/lib -lfontconfig"
pc freetype2 26.2.20 "-I$SYSROOT/include/freetype2" "-L$SYSROOT/lib/wasm32-emscripten -lfreetype -lz"
pc libpng 1.6.39 "-I$SYSROOT/include" "-L$SYSROOT/lib/wasm32-emscripten -lpng -lz"
pc zlib 1.3.1 "-I$SYSROOT/include" "-L$SYSROOT/lib/wasm32-emscripten -lz"
pc icu-uc 68.2 "-I$SYSROOT/include" "-L$SYSROOT/lib/wasm32-emscripten -licu_common -L$PREFIX/lib -licudata_tectonic"
ls -la "$PREFIX/lib" "$PREFIX/lib/pkgconfig"
