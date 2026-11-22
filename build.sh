#!/bin/sh
# Builds csv.wasm from csv.c: clang's wasm32 backend (LLVM 22) and wasm-ld, SIMD128 and bulk memory on, no libc.
set -e
cd "$(dirname "$0")"
CLANG=${CLANG:-/usr/local/opt/llvm@22/bin/clang}
"$CLANG" --target=wasm32 -O3 -msimd128 -mbulk-memory -nostdlib \
  -fuse-ld="${WASM_LD:-wasm-ld}" -Wl,--no-entry -Wl,--export-memory -Wl,--strip-all -Wl,-z,stack-size=65536 \
  -o csv.wasm csv.c
