// SPDX-License-Identifier: AGPL-3.0-or-later
// Access to Tectonic's default ITAR bundle (the "indexed tar" that native
// Tectonic reads with HTTP range requests), with an on-disk cache.
//
// The bundle is one 2.9 GB tar on a CDN plus a gzipped index of
// "name offset length" lines. The CDN sends no CORS headers (and answers
// preflights with 403), so a browser cannot read it directly; this module
// is used by the dev server (serve.ts), which does the range requests for it.
//
// The disk cache: the tar is read in 1 MiB chunks and every chunk fetched is
// kept on disk, so the CDN is asked once per chunk and dev work runs offline after the first fetch.
//   where     $TECTONIC_CACHE, else the workspace's shared cache: <workspace>/.cache/tectonic-bundle, where
//             Outside every checkout, so all checkouts of the repository share one copy; it is in no git
//             repository, so nothing can commit it. Without a git checkout (a copied tree): cache/ next to this file, as before.
//   layout    <cache>/tar-url-<relay name>.txt         the relay's redirect (the bundle's version)
//             <cache>/<tar name>/index.gz              the index as published
//             <cache>/<tar name>/size.txt              the tar's length (from Content-Range)
//             <cache>/<tar name>/chunks/<CHUNK>-<n>    chunk n, and <…>.sha256 its SHA-256 (hex)
//   writes    temp file (pid + random) and rename: servers in several checkouts can fill it at once
//   checks    a chunk's length (CHUNK, the last one shorter) and its SHA-256 (written when it was fetched; a chunk
//             without one, from before, has its length checked and gets one). A bad chunk is deleted and fetched again.
//   log       logCacheTo(fn): each miss and each bad chunk, and a summary of hits/misses after a burst of reads.

import { mkdirSync, existsSync, readFileSync, writeFileSync, renameSync, unlinkSync, statSync } from "node:fs";
import { createHash, randomBytes } from "node:crypto";
import { gunzipSync } from "node:zlib";
import { join, dirname, basename, resolve } from "node:path";
import { fileURLToPath } from "node:url";

export const RELAY_URL =
  process.env.TECTONIC_BUNDLE ?? "https://relay.fullyjustified.net/default_bundle_v33.tar";
const HERE = dirname(fileURLToPath(import.meta.url));

/** The workspace's shared cache directory, or null without a git checkout above this file. */
export function sharedCacheDir(from = HERE): string | null {
  for (let d = resolve(from); ; d = dirname(d)) {
    const g = join(d, ".git");
    if (existsSync(g)) {
      let common = g;                                            // the main checkout's .git
      if (statSync(g).isFile()) {
        const m = /^gitdir:\s*(.+)$/m.exec(readFileSync(g, "utf8"));
        if (!m) return null;
        const p = resolve(d, m[1].trim());
        const i = p.lastIndexOf("/.git/");
        if (i < 0) return null;
        common = p.slice(0, i + 5);
      }
      return join(dirname(dirname(common)), ".cache", "tectonic-bundle");
    }
    if (dirname(d) === d) return null;
  }
}
export const CACHE = process.env.TECTONIC_CACHE ?? sharedCacheDir() ?? join(HERE, "cache");

export interface Entry { offset: number; length: number }

let tarUrl: string | undefined;
let index: Map<string, Entry> | undefined;
let indexGz: Buffer | undefined;

// ── the log ──────────────────────────────────────────────────────────────────
export const cacheStats = { hits: 0, misses: 0, bad: 0, bytesFetched: 0 };
/** kept for older callers: chunks fetched from the CDN and their bytes */
export const chunkStats = { fetched: 0, bytes: 0 };
let log: ((line: string) => void) | null = null;
let summaryTimer: ReturnType<typeof setTimeout> | null = null;
let last = { hits: 0, misses: 0, bad: 0 };
/** Where the cache reports: each miss and each bad chunk, and after a burst of reads (1 s quiet) the counts. */
export function logCacheTo(fn: ((line: string) => void) | null): void { log = fn; }
function note(line: string | null): void {
  if (!log) return;
  if (line) log(`bundle cache: ${line}`);
  if (summaryTimer) clearTimeout(summaryTimer);
  summaryTimer = setTimeout(() => {
    summaryTimer = null;
    const s = cacheStats, d = { hits: s.hits - last.hits, misses: s.misses - last.misses, bad: s.bad - last.bad };
    last = { hits: s.hits, misses: s.misses, bad: s.bad };
    if (d.hits || d.misses || d.bad) log?.(`bundle cache: ${d.hits} hits, ${d.misses} misses${d.bad ? `, ${d.bad} bad` : ""} (total ${s.hits} hits, ${s.misses} misses, ${(s.bytesFetched / 1048576).toFixed(1)} MB fetched; ${CACHE})`);
  }, 1000);
  summaryTimer.unref?.();
}

// ── files on disk ────────────────────────────────────────────────────────────
/** Write a file whole or not at all: a temp file of this process, then rename (atomic on one file system). */
export function writeAtomic(f: string, data: Buffer | string): void {
  mkdirSync(dirname(f), { recursive: true });
  const tmp = `${f}.${process.pid}.${randomBytes(4).toString("hex")}.tmp`;
  writeFileSync(tmp, data);
  renameSync(tmp, f);
}
const readOr = (f: string): Buffer | null => { try { return readFileSync(f); } catch { return null; } };
const sha256 = (b: Buffer) => createHash("sha256").update(b).digest("hex");
const remove = (f: string) => { try { unlinkSync(f); } catch { /* gone already */ } };

/** Resolve the relay's redirect to the actual tar URL (cached on disk, per relay URL). */
export async function resolveTarUrl(): Promise<string> {
  if (tarUrl) return tarUrl;
  const f = join(CACHE, `tar-url-${basename(RELAY_URL).replace(/[^A-Za-z0-9._-]/g, "_")}.txt`);
  // (a cache from before 2026-10-12 has one tar-url.txt: the default relay's)
  const known = (readOr(f) ?? (!process.env.TECTONIC_BUNDLE ? readOr(join(CACHE, "tar-url.txt")) : null))?.toString("utf8").trim();
  if (known) { if (!existsSync(f)) writeAtomic(f, known + "\n"); return (tarUrl = known); }
  const r = await fetch(RELAY_URL, { method: "HEAD", redirect: "manual" });
  const loc = r.headers.get("location");
  tarUrl = loc ?? RELAY_URL;
  writeAtomic(f, tarUrl + "\n");
  return tarUrl;
}

function bundleDir(url: string): string {
  return join(CACHE, url.split("/").pop()!.replace(/[^A-Za-z0-9._-]/g, "_"));
}

/** The gzipped index exactly as published next to the tar (fetched again when the copy on disk does not unzip). */
export async function getIndexGz(): Promise<Buffer> {
  if (indexGz) return indexGz;
  const url = await resolveTarUrl();
  const f = join(bundleDir(url), "index.gz");
  const have = readOr(f);
  if (have) {
    try { gunzipSync(have); return (indexGz = have); } catch { cacheStats.bad++; note(`bad index ${f}: fetched again`); remove(f); }
  }
  const r = await fetch(url + ".index.gz");
  if (!r.ok) throw new Error(`index fetch failed: ${r.status}`);
  const data = Buffer.from(await r.arrayBuffer());
  gunzipSync(data);                                              // throws on a broken download: nothing is kept
  writeAtomic(f, data);
  return (indexGz = data);
}

let indexEnd = 0;                                                // the end of the last member: the tar is at least this long
export async function getIndex(): Promise<Map<string, Entry>> {
  if (index) return index;
  const text = gunzipSync(await getIndexGz()).toString("utf8");
  const m = new Map<string, Entry>();
  for (const line of text.split("\n")) {
    const [name, off, len] = line.split(" ");
    if (name && len !== undefined) {
      const e = { offset: Number(off), length: Number(len) };
      m.set(name, e);
      indexEnd = Math.max(indexEnd, e.offset + e.length);
    }
  }
  return (index = m);
}

export interface Fetched { data: Buffer; fromCache: boolean }

// The tar is read in aligned chunks, not file by file. Its members are in
// name order, so a package's files (pgf*.code.tex, tikz*.tex, ...) sit next
// to each other: one chunk request brings in many of the files TeX will ask
// for next. On a cold cache this turns ~0.3 s per file into ~0.3 s per chunk.
const CHUNK = Number(process.env.TECTONIC_CHUNK ?? 1 << 20);
const inflight = new Map<number, Promise<Buffer>>();
const checked = new Set<string>();                               // chunks whose hash this process has checked

function tarSize(dir: string): number | null {
  const n = Number(readOr(join(dir, "size.txt"))?.toString("utf8").trim());
  return n > 0 ? n : null;
}
/** Is this chunk's length right? CHUNK, or shorter only for the tar's last chunk. */
function lengthOk(dir: string, n: number, len: number): boolean {
  if (len === CHUNK) return true;
  const size = tarSize(dir);
  if (size !== null) return len === Math.min(CHUNK, size - n * CHUNK);
  return len > 0 && len < CHUNK && n * CHUNK + len >= indexEnd;  // no size known (an older cache): it holds the last member
}
/** The chunk from disk when it is whole and unchanged; a bad one is removed (null). */
function readChunk(dir: string, n: number): Buffer | null {
  const f = join(dir, "chunks", `${CHUNK}-${n}`);
  const data = readOr(f);
  if (!data) return null;
  if (checked.has(f)) return data;
  const want = readOr(`${f}.sha256`)?.toString("utf8").trim();
  const problem = !lengthOk(dir, n, data.length) ? `length ${data.length}` : want && want !== sha256(data) ? "SHA-256 differs" : "";
  if (problem) {
    cacheStats.bad++;
    note(`bad chunk ${n} (${problem}): fetched again`);
    remove(f); remove(`${f}.sha256`);
    return null;
  }
  if (!want) writeAtomic(`${f}.sha256`, sha256(data) + "\n");   // a chunk from before the hashes: its length was right
  checked.add(f);
  return data;
}

async function getChunk(n: number): Promise<{ data: Buffer; fromCache: boolean }> {
  const url = await resolveTarUrl();
  const dir = bundleDir(url);
  const have = readChunk(dir, n);
  if (have) { cacheStats.hits++; note(null); return { data: have, fromCache: true }; }
  let p = inflight.get(n);
  if (!p) {
    p = (async () => {
      const t0 = Date.now();
      for (let attempt = 1; ; attempt++) {
        try {
          const r = await fetch(url, { headers: { Range: `bytes=${n * CHUNK}-${(n + 1) * CHUNK - 1}` } });
          if (r.status !== 206) throw new Error(`range request got HTTP ${r.status}`);
          const data = Buffer.from(await r.arrayBuffer());
          const total = Number(/\/(\d+)\s*$/.exec(r.headers.get("content-range") ?? "")?.[1]);
          if (total > 0 && tarSize(dir) !== total) writeAtomic(join(dir, "size.txt"), `${total}\n`);
          if (!lengthOk(dir, n, data.length)) throw new Error(`chunk ${n}: ${data.length} bytes`);
          const f = join(dir, "chunks", `${CHUNK}-${n}`);
          writeAtomic(f, data);
          writeAtomic(`${f}.sha256`, sha256(data) + "\n");
          checked.add(f);
          chunkStats.fetched++; chunkStats.bytes += data.length;
          cacheStats.misses++; cacheStats.bytesFetched += data.length;
          note(`miss chunk ${n}: fetched ${(data.length / 1024).toFixed(0)} KB in ${Date.now() - t0} ms`);
          return data;
        } catch (err) {
          if (attempt >= 3) throw err;
        }
      }
    })().finally(() => inflight.delete(n));
    inflight.set(n, p);
  }
  return { data: await p, fromCache: false };
}

/** One support file by name, or undefined if the bundle does not have it. */
export async function getFile(name: string): Promise<Fetched | undefined> {
  const idx = await getIndex();
  const e = idx.get(name);
  if (!e) return undefined;
  if (e.length === 0) return { data: Buffer.alloc(0), fromCache: true };
  const first = Math.floor(e.offset / CHUNK), last = Math.floor((e.offset + e.length - 1) / CHUNK);
  const chunks = await Promise.all(Array.from({ length: last - first + 1 }, (_, i) => getChunk(first + i)));
  const all = chunks.length === 1 ? chunks[0].data : Buffer.concat(chunks.map((c) => c.data));
  const start = e.offset - first * CHUNK;
  return { data: all.subarray(start, start + e.length), fromCache: chunks.every((c) => c.fromCache) };
}
/** Synchronous variant for the Node runner: the file if all its chunks are on disk (and check out). */
export function getFileCachedSync(name: string): Buffer | null | undefined {
  const e = index?.get(name);
  if (!e) return undefined;
  if (e.length === 0) return Buffer.alloc(0);
  if (!tarUrl) return null;
  const dir = bundleDir(tarUrl);
  const first = Math.floor(e.offset / CHUNK), last = Math.floor((e.offset + e.length - 1) / CHUNK);
  const parts: Buffer[] = [];
  for (let n = first; n <= last; n++) {
    const c = readChunk(dir, n);
    if (!c) return null;
    parts.push(c);
  }
  const all = Buffer.concat(parts), start = e.offset - first * CHUNK;
  return all.subarray(start, start + e.length);
}
