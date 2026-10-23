// SPDX-License-Identifier: AGPL-3.0-or-later
// Runs the wasm engine under Node (the .wasm the browser uses): reflow/build.sh format builds latex.fmt with it.
//   node reflow/node/tectonic-node.ts format            -> reflow/dist/latex.fmt
//   node reflow/node/tectonic-node.ts compile doc.tex [--pdf] [--out dir]
// Support files come from bundle.ts's disk cache (TECTONIC_CACHE); misses are fetched
// synchronously through a child process (fetch-one.ts).
import { readFileSync, writeFileSync, existsSync, mkdirSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { join, dirname, basename } from "node:path";
import { fileURLToPath } from "node:url";
import { getIndex, resolveTarUrl, getFileCachedSync } from "./bundle.ts";
import { Tectonic } from "./tectonic-api.js";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const createModule = (await import(join(ROOT, "dist/async/tectonic.mjs"))).default;
const index = await getIndex();
await resolveTarUrl();

const stats = { fetched: 0, bytes: 0, misses: 0, names: [] as string[] };
function fetchSync(kind: number, name: string): Uint8Array | null {
  if (kind === 1) {
    const f = join(ROOT, "dist", name);
    return existsSync(f) ? readFileSync(f) : null;
  }
  if (!index.has(name)) { stats.misses++; return null; }
  const data = getFileCachedSync(name) ??
    execFileSync(process.execPath, [join(ROOT, "node/fetch-one.ts"), name], { maxBuffer: 1 << 30 });
  stats.fetched++; stats.bytes += data.length; stats.names.push(name);
  return data;
}

const tt = await Tectonic.create({ createModule, fetchSync,
  log: (lvl: number, s: string) => console.error(["note", "warning", "error"][lvl] + ": " + s) });
const [cmd, arg] = process.argv.slice(2);
const t0 = performance.now();
if (cmd === "format") {
  const r = tt.compile("tectonic-format-latex.tex", { initex: true, format: "latex.fmt", buildDate: 1700000000 });
  const fmt = tt.file("latex.fmt");
  console.error(`status ${r}, fmt ${fmt?.length} bytes`);
  if (!fmt) { process.stderr.write(Buffer.from(tt.file("") ?? [])); process.exit(1); }
  mkdirSync(join(ROOT, "dist"), { recursive: true });
  writeFileSync(join(ROOT, "dist/latex.fmt"), fmt);
} else if (cmd === "compile") {
  const out = process.argv.includes("--out") ? process.argv[process.argv.indexOf("--out") + 1] : "out";
  tt.addFile(basename(arg), readFileSync(arg));
  const r = tt.compile(basename(arg), { pdf: process.argv.includes("--pdf") });
  mkdirSync(out, { recursive: true });
  for (const n of tt.files()) writeFileSync(join(out, n), tt.file(n)!);
  writeFileSync(join(out, "terminal.txt"), tt.file("") ?? new Uint8Array());
  console.error(`status ${r}; outputs: ${tt.files().join(" ")}`);
} else {
  console.error("usage: format | compile doc.tex [--pdf] [--out dir]"); process.exit(2);
}
console.error(`wasm memory ${(tt.M.HEAPU8.length / 1e6).toFixed(0)} MB`);
console.error(`time ${(performance.now() - t0).toFixed(0)} ms; fetched ${stats.fetched} files, ` +
  `${(stats.bytes / 1e6).toFixed(2)} MB; ${stats.misses} lookups not in bundle`);
