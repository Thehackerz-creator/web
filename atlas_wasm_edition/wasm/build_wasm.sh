#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/web/assets"
mkdir -p "$OUT"

if ! command -v emcc >/dev/null 2>&1; then
  echo "emcc not found. Install Emscripten, then rerun this script." >&2
  echo "See: https://emscripten.org/docs/getting_started/downloads.html" >&2
  exit 127
fi

COMMON=(
  "$ROOT/src/cli/logger.c"
  "$ROOT/src/core/lexer.c"
  "$ROOT/src/core/parser.c"
  "$ROOT/src/core/symtable.c"
  "$ROOT/src/core/semantic.c"
  "$ROOT/src/core/compiler.c"
  "$ROOT/src/core/gc.c"
  "$ROOT/src/core/optimizer.c"
  "$ROOT/src/diagnostics/diagnostics.c"
  "$ROOT/src/diagnostics/logic_graph.c"
  "$ROOT/src/security/security.c"
  "$ROOT/src/safety/safety.c"
  "$ROOT/src/targets/codegen.c"
  "$ROOT/src/targets/export_plcopen.c"
  "$ROOT/src/targets/st_gen.c"
  "$ROOT/src/targets/translator.c"
  "$ROOT/wasm/atlas_wasm_bridge.c"
)

emcc "${COMMON[@]}" \
  -std=c99 -O3 \
  -I"$ROOT/src/core" \
  -I"$ROOT/src/cli" \
  -I"$ROOT/src/safety" \
  -I"$ROOT/src/targets" \
  -I"$ROOT/src/diagnostics" \
  -I"$ROOT/src/security" \
  -sMODULARIZE=1 \
  -sEXPORT_NAME=AtlasCompilerModule \
  -sALLOW_MEMORY_GROWTH=1 \
  -sENVIRONMENT=web,worker \
  -sEXPORTED_FUNCTIONS='["_atlas_compile_demo","_atlas_compile","_atlas_sha256","_atlas_free","_malloc","_free"]' \
  -sEXPORTED_RUNTIME_METHODS='["ccall","cwrap","UTF8ToString","stringToUTF8","lengthBytesUTF8","getValue"]' \
  -o "$OUT/atlas_compiler.js"

echo "WASM built:"
echo "  $OUT/atlas_compiler.js"
echo "  $OUT/atlas_compiler.wasm"
