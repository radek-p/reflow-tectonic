// SPDX-License-Identifier: AGPL-3.0-or-later
// TeX Live's package database (tlpkg/texlive.tlpdb): the packages, their files, sizes, dependencies and licences.
//
//   const db = parseTlpdb(text)              Map name -> Pkg
//   closure(db, ['scheme-basic'])            every package a scheme or collection brings (its `depend` lines, recursively)
//   ownerOf(db)                              runfile basename -> package (the first that has it)
//
// Sizes: `runfiles size=N` counts TeX Live's 4 KiB blocks (uncompressed); `containersize` is the .tar.xz in bytes.

export interface Pkg {
  name: string; category: string; revision: number; shortdesc: string;
  depends: string[];
  runfiles: string[]; docfiles: string[];
  /** 4 KiB blocks */ runsize: number; docsize: number; srcsize: number;
  /** .tar.xz bytes */ containersize: number; doccontainersize: number;
  containerchecksum: string;
  license: string;
}

export function parseTlpdb(text: string): Map<string, Pkg> {
  const db = new Map<string, Pkg>();
  let p: Pkg | null = null;
  let list: string[] | null = null;
  for (const line of text.split('\n')) {
    if (line.startsWith(' ')) { if (list) list.push(line.trim().split(' ')[0]); continue; }
    list = null;
    if (!line) { p = null; continue; }
    const sp = line.indexOf(' ');
    const key = sp < 0 ? line : line.slice(0, sp), val = sp < 0 ? '' : line.slice(sp + 1);
    if (key === 'name') {
      p = { name: val, category: '', revision: 0, shortdesc: '', depends: [], runfiles: [], docfiles: [], runsize: 0, docsize: 0, srcsize: 0,
        containersize: 0, doccontainersize: 0, containerchecksum: '', license: '' };
      db.set(val, p);
      continue;
    }
    if (!p) continue;
    const size = () => Number(/size=(\d+)/.exec(val)?.[1] ?? 0);
    switch (key) {
      case 'category': p.category = val; break;
      case 'revision': p.revision = Number(val); break;
      case 'shortdesc': p.shortdesc = val; break;
      case 'depend': p.depends.push(val); break;
      case 'containersize': p.containersize = Number(val); break;
      case 'doccontainersize': p.doccontainersize = Number(val); break;
      case 'containerchecksum': p.containerchecksum = val; break;
      case 'catalogue-license': p.license = val; break;
      case 'runfiles': p.runsize = size(); list = p.runfiles; break;
      case 'docfiles': p.docsize = size(); list = p.docfiles; break;
      case 'srcfiles': p.srcsize = size(); list = []; break;
      case 'binfiles': list = []; break;
    }
  }
  return db;
}

/** Platform-specific packages (`foo.x86_64-linux`, `foo.ARCH` in depends) are left out: the browser has no binaries. */
const platform = (n: string) => /\.(ARCH|[\w-]+-(linux|darwin|cygwin|solaris|freebsd|netbsd|windows|mingw\w*)|win32|universal-darwin|windows)$/.test(n);

export function closure(db: Map<string, Pkg>, roots: string[]): Set<string> {
  const out = new Set<string>();
  const walk = (n: string) => {
    if (out.has(n) || platform(n)) return;
    const p = db.get(n);
    if (!p) return;
    out.add(n);
    for (const d of p.depends) walk(d);
  };
  for (const r of roots) walk(r);
  return out;
}

/** runfile basename -> the package that ships it (texmf-dist files only; the first package wins) */
export function ownerOf(db: Map<string, Pkg>): Map<string, string> {
  const m = new Map<string, string>();
  // a development snapshot (latex-dev, latex-tools-dev, …) never wins over the release package
  const order = [...db.values()].sort((a, b) => Number(/-dev$/.test(a.name)) - Number(/-dev$/.test(b.name)));
  for (const p of order) for (const f of p.runfiles) {
    if (!f.startsWith('texmf-dist/') && !f.startsWith('RELOC/')) continue;
    const b = f.slice(f.lastIndexOf('/') + 1);
    if (!m.has(b)) m.set(b, p.name);
  }
  return m;
}

/** a runfile's path in the installed tree (relocatable packages say RELOC/ for texmf-dist/) */
export const installedPath = (f: string) => (f.startsWith('RELOC/') ? `texmf-dist/${f.slice(6)}` : f);

export interface Size { packages: number; files: number; bytes: number; xz: number }
/** what a set of packages weighs without documentation and sources (runfiles only) */
export function sizeOf(db: Map<string, Pkg>, names: Iterable<string>): Size {
  const s: Size = { packages: 0, files: 0, bytes: 0, xz: 0 };
  for (const n of names) {
    const p = db.get(n);
    if (!p) continue;
    s.packages++; s.files += p.runfiles.length; s.bytes += p.runsize * 4096; s.xz += p.containersize;
  }
  return s;
}
