# Build image for Tectonic -> wasm32-unknown-emscripten.
# emsdk 3.1.74 ships clang/LLVM 20; Rust 1.89 also uses LLVM 20, so the
# object files Rust emits match what emscripten's wasm-ld expects.
FROM emscripten/emsdk:3.1.74

RUN apt-get update && apt-get install -y --no-install-recommends pkg-config ninja-build \
    && rm -rf /var/lib/apt/lists/*

ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
ARG RUST_VERSION=1.89.0
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain ${RUST_VERSION} \
    && rustup target add wasm32-unknown-emscripten \
    && chmod -R a+rwX /opt/rustup /opt/cargo

# Emscripten ports used as Tectonic's C dependencies (built once into the image cache).
RUN embuilder build zlib libpng freetype icu && chmod -R a+rwX /emsdk/upstream/emscripten/cache
