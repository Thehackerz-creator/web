#!/usr/bin/env node
/**
 * Premium API example.
 *
 * Keep this on your server. Do not ship this compiler path to the browser.
 * It wraps the native compiler binary and returns production artifacts only
 * after a server-side bearer token check.
 */

const http = require("http");
const { mkdtemp, readFile, writeFile, rm } = require("fs/promises");
const { spawn } = require("child_process");
const { tmpdir } = require("os");
const { join, resolve } = require("path");

const PORT = Number(process.env.PORT || 8787);
const PREMIUM_TOKEN = process.env.ATLAS_PREMIUM_TOKEN || "";
const COMPILER = resolve(process.env.ATLAS_COMPILER || "../plc_compiler_debug");
const ALLOWED_ORIGIN = process.env.ATLAS_ALLOWED_ORIGIN || "http://localhost:8080";

function send(res, status, body, type = "application/json") {
  res.writeHead(status, {
    "content-type": type,
    "cache-control": "no-store",
    "x-content-type-options": "nosniff",
    "access-control-allow-origin": ALLOWED_ORIGIN,
    "access-control-allow-headers": "content-type, authorization"
  });
  res.end(type === "application/json" ? JSON.stringify(body) : body);
}

function readJson(req) {
  return new Promise((resolveRead, reject) => {
    let body = "";
    req.on("data", chunk => {
      body += chunk;
      if (body.length > 512 * 1024) {
        req.destroy();
        reject(new Error("request too large"));
      }
    });
    req.on("end", () => {
      try { resolveRead(JSON.parse(body || "{}")); }
      catch { reject(new Error("invalid json")); }
    });
    req.on("error", reject);
  });
}

function runCompiler(args) {
  return new Promise((resolveRun, reject) => {
    const child = spawn(COMPILER, args, { stdio: ["ignore", "pipe", "pipe"] });
    let stdout = "";
    let stderr = "";
    const timer = setTimeout(() => {
      child.kill("SIGKILL");
      reject(new Error("compile timeout"));
    }, 10000);
    child.stdout.on("data", d => stdout += d);
    child.stderr.on("data", d => stderr += d);
    child.on("error", reject);
    child.on("close", code => {
      clearTimeout(timer);
      resolveRun({ code, stdout, stderr });
    });
  });
}

function authorized(req) {
  if (!PREMIUM_TOKEN) return false;
  const value = req.headers.authorization || "";
  return value === `Bearer ${PREMIUM_TOKEN}`;
}

const server = http.createServer(async (req, res) => {
  if (req.method === "OPTIONS") {
    res.writeHead(204, {
      "access-control-allow-origin": ALLOWED_ORIGIN,
      "access-control-allow-methods": "POST, OPTIONS",
      "access-control-allow-headers": "content-type, authorization"
    });
    res.end();
    return;
  }

  if (req.method !== "POST" || req.url !== "/api/premium/compile") {
    send(res, 404, { ok: false, error: "not found" });
    return;
  }

  if (!authorized(req)) {
    send(res, 401, { ok: false, error: "premium token required" });
    return;
  }

  let dir;
  try {
    const payload = await readJson(req);
    const source = String(payload.source || "");
    if (!source.trim()) throw new Error("source required");
    if (source.length > 512 * 1024) throw new Error("source too large");

    dir = await mkdtemp(join(tmpdir(), "atlas-premium-"));
    const input = join(dir, "input.dsl");
    const st = join(dir, "output.st");
    const safety = join(dir, "safety.txt");
    const diagnostics = join(dir, "diagnostics.json");
    const graph = join(dir, "logic.dot");
    await writeFile(input, source, "utf8");

    const result = await runCompiler([
      input, st,
      "--target", "codesys",
      "--deterministic",
      "--safety",
      "--diagnostics",
      "--diagnostics-format", "json",
      "--safety-report", safety,
      "--diagnostics-report", diagnostics,
      "--graph", graph,
      "--quiet",
      "--no-print-output"
    ]);

    const response = {
      ok: result.code === 0,
      edition: "premium-server",
      st: await readFile(st, "utf8").catch(() => ""),
      safety: await readFile(safety, "utf8").catch(() => ""),
      diagnostics: await readFile(diagnostics, "utf8").catch(() => ""),
      graph: await readFile(graph, "utf8").catch(() => ""),
      error: result.code === 0 ? "" : (result.stderr || result.stdout || "compile failed")
    };
    send(res, result.code === 0 ? 200 : 422, response);
  } catch (err) {
    send(res, 400, { ok: false, error: err.message });
  } finally {
    if (dir) await rm(dir, { recursive: true, force: true }).catch(() => {});
  }
});

server.listen(PORT, () => {
  console.log(`ATLAS premium API listening on http://localhost:${PORT}`);
});
