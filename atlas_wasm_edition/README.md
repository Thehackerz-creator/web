# ATLAS WASM Edition

This is a browser-ready clone of the PLC compiler. It keeps the native C core
and adds:

- `wasm/atlas_wasm_bridge.c` - exported browser API
- `wasm/build_wasm.sh` - Emscripten build script
- `web/index.html` - professional playground/site UI
- `web/assets/app.js` - WASM loader with JS fallback
- `server/premium_api_example.js` - server-only premium compile API
- `PROTECTION.md` - IP protection and deployment checklist

## Build

Install Emscripten first, then:

```bash
cd atlas_wasm_edition
./wasm/build_wasm.sh
python3 -m http.server 8080 -d web
```

Open `http://localhost:8080`.

## Editions

Community mode is intentionally transparent and limited:

- ST download is enabled.
- Programs are limited to 80 lines / 12 KB in the WASM API.
- The browser WASM calls `atlas_compile_demo()` only.
- Bundle export and full DOT graph export are Premium server controls.

Premium mode must run on your server, not in browser WASM:

```bash
cd atlas_wasm_edition
ATLAS_PREMIUM_TOKEN=dev-secret \
ATLAS_COMPILER=/home/devansh/Downloads/plc_compiler_v2_final/plc_compiler_debug \
node server/premium_api_example.js
```

The web UI sends Premium requests to `/api/premium/compile` with a bearer token.
Replace the example token check with your real auth/billing system before
hosting publicly.

## Browser API

```js
const mod = await AtlasCompilerModule();
const compile = mod.cwrap("atlas_compile_demo", "number", ["string"]);
const freePtr = mod.cwrap("atlas_free", null, ["number"]);

const ptr = compile(sourceDsl);
const result = JSON.parse(mod.UTF8ToString(ptr));
freePtr(ptr);
```

## Protection Note

Do not ship the full premium compiler as WASM. Browser assets can always be
copied. Keep core IP and premium export logic behind the server API described in
`PROTECTION.md`.
