// SPDX-License-Identifier: AGPL-3.0-or-later
// kpathsea's file database (ls-R), the resource server's only index: TeX Live's own format, so the server serves an
// unmodified TeX Live tree. The engine asks for bare names (tikz.sty, cmr10.tfm, t1lmr.fd); a
// name is found the way kpathsea finds it for XeLaTeX: among the directories that hold a file of that name, the
// ones on the search path of its kind first (tex/xelatex before tex/latex before tex/generic for TeX input,
// fonts/tfm for .tfm, …), then any.
//
//   lsR(paths) -> text                  the database for a tree's files (paths relative to its root), as mktexlsr writes it
//   const db = parseLsR(text); db.resolve('tikz.sty') -> 'tex/latex/pgf/frontendlayer/tikz.sty' | null
//   db.inDir('tex/latex/siunitx') -> the names of the files in that directory (a package's own directory: its files)

const HEADER = '% ls-R -- filename database for kpathsea; do not change this line.';

export function lsR(paths: string[]): string {
  const dirs = new Map<string, Set<string>>([['.', new Set()]]);
  const add = (dir: string, entry: string) => { let s = dirs.get(dir); if (!s) dirs.set(dir, s = new Set()); s.add(entry); };
  for (const p of paths) {
    const parts = p.split('/');
    for (let i = 0; i < parts.length; i++) add(i ? `./${parts.slice(0, i).join('/')}` : '.', parts[i]);
  }
  const out = [HEADER];
  for (const d of [...dirs.keys()].sort()) {
    out.push(`${d === '.' ? './' : d}:`);
    out.push(...[...dirs.get(d)!].sort());
    out.push('');
  }
  return out.join('\n');
}

/** where kpathsea looks for a name of this kind under texmf-dist (XeLaTeX's search paths, in order) */
function searchPath(name: string): string[] {
  const ext = /\.([^./]+)$/.exec(name)?.[1]?.toLowerCase() ?? '';
  switch (ext) {
    case 'tfm': return ['fonts/tfm/'];
    case 'pfb': case 'pfa': return ['fonts/type1/'];
    case 'otf': return ['fonts/opentype/'];
    case 'ttf': case 'ttc': return ['fonts/truetype/'];
    case 'afm': return ['fonts/afm/'];
    case 'vf': return ['fonts/vf/'];
    case 'enc': return ['fonts/enc/dvips/', 'fonts/enc/'];
    case 'map': return ['fonts/map/dvipdfmx/', 'fonts/map/dvips/', 'fonts/map/'];
    case 'bst': return ['bibtex/bst/'];
    case 'bib': return ['bibtex/bib/'];
    case 'lua': return ['tex/xelatex/', 'tex/latex/', 'tex/generic/', 'tex/', 'scripts/'];
    default: return ['tex/xelatex/', 'tex/latex/', 'tex/xetex/', 'tex/generic/', 'tex/'];
  }
}

export interface LsR {
  /** the file's path in the tree (relative to its root), or null */
  resolve(name: string): string | null;
  has(name: string): boolean;
  /** the names of the files in a directory (relative to the tree's root, no trailing /) */
  inDir(dir: string): string[];
  /** how many files it lists */
  size: number;
}

export function parseLsR(text: string): LsR {
  const where = new Map<string, string[]>();          // basename -> directories ('' = the root), in the file's order
  const subdirs = new Set<string>();                    // every directory, as 'a/b'
  let dir: string | null = null;
  const lines = text.split('\n');
  for (const raw of lines) {
    const line = raw.replace(/\r$/, '');
    if (!line || line.startsWith('%')) continue;
    if (line.endsWith(':')) {
      dir = line.slice(0, -1).replace(/^\.\/?/, '').replace(/\/$/, '');
      if (dir) subdirs.add(dir);
      continue;
    }
    if (dir == null) continue;
    let l = where.get(line);
    if (!l) where.set(line, l = []);
    l.push(dir);
  }
  // an entry that is itself a directory is not a file
  let size = 0;
  const byDir = new Map<string, string[]>();
  for (const [name, ds] of where) {
    const files = ds.filter((d) => !subdirs.has(d ? `${d}/${name}` : name));
    if (files.length) { where.set(name, files); size += files.length; } else where.delete(name);
    for (const d of files) { let l = byDir.get(d); if (!l) byDir.set(d, l = []); l.push(name); }
  }
  const memo = new Map<string, string | null>();
  function resolve(name: string): string | null {
    if (memo.has(name)) return memo.get(name)!;
    let r: string | null = null;
    const slash = name.lastIndexOf('/');
    const base = slash >= 0 ? name.slice(slash + 1) : name;
    const ds = where.get(base);
    if (ds) {
      const full = (d: string) => (d ? `${d}/${base}` : base);
      if (slash >= 0) r = ds.map(full).find((p) => p === name || p.endsWith(`/${name}`)) ?? null;   // a name with a directory: that one
      else {
        for (const pre of searchPath(base)) { const d = ds.find((x) => `${x}/`.startsWith(pre)); if (d != null) { r = full(d); break; } }
        r ??= full(ds[0]);
      }
    }
    memo.set(name, r);
    return r;
  }
  return { resolve, has: (n) => resolve(n) != null, inDir: (d) => byDir.get(d) ?? [], size };
}
