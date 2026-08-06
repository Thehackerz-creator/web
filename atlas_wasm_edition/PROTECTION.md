# Protecting the ATLAS Compiler Idea

Browser code cannot be fully protected. If the complete compiler ships as
JavaScript or WASM, a determined user can copy it. The protected architecture is
therefore:

1. Public website ships only UI and a limited Community demo WASM.
2. Premium compilation runs on your server.
3. Premium artifacts are returned only after auth, rate limits, and logging.
4. The strongest compiler source, vendor exporters, signing, and advanced safety
   evidence stay out of the browser bundle.

## What Stays Public

- `web/index.html`, `web/assets/style.css`, and `web/assets/app.js`
- Community demo WASM built from `wasm/atlas_wasm_bridge.c`
- Small examples and docs
- Clear Community limits

## What Stays Private

- Production compiler binary
- Premium graph export and bundle generation
- License validation
- Customer project storage
- Deterministic build signing
- Any advanced algorithms you consider core IP

## Deployment Shape

```
Browser
  - UI
  - limited demo WASM
  - no premium secrets

Premium API
  - checks token/session
  - calls native compiler in a temp sandbox
  - returns ST, safety report, diagnostics, graph

Private Build Server
  - stores compiler source
  - builds release binaries
  - signs artifacts
```

## Practical Protections

- Do not put premium flags or billing decisions in client-only code.
- Do not embed private keys, license secrets, or permanent tokens in WASM.
- Add rate limits and request size limits to the API.
- Compile in short-lived temp directories.
- Run the API as an unprivileged user or container.
- Log account id, source hash, output hash, and compiler version.
- Ship minified UI for polish, but do not treat minification as security.
- Use a commercial license and visible copyright notices.

## Current Implementation

- `wasm/atlas_wasm_bridge.c` exports `atlas_compile_demo()` only for Community
  browser compilation.
- `web/assets/app.js` calls `/api/premium/compile` for Premium mode.
- `server/premium_api_example.js` shows the protected server-side path.
- `.gitignore` excludes generated WASM artifacts so release binaries are not
  accidentally committed.

## Launch Checklist

- Set `ATLAS_PREMIUM_TOKEN` or replace bearer tokens with real auth.
- Set `ATLAS_COMPILER` to the private native compiler binary.
- Serve the web UI from HTTPS.
- Put the API behind HTTPS, auth, and rate limiting.
- Keep source repositories private.
- Register copyright/trademark where appropriate.
