// SPDX-License-Identifier: AGPL-3.0-or-later
// Small JS wrapper over the wasm driver, shared by the browser worker and Node.
//   const tt = await Tectonic.create({ createModule, fetchSync, log });
//   tt.addFile("doc.tex", bytes); const r = tt.compile("doc.tex", { pdf: true });
//   tt.file("doc.log") -> Uint8Array
// fetchSync(kind, name) must return a Uint8Array or null *synchronously*
// (kind 0: support file from the bundle, 1: format file).

const FLAG_INITEX = 1, FLAG_PDF = 2, FLAG_SYNCTEX = 4, FLAG_KEEP_GOING = 8;
const enc = new TextEncoder();

export class Tectonic {
  static async create({ createModule, fetchSync, log, moduleArgs = {} }) {
    const M = await createModule({
      ...moduleArgs,
      ttwFetch: fetchSync,
      ttwLog: log ?? ((lvl, s) => (lvl ? console.warn : console.log)(s)),
      print: moduleArgs.print ?? ((s) => {}),
      printErr: moduleArgs.printErr ?? ((s) => console.warn(s)),
    });
    return new Tectonic(M);
  }
  constructor(M) { this.M = M; }

  // Copy a JS string/bytes into wasm memory; returns [ptr, len]; caller frees.
  _bytes(b) {
    if (typeof b === "string") b = enc.encode(b);
    const p = b.length ? this.M._malloc(b.length) : 0;
    if (b.length) this.M.HEAPU8.set(b, p);
    return [p, b.length];
  }
  _call(fn, ...bufs) {
    const args = bufs.map((b) => this._bytes(b));
    try { return fn(...args.flat()); } finally { for (const [p] of args) if (p) this.M._free(p); }
  }

  addFile(name, data) { this._call((np, nl, dp, dl) => this.M._ttw_add_file(np, nl, dp, dl), name, data); }
  clearFiles() { this.M._ttw_clear_files(); }
  files() {
    const s = this.M.UTF8ToString(this.M._ttw_file_list());
    return s.split("\n").filter((n) => n !== "");
  }
  /** A copy of a file in the driver's memory (outputs: .log .aux .xdv .pdf ...). "" is the terminal. */
  file(name) {
    const lenp = this.M._malloc(4);
    try {
      const p = this._call((np, nl) => this.M._ttw_file_data(np, nl, lenp), name);
      if (!p) return null;
      const len = this.M.HEAPU32[lenp >> 2];
      return this.M.HEAPU8.slice(p, p + len);
    } finally { this.M._free(lenp); }
  }
  /**
   * Compile `main` (added with addFile). Returns the driver status:
   * 0 spotless, 1 warnings, 2 TeX errors, 3 abort.
   * With { initex: true } main is a bundle file (tectonic-format-latex.tex)
   * and the dumped format is stored in memory as `format`.
   */
  compile(main, { format = "latex.fmt", pdf = false, initex = false, synctex = false, keepGoing = false, maxPasses = 3,
                  buildDate = Math.floor(Date.now() / 1000) } = {}) {
    const flags = (initex ? FLAG_INITEX : 0) | (pdf ? FLAG_PDF : 0) | (synctex ? FLAG_SYNCTEX : 0) |
      (keepGoing ? FLAG_KEEP_GOING : 0);
    return this._call((mp, ml, fp, fl) => this.M._ttw_compile(mp, ml, fp, fl, flags, maxPasses, buildDate),
      main, format);
  }
}
