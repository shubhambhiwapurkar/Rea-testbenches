#!/usr/bin/env bash
# Compiles the simulation core plus the bridge to Web/assets/summit_sim.wasm and regenerates
# Web/js/simLayout.js from the IO_FIELDS list in SummitBridge.cpp.
#
# Needs clang with the wasm32 target, wasm-ld, and the wasi-sdk sysroot
# (https://github.com/WebAssembly/wasi-sdk/releases). Point WASI_SDK at the unpacked folder:
#   WASI_SDK=/opt/wasi/wasi-sdk-24.0-x86_64-linux Web/bridge/build_wasm.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WASI_SDK="${WASI_SDK:-/opt/wasi/wasi-sdk-24.0-x86_64-linux}"
CXX="${CXX:-clang++}"
OUT="$ROOT/Web/assets/summit_sim.wasm"
BUILTINS="$(find "$WASI_SDK/lib/clang" -name 'libclang_rt.builtins-wasm32.a' | head -1)"

"$CXX" --target=wasm32-wasi --sysroot="$WASI_SDK/share/wasi-sysroot" \
  -std=c++17 -O2 -fno-exceptions -fno-rtti -Wall -Wextra \
  -mexec-model=reactor -nodefaultlibs -Wl,--strip-all \
  -I"$ROOT/Source/SummitSim/Public" \
  "$ROOT"/Source/SummitSim/Private/Sim/*.cpp "$ROOT/Web/bridge/SummitBridge.cpp" \
  -lc++ -lc++abi -lc "$BUILTINS" \
  -o "$OUT"

python3 "$ROOT/Web/bridge/gen_layout.py" "$ROOT"
ls -la "$OUT"
