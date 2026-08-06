// Determine base URL for assets relative to this script
const _scriptUrl = (function(){
  try { return document.currentScript && document.currentScript.src; } catch(e) { return null; }
})();
const ASSET_BASE = _scriptUrl ? new URL(_scriptUrl, location.href).href.replace(/\/app\.js$/, '/') : 'assets/';

const sample = `VAR_INPUT\n  a1 : BOOL @ESTOP\n  temp : REAL IN celsius\nEND_VAR\n\nVAR_OUTPUT\n  b2 : BOOL @CRITICAL\n  fan : BOOL\nEND_VAR\n\nIF a1 = ON THEN b2 = OFF ELSE b2 = ON END\nIF temp >= 50 THEN fan = ON ELSE fan = OFF END`;

let premium = false;
let wasm = null;
let lastResult = null;
let premiumSession = localStorage.getItem("atlasPremiumSession") || "";

const el = {
  dsl: document.getElementById("dsl"),
  st: document.getElementById("st"),
  safety: document.getElementById("safety"),
  graph: document.getElementById("graph"),
  diagnostics: document.getElementById("diagnostics"),
  run: document.getElementById("runBtn"),
  sample: document.getElementById("sampleBtn"),
  premium: document.getElementById("premiumToggle"),
  engine: document.getElementById("engineMode"),
  downloadSt: document.getElementById("downloadStBtn"),
  downloadBundle: document.getElementById("downloadBundleBtn")
};

el.dsl.value = sample;

async function initWasm() {
  try {
    const atlasScript = ASSET_BASE + 'atlas_compiler.js';
    const res = await fetch(atlasScript, { method: "HEAD" });
    if (!res.ok) throw new Error("WASM glue missing");
    await new Promise((resolve, reject) => {
      const script = document.createElement("script");
      script.src = atlasScript;
      script.onload = resolve;
      script.onerror = reject;
      document.head.appendChild(script);
    });
    wasm = await window.AtlasCompilerModule();
    el.engine.textContent = "WASM";
  } catch (e) {
    wasm = null;
    el.engine.textContent = "JS fallback";
  }
}

function compileWithWasm(source) {
  const compile = wasm.cwrap("atlas_compile_demo", "number", ["string"]);
  const freePtr = wasm.cwrap("atlas_free", null, ["number"]);
  const ptr = compile(source);
  const json = wasm.UTF8ToString(ptr);
  freePtr(ptr);
  return JSON.parse(json);
}

async function compileWithPremiumApi(source) {
  const res = await fetch("/api/premium/compile", {
    method: "POST",
    headers: {
      "content-type": "application/json",
      "authorization": `Bearer ${premiumSession}`
    },
    body: JSON.stringify({ source, target: "codesys", graph: true, bundle: true })
  });
  if (!res.ok) {
    const text = await res.text();
    throw new Error(text || `Premium API failed with HTTP ${res.status}`);
  }
  return res.json();
}

function compileFallback(source) {
  const model = parseProgram(source);
  const safety = analyze(model);
  return {
    ok: true,
    edition: premium ? "premium" : "community",
    st: toST(model),
    safety: safety.map(i => `[${i.type.toUpperCase()}] ${i.text}`).join("\n"),
    diagnostics: JSON.stringify({
      engine: "fallback",
      variables: model.vars.length,
      rules: model.rules.length,
      community_limits: premium ? null : "80 lines / 12 KB"
    }, null, 2),
    graph: premium ? toDot(model) : "",
    model
  };
}

function parseProgram(src) {
  const vars = new Map();
  const rules = [];
  let section = "MEMORY";
  src.split(/\n/).forEach((line, idx) => {
    const raw = line.trim();
    if (!raw || raw.startsWith("#")) return;
    if (/^VAR_INPUT\b/i.test(raw)) { section = "INPUT"; return; }
    if (/^VAR_OUTPUT\b/i.test(raw)) { section = "OUTPUT"; return; }
    if (/^VAR\b/i.test(raw)) { section = "MEMORY"; return; }
    if (/^END_VAR\b/i.test(raw)) { section = "MEMORY"; return; }

    const rule = raw.match(/^IF\s+(.+?)\s+THEN\s+(.+?)(?:\s+ELSE\s+(.+?))?\s+END$/i);
    if (rule) {
      const actions = parseActions(rule[2]);
      const elseActions = parseActions(rule[3] || "");
      rules.push({ line: idx + 1, cond: rule[1], actions, elseActions });
      const condVar = (rule[1].match(/^([A-Za-z_][A-Za-z0-9_]*)/) || [])[1];
      if (condVar && !vars.has(condVar)) vars.set(condVar, { name: condVar, dir: "INPUT", type: "BOOL" });
      actions.concat(elseActions).forEach(a => {
        if (!vars.has(a.name)) vars.set(a.name, { name: a.name, dir: "OUTPUT", type: "BOOL" });
      });
      return;
    }

    const decl = raw.match(/^([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*([A-Za-z_][A-Za-z0-9_]*))?(?:\s+IN\s+([A-Za-z_][A-Za-z0-9_]*))?(.*)$/i);
    if (decl) {
      const ann = decl[4] || "";
      vars.set(decl[1], {
        name: decl[1],
        dir: section,
        type: decl[2] || "BOOL",
        unit: decl[3] || "",
        estop: /@ESTOP/i.test(ann),
        critical: /@CRITICAL|@SAFETY|@SIL[1-4]/i.test(ann)
      });
    }
  });
  return { vars: [...vars.values()], rules };
}

function parseActions(text) {
  return text.split(/\s*;\s*/).map(part => {
    const m = part.trim().match(/^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(ON|OFF|[A-Za-z_][A-Za-z0-9_]*|\d+(?:\.\d+)?)$/i);
    return m ? { name: m[1], value: m[2].toUpperCase() } : null;
  }).filter(Boolean);
}

function toST(model) {
  const decls = model.vars.map(v => `    ${v.name} : ${v.type || "BOOL"};`).join("\n");
  const logic = model.rules.map(r => {
    const body = r.actions.map(a => `    ${a.name} := ${lit(a.value)};`).join("\n");
    const elseBody = r.elseActions.length
      ? `\nELSE\n${r.elseActions.map(a => `    ${a.name} := ${lit(a.value)};`).join("\n")}`
      : "";
    return `IF ${cond(r.cond)} THEN\n${body}${elseBody}\nEND_IF;`;
  }).join("\n\n");
  return `PROGRAM PLC_PRG\nVAR\n${decls}\nEND_VAR\n\n${logic}\nEND_PROGRAM`;
}

function lit(v) { return v === "ON" ? "TRUE" : v === "OFF" ? "FALSE" : v; }
function cond(c) { return c.replace(/\bON\b/gi, "TRUE").replace(/\bOFF\b/gi, "FALSE"); }

function analyze(model) {
  const issues = [];
  const writes = new Map();
  model.rules.forEach(r => {
    if (!r.elseActions.length) issues.push({ type: "warn", text: `Line ${r.line}: missing ELSE safe state.` });
    r.actions.concat(r.elseActions).forEach(a => writes.set(a.name, (writes.get(a.name) || 0) + 1));
  });
  writes.forEach((count, name) => {
    if (count > 2) issues.push({ type: "bad", text: `${name} is written by ${count} paths.` });
  });
  const estopRules = model.rules.filter(r => {
    const first = (r.cond.match(/^([A-Za-z_][A-Za-z0-9_]*)/) || [])[1];
    const v = model.vars.find(x => x.name === first);
    return v && v.estop;
  });
  model.vars.filter(v => v.dir === "OUTPUT" && v.critical).forEach(v => {
    const covered = estopRules.some(r => r.actions.concat(r.elseActions).some(a => a.name === v.name && a.value === "OFF"));
    if (!covered) issues.push({ type: "bad", text: `${v.name} is critical but not forced OFF by @ESTOP logic.` });
  });
  if (!issues.length) issues.push({ type: "good", text: "No safety findings in live analysis." });
  return issues;
}

function toDot(model) {
  const lines = ["digraph AtlasLogic {", "  rankdir=LR;"];
  model.vars.forEach(v => lines.push(`  "${v.name}" [shape=note,label="${v.name}\\n${v.dir}"];`));
  model.rules.forEach((r, i) => {
    lines.push(`  "rule${i}" [shape=diamond,label="IF\\n${r.cond.replaceAll('"', "'")}"];`);
    const condVar = (r.cond.match(/^([A-Za-z_][A-Za-z0-9_]*)/) || [])[1];
    if (condVar) lines.push(`  "${condVar}" -> "rule${i}" [label="guards"];`);
    r.actions.concat(r.elseActions).forEach(a => lines.push(`  "rule${i}" -> "${a.name}" [label="${a.value}"];`));
  });
  lines.push("}");
  return lines.join("\n");
}

function renderGraph(result) {
  if (result.graph && result.graph.trim()) {
    el.graph.innerHTML = `<div class="graph-header">Graphviz DOT Analysis</div><pre class="dot-source">${escapeHtml(result.graph)}</pre><div class="graph-hint">Copy this source into a Graphviz visualizer for full interactive analysis.</div>`;
    return;
  }
  const model = result.model || parseProgram(el.dsl.value);
  const vars = model.vars.map((v, i) => `<g><rect x="24" y="${32 + i * 64}" width="180" height="44" rx="7" fill="${v.critical || v.estop ? "#fef3c7" : "#f8fafc"}"/><text x="38" y="${58 + i * 64}">${escapeHtml(v.name)} ${escapeHtml(v.dir)}</text></g>`).join("");
  const rules = model.rules.map((r, i) => `<g><polygon points="360,${44 + i * 92} 430,${76 + i * 92} 360,${108 + i * 92} 290,${76 + i * 92}" fill="#e0f2fe" stroke="#334155"/><text x="360" y="${80 + i * 92}" text-anchor="middle">IF ${escapeHtml(String(r.line))}</text></g>`).join("");
  el.graph.innerHTML = `<svg viewBox="0 0 920 560">${vars}${rules}</svg>`;
}

function renderSafety(text) {
  const rows = String(text || "").split("\n").filter(Boolean);
  el.safety.innerHTML = rows.map(row => {
    const cls = row.includes("[BAD]") || row.includes("CRIT") || row.includes("FATAL") ? "bad" : row.includes("[GOOD]") ? "good" : "";
    return `<div class="finding ${cls}">${escapeHtml(row)}</div>`;
  }).join("") || `<div class="finding good">No findings.</div>`;
}

async function run() {
  try {
    if (premium && premiumSession) {
      lastResult = await compileWithPremiumApi(el.dsl.value);
    } else {
      lastResult = wasm ? compileWithWasm(el.dsl.value) : compileFallback(el.dsl.value);
    }
  } catch (err) {
    lastResult = compileFallback(el.dsl.value);
    lastResult.error = premium
      ? `Premium API unavailable: ${err.message}`
      : `WASM call failed, fallback used: ${err.message}`;
  }
  el.st.textContent = lastResult.st || "";
  el.diagnostics.textContent = lastResult.diagnostics || lastResult.error || "";
  renderSafety(lastResult.safety || lastResult.error || "");
  renderGraph(lastResult);
  updatePremiumUi();
}

function download(name, text, type = "text/plain") {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([text], { type }));
  a.download = name;
  a.click();
  URL.revokeObjectURL(a.href);
}

function updatePremiumUi() {
  el.premium.textContent = premium ? "Premium API" : "Community Demo";
  el.downloadBundle.classList.toggle("locked", !premium);
  el.downloadBundle.title = premium ? "Download server-generated ST, reports, and graph" : "Premium unlock: server-side bundle export and full graph DOT";
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

document.querySelectorAll(".tab").forEach(btn => btn.addEventListener("click", () => {
  document.querySelectorAll(".tab").forEach(b => b.classList.toggle("active", b === btn));
  document.querySelectorAll(".pane").forEach(p => p.classList.toggle("active", p.id === btn.dataset.pane));
}));

el.run.addEventListener("click", run);
el.sample.addEventListener("click", async () => {
  // Load examples/sample.dsl from the examples directory (relative to assets)
  try {
    const resp = await fetch(ASSET_BASE + '../examples/sample.dsl');
    if (resp.ok) {
      const txt = await resp.text();
      el.dsl.value = txt;
    } else {
      el.dsl.value = sample;
    }
  } catch (e) {
    el.dsl.value = sample;
  }
  run();
});
el.premium.addEventListener("click", () => {
  if (!premiumSession) {
    const token = prompt("Enter premium session token from your account dashboard.");
    if (token) {
      premiumSession = token.trim();
      localStorage.setItem("atlasPremiumSession", premiumSession);
      premium = true;
    }
  } else {
    premium = !premium;
  }
  run();
});
el.dsl.addEventListener("input", () => run());
el.downloadSt.addEventListener("click", () => download("atlas_output.st", lastResult?.st || ""));
el.downloadBundle.addEventListener("click", () => {
  if (!premium) { alert("Bundle export is a Premium feature. Community export still supports ST download."); return; }
  const bundle = [
    "=== atlas_output.st ===\n" + (lastResult?.st || ""),
    "=== safety.txt ===\n" + (lastResult?.safety || ""),
    "=== diagnostics.json ===\n" + (lastResult?.diagnostics || ""),
    "=== logic.dot ===\n" + (lastResult?.graph || "")
  ].join("\n\n");
  download("atlas_bundle.txt", bundle);
});

initWasm().then(run);
