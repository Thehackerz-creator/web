const http = require("http");
const fs = require("fs");
const path = require("path");

const PORT = Number(process.env.PORT || 8080);
const STATIC_DIR = path.join(__dirname, "web");

const MIME_TYPES = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".wasm": "application/wasm",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".jpeg": "image/jpeg",
  ".ico": "image/x-icon"
};

const jsPath = path.join(STATIC_DIR, "assets", "atlas_compiler.js");
const wasmPath = path.join(STATIC_DIR, "assets", "atlas_compiler.wasm");

let wasmModule = null;

/**
 * Initializes and executes compilation via the WebAssembly binary.
 */
async function compileWasm(source) {
  if (!wasmModule) {
    if (!fs.existsSync(jsPath) || !fs.existsSync(wasmPath)) {
      throw new Error(`Compiler assets missing. Ensure atlas_compiler.js and atlas_compiler.wasm exist under web/assets/`);
    }

    const wasmBuffer = fs.readFileSync(wasmPath);
    const scriptContent = fs.readFileSync(jsPath, "utf8");

    // Evaluate the Emscripten IIFE to capture the AtlasCompilerModule constructor
    const runInContext = new Function(scriptContent + "; return AtlasCompilerModule;");
    const AtlasCompilerModule = runInContext();

    wasmModule = await AtlasCompilerModule({
      wasmBinary: wasmBuffer,
      locateFile: () => wasmPath
    });
  }

  const compile = wasmModule.cwrap("atlas_compile_demo", "number", ["string"]);
  const freePtr = wasmModule.cwrap("atlas_free", null, ["number"]);

  const ptr = compile(source);
  const jsonStr = wasmModule.UTF8ToString(ptr);
  freePtr(ptr);

  return JSON.parse(jsonStr);
}

// Create HTTP Server
const server = http.createServer(async (req, res) => {
  // CORS Headers
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  res.setHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

  if (req.method === "OPTIONS") {
    res.writeHead(204);
    res.end();
    return;
  }

  const url = new URL(req.url, `http://${req.headers.host}`);
  const pathname = url.pathname;

  // API compiler endpoint
  if (req.method === "POST" && (pathname === "/api/premium/compile" || pathname === "/api/compile")) {
    let body = "";
    req.on("data", chunk => {
      body += chunk;
      if (body.length > 1024 * 1024) { // 1MB limit
        req.destroy();
      }
    });

    req.on("end", async () => {
      try {
        const payload = JSON.parse(body || "{}");
        const source = payload.source || "";

        if (!source.trim()) {
          res.writeHead(400, { "Content-Type": "application/json" });
          res.end(JSON.stringify({ ok: false, error: "source code required" }));
          return;
        }

        const result = await compileWasm(source);
        
        // Add additional server metadata if successfully compiled
        result.edition = "premium-wasm-server";
        
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify(result));
      } catch (err) {
        res.writeHead(500, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ ok: false, error: err.message }));
      }
    });
    return;
  }

  // Static Assets Server
  if (req.method === "GET") {
    let safePath = path.normalize(pathname).replace(/^(\.\.[\/\\])+/, "");
    let filePath = path.join(STATIC_DIR, safePath);

    // Directory index mapping (e.g. /playground/ -> /playground/index.html)
    try {
      const stats = fs.statSync(filePath);
      if (stats.isDirectory()) {
        filePath = path.join(filePath, "index.html");
      }
    } catch (e) {
      res.writeHead(404, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ ok: false, error: "not found" }));
      return;
    }

    const ext = path.extname(filePath).toLowerCase();
    const contentType = MIME_TYPES[ext] || "application/octet-stream";

    fs.readFile(filePath, (err, data) => {
      if (err) {
        res.writeHead(404, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ ok: false, error: "not found" }));
      } else {
        res.writeHead(200, { "Content-Type": contentType });
        res.end(data);
      }
    });
    return;
  }

  // Fallback 404
  res.writeHead(404, { "Content-Type": "application/json" });
  res.end(JSON.stringify({ ok: false, error: "not found" }));
});

// Start listening
server.listen(PORT, () => {
  console.log(`ATLAS Unified Web & API Server running at http://localhost:${PORT}`);
});
