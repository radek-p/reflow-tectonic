// SPDX-License-Identifier: AGPL-3.0-or-later
// Makes the engine's TeX tree: a TeX Live tree as TeX Live installs it, for a set of packages, served beside a page
// that runs the engine (it fetches what a document loads from there).
//
//   node reflow/tree/build-tree.ts [--set reflow/tree/starting-set.txt (scheme-small)] [--out reflow/dist/tree]
//        [--repo <tlnet url>] [--year 2022] [--from <an installed TeX Live root>] [--closure [--embedded a.txt,…]]
//   (reflow/build.sh tree runs it)
//
// What it writes, under <out>/texlive/<year>/ (served as is by a static server):
//   texmf-dist/…               every runfile of each package, at its TDS path (unchanged bytes)
//   texmf-dist/doc/…           only the packages' licence files (README when a package has none); no other doc/,
//                              and no source/
//   texmf-dist/ls-R            kpathsea's file database (as mktexlsr writes it): the client's only index
//   tlpkg/tlpobj/<pkg>.tlpobj  each package's record (its licence: catalogue-license), as an installation keeps it
//   LICENSE.TL, LICENSE.CTAN   TeX Live's own notices
//   README.txt, LICENCES.txt   what this tree is, and each package's licence and licence files (our notes)
//
// The packages come from TeX Live's network archive (archive/<pkg>.tar.xz, checked against the tlpdb's sha512),
// cached in $TEXLIVE_ARCHIVE, else reflow/.cache/texlive-archive/. The year must be the engine's: its format and embedded files are TeX Live 2022
// (Tectonic's bundle v33, LaTeX 2021-11-15), so the default is TeX Live 2022's final archive (tlnet-final).
// --from copies from an installed tree instead (no download; its year must match too).
import { readFileSync, writeFileSync, mkdirSync, existsSync, readdirSync, statSync, rmSync, copyFileSync, renameSync } from 'node:fs';
import { join, dirname, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { parseTlpdb, installedPath, ownerOf, closure, type Pkg } from './tlpdb.ts';
import { lsR, parseLsR } from './ls-r.ts';

const HERE = dirname(fileURLToPath(import.meta.url)), R = join(HERE, '..');   // reflow/
const args = process.argv.slice(2);
const opt = (k: string, d?: string) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const YEAR = opt('--year', '2022')!;
const REPO = opt('--repo', `https://ftp.tu-chemnitz.de/pub/tug/historic/systems/texlive/${YEAR}/tlnet-final`)!;
const SET = opt('--set', join(HERE, 'starting-set.txt'))!;
/** dependencies TeX Live does not record (extra-packages.txt says why); --no-extra leaves them out */
const EXTRA = args.includes('--no-extra') ? null : opt('--extra', join(HERE, 'extra-packages.txt'))!;
const OUT = opt('--out', join(R, 'dist/tree'))!;
const FROM = opt('--from');
/** TeX Live's archive files, downloaded once ($TEXLIVE_ARCHIVE, else reflow/.cache/texlive-archive) */
const CACHE = process.env.TEXLIVE_ARCHIVE ?? join(R, '.cache/texlive-archive');
const TREE = join(OUT, 'texlive', YEAR);
// ("the version blocker") TeX Live 2022's final archive is TeX Live as of March 2023; the engine's format
// and embedded files are the 2022 RELEASE (Tectonic's bundle v33). Packages updated during the year need a newer kernel
// (siunitx: "Support package expl3 too old"). So for 2022 the files' bytes come from the engine's own bundle (the same
// snapshot, read with HTTP range requests and cached), at the paths, with the licences and records, of TeX Live 2022.
// --tlnet-bytes keeps the archive's bytes; --from (an installed tree) always uses its own.
const FROM_BUNDLE = !FROM && YEAR === '2022' && !args.includes('--tlnet-bytes');
/** --closure: also the packages the set's files load (a text scan, recursive: it over-reaches, pulling in pstricks,
 *  newtx, unicode-math through optional loads; the set is checked by loading each package instead: ReflowEd's tests/test-resources.ts --verify-set) */
const CLOSURE = args.includes('--closure');
/** a package's licence files, in its doc container (doc/ is otherwise left out) */
export const LICENCE_FILE = /(^|\/)(licen[cs]e|copying|copyright|ofl|lppl|gpl|notice)[^/]*$/i;

async function download(url: string, to: string, sha512?: string): Promise<void> {
  if (existsSync(to) && (!sha512 || hashOf(to) === sha512)) return;
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  const b = Buffer.from(await r.arrayBuffer());
  if (sha512 && createHash('sha512').update(b).digest('hex') !== sha512) throw new Error(`${url}: checksum differs from the tlpdb's`);
  mkdirSync(dirname(to), { recursive: true });
  writeFileSync(to + '.part', b);
  renameSync(to + '.part', to);
}
const hashOf = (f: string) => createHash('sha512').update(readFileSync(f)).digest('hex');

function filesUnder(d: string): string[] {
  const out: string[] = [];
  const walk = (x: string) => { for (const n of readdirSync(x)) { const p = join(x, n); if (statSync(p).isDirectory()) walk(p); else out.push(p); } };
  if (existsSync(d)) walk(d);
  return out;
}

async function main() {
  const names = (file: string) => readFileSync(file, 'utf8').split('\n').map((l) => l.replace(/#.*/, '').trim()).filter(Boolean);
  const roots = [...names(SET), ...(EXTRA && existsSync(EXTRA) ? names(EXTRA) : [])];
  let bundle: { getFile(name: string): Promise<{ data: Buffer } | undefined> } | null = null;
  if (FROM_BUNDLE) {
    process.env.TECTONIC_CHUNK ??= String(1 << 16);         // small range reads: the set's files are spread over the 2.9 GB tar
    process.env.TECTONIC_CACHE ??= join(R, '.cache/tectonic-bundle');
    bundle = await import('../node/bundle.ts');
  }
  // --embedded a.txt,b.txt: lists of files a host already has (one name a line); --closure takes no package for them
  const embedded = new Set((opt('--embedded') ?? '').split(',').filter(Boolean)
    .flatMap((f) => readFileSync(f, 'utf8').split('\n').map((l) => l.trim()).filter(Boolean)));
  mkdirSync(CACHE, { recursive: true });
  const tlpdbFile = join(CACHE, `texlive-${YEAR}.tlpdb`);
  if (!FROM && !existsSync(tlpdbFile)) {
    await download(`${REPO}/tlpkg/texlive.tlpdb.xz`, tlpdbFile + '.xz');
    execFileSync('xz', ['-dkf', tlpdbFile + '.xz']);
  }
  const db = parseTlpdb(readFileSync(FROM ? join(FROM, 'tlpkg/texlive.tlpdb') : tlpdbFile, 'utf8'));
  const unknown = roots.filter((n) => !db.has(n));
  if (unknown.length) throw new Error(`not in TeX Live ${YEAR}: ${unknown.join(', ')}`);
  // a scheme or a collection stands for what tlmgr installs with it: its dependency closure (no platform binaries)
  const wanted = [...closure(db, roots)].sort();
  console.log(`the set: ${roots.join(', ')} = ${wanted.length} TeX Live packages`);
  rmSync(TREE, { recursive: true, force: true });
  mkdirSync(join(TREE, 'tlpkg/tlpobj'), { recursive: true });
  const report: { name: string; licence: string; files: number; licenceFiles: string[] }[] = [];
  const stage = join(OUT, '.stage');   // unpacked here, then moved into the tree (same file system as the tree)
  const owner = ownerOf(db);
  // names at more than one path in the set: the snapshot's bytes go to the path the engine resolves (ls-R), the
  // others keep the archive's bytes
  const pathsOf = new Map<string, number>();
  for (const n of wanted) for (const f of db.get(n)!.runfiles.map(installedPath)) if (f.startsWith('texmf-dist/')) pathsOf.set(base(f), (pathsOf.get(base(f)) ?? 0) + 1);
  const dupNames = new Set([...pathsOf].filter(([, c]) => c > 1).map(([n]) => n));
  const queue = [...wanted], included = new Set(wanted), added: string[] = [], notInBundle: string[] = [];
  while (queue.length) {
    const name = queue.shift()!;
    const p = db.get(name)!;
    let licenceFiles: string[] = [];
    if (FROM) {
      for (const f of p.runfiles.map(installedPath)) { const to = join(TREE, f); mkdirSync(dirname(to), { recursive: true }); copyFileSync(join(FROM, f), to); }
      licenceFiles = p.docfiles.map(installedPath).filter((f) => LICENCE_FILE.test(f) && existsSync(join(FROM, f)));
      for (const f of licenceFiles) { const to = join(TREE, f); mkdirSync(dirname(to), { recursive: true }); copyFileSync(join(FROM, f), to); }
      writeFileSync(join(TREE, 'tlpkg/tlpobj', `${name}.tlpobj`), tlpobjOf(readFileSync(join(FROM, 'tlpkg/texlive.tlpdb'), 'utf8'), name));
    } else {
      rmSync(stage, { recursive: true, force: true }); mkdirSync(stage, { recursive: true });
      const run = join(CACHE, `${name}.tar.xz`);
      await download(`${REPO}/archive/${name}.tar.xz`, run, p.containerchecksum || undefined);
      execFileSync('tar', ['-xJf', run, '-C', stage]);
      // a relocatable package's container holds texmf-dist's contents at its root (RELOC): moved under texmf-dist/
      place(stage, p);
      if (p.doccontainersize) {
        const doc = join(CACHE, `${name}.doc.tar.xz`);
        await download(`${REPO}/archive/${name}.doc.tar.xz`, doc);
        rmSync(stage, { recursive: true, force: true }); mkdirSync(stage, { recursive: true });
        execFileSync('tar', ['-xJf', doc, '-C', stage]);
        const docs = filesUnder(stage).map((f) => relative(stage, f)).filter((f) => !f.startsWith('tlpkg/'));
        let keep = docs.filter((f) => LICENCE_FILE.test(f));
        if (!keep.length) keep = docs.filter((f) => /(^|\/)readme[^/]*$/i.test(f));
        for (const f of keep) {
          const dest = join(TREE, p.runfiles.some((x) => x.startsWith('RELOC/')) || !f.startsWith('texmf-dist/') ? `texmf-dist/${f.replace(/^texmf-dist\//, '')}` : f);
          mkdirSync(dirname(dest), { recursive: true }); copyFileSync(join(stage, f), dest);
          licenceFiles.push(relative(TREE, dest));
        }
      }
    }
    if (bundle) {
      // the bytes of the engine's snapshot; a file the snapshot does not have (added to the package later) is left out.
      // The snapshot is flat (one file per name): a name at several paths is done after the loop, below.
      const b = bundle;
      const todo = p.runfiles.map(installedPath).filter((f) => f.startsWith('texmf-dist/') && existsSync(join(TREE, f)) && !dupNames.has(base(f)));
      await pool(todo, 8, async (f) => {
        const got = await b.getFile(base(f));
        if (got) writeFileSync(join(TREE, f), got.data); else { rmSync(join(TREE, f)); notInBundle.push(f); }
      });
    }
    if (CLOSURE) {
      // what its packages and classes load: the TeX Live packages behind those names, unless the embedded set has the file
      for (const f of p.runfiles.map(installedPath).filter((x) => /\.(sty|cls)$/.test(x) && existsSync(join(TREE, x)))) {
        const t = readFileSync(join(TREE, f), 'latin1').replace(/(^|[^\\])%.*$/gm, '$1');
        for (const m of t.matchAll(/\\(?:RequirePackage(?:WithOptions)?|usepackage|LoadClass(?:WithOptions)?)\s*(?:\[[^\]]*\])?\s*\{([^}#\\]*)\}/g)) {
          const cls = /LoadClass/.test(m[0]);
          for (const n of m[1].split(',').map((x) => x.trim()).filter((x) => /^[\w.-]+$/.test(x))) {
            const file = `${n}.${cls ? 'cls' : 'sty'}`;
            const pkg = owner.get(file);
            if (!pkg || included.has(pkg) || embedded.has(file)) continue;
            included.add(pkg); queue.push(pkg); added.push(`${pkg} (for ${file}, by ${name})`);
          }
        }
      }
    }
    report.push({ name, licence: p.license || '(not stated in the tlpdb)', files: p.runfiles.length, licenceFiles });
  }
  rmSync(stage, { recursive: true, force: true });
  // TeX Live's own notices: an installation has them at its root (they come with texlive.infra)
  if (FROM) { for (const n of ['LICENSE.TL', 'LICENSE.CTAN']) if (existsSync(join(FROM, n))) copyFileSync(join(FROM, n), join(TREE, n)); }
  else {
    const infra = db.get('texlive.infra')!;
    const tar = join(CACHE, 'texlive.infra.tar.xz');
    await download(`${REPO}/archive/texlive.infra.tar.xz`, tar, infra.containerchecksum || undefined);
    mkdirSync(stage, { recursive: true });
    execFileSync('tar', ['-xJf', tar, '-C', stage, 'LICENSE.TL', 'LICENSE.CTAN']);
    for (const n of ['LICENSE.TL', 'LICENSE.CTAN']) renameSync(join(stage, n), join(TREE, n));
    rmSync(stage, { recursive: true, force: true });
  }
  const listing = () => lsR(filesUnder(join(TREE, 'texmf-dist')).map((f) => relative(join(TREE, 'texmf-dist'), f)).filter((f) => f !== 'ls-R'));
  const dupResolved: string[] = [];
  if (bundle && dupNames.size) {
    const idx = parseLsR(listing());
    for (const n of dupNames) {
      const path = idx.resolve(n);
      if (!path || !existsSync(join(TREE, 'texmf-dist', path))) continue;
      const got = await bundle.getFile(n);
      if (got) { writeFileSync(join(TREE, 'texmf-dist', path), got.data); dupResolved.push(path); }
    }
  }
  writeFileSync(join(TREE, 'texmf-dist/ls-R'), listing());
  // the notes at the tree's root: what this is, and every package's licence
  writeFileSync(join(TREE, 'README.txt'), readmeOf(roots, report.length, FROM ? 'installation' : bundle ? 'snapshot' : 'archive'));
  writeFileSync(join(TREE, 'LICENCES.txt'), licencesOf(report));
  const bytes = filesUnder(TREE).reduce((s, f) => s + statSync(f).size, 0);
  const noLicence = report.filter((r) => !r.licenceFiles.length).map((r) => r.name);
  if (added.length) console.log(`added for what the set's files load: ${added.join(', ')}`);
  if (notInBundle.length) console.log(`${notInBundle.length} files of the archive's packages are not in the engine's TeX Live 2022 snapshot (left out): ${notInBundle.slice(0, 12).join(', ')}${notInBundle.length > 12 ? ', …' : ''}`);
  console.log(`${TREE}: ${report.length} packages, ${filesUnder(TREE).length} files, ${(bytes / 1048576).toFixed(1)} MB`);
  if (dupNames.size) console.log(`${dupNames.size} names at more than one path: the snapshot's bytes at the path the engine resolves (${dupResolved.length}), the archive's elsewhere`);
  if (noLicence.length) console.log(`packages with no licence file in their documentation (their licence is in the tlpobj and in their files' headers): ${noLicence.join(', ')}`);
  writeFileSync(join(R, 'dist/tree-report.json'), JSON.stringify({ year: YEAR, repo: FROM ?? REPO, bytesFrom: FROM ? 'installation' : bundle ? 'tectonic bundle v33 (the engine\'s TeX Live 2022 snapshot)' : 'tlnet archive', bytes, files: filesUnder(TREE).length, roots, added, notInBundle, duplicateNames: [...dupNames].sort(), packages: report }, null, 1));
}

function readmeOf(roots: string[], packages: number, bytes: 'installation' | 'snapshot' | 'archive'): string {
  return `TeX Live ${YEAR} for ReflowEd's TeX resource server
${'='.repeat(22 + YEAR.length + 30)}

This directory is a part of a TeX Live ${YEAR} installation: the packages of ${roots.join(', ')}
(${packages} TeX Live packages, the set as TeX Live's installer gives it), laid out as TeX Live installs them.
ReflowEd, a TeX editor in the browser, reads files from here when a document needs a package, class or font
its own file does not carry. Nothing here is ReflowEd's code.

What is here
  texmf-dist/            each package's runfiles at their TeX Live paths; texmf-dist/ls-R is kpathsea's file list,
                         the only index ReflowEd reads. No doc/ (except each package's licence files) and no source/.
  tlpkg/tlpobj/          each package's TeX Live record (name, revision, catalogue-license, files)
  LICENSE.TL, LICENSE.CTAN   TeX Live's own notices
  LICENCES.txt           each package's licence and the licence files kept for it

The bytes${bytes === 'snapshot' ? `: each file is the version of TeX Live ${YEAR} as released (the snapshot ReflowEd's engine
was built from: Tectonic's bundle v33), at the path of TeX Live ${YEAR}'s final archive. Files added to a package
later in the year are left out.` : bytes === 'installation' ? `: copied from an installation, unchanged.` : `: those of TeX Live ${YEAR}'s final archive, unchanged.`}

Licences
  Every package keeps its own licence (most are LPPL; see LICENCES.txt and tlpkg/tlpobj/<package>.tlpobj).
  They are passed on as TeX Live and CTAN distribute them, with their licence files. A file's own header says
  more where a package has no separate licence file.

Made by reflow/tree/build-tree.ts (https://github.com/radek-p/reflow-tectonic).
`;
}

function licencesOf(report: { name: string; licence: string; files: number; licenceFiles: string[] }[]): string {
  const rows = [...report].sort((a, b) => a.name.localeCompare(b.name)).map((r) =>
    `${r.name}\n  licence (TeX Live's catalogue-license): ${r.licence}\n  licence files: ${r.licenceFiles.length ? r.licenceFiles.join(', ') : 'none in its documentation: see tlpkg/tlpobj/' + r.name + '.tlpobj and its files\' headers'}\n`);
  return `The licences of the TeX Live packages in this tree (TeX Live ${YEAR})
One entry per package: its licence as TeX Live's database records it, and where its licence texts are kept.
TeX Live's own notices: LICENSE.TL and LICENSE.CTAN, beside this file.

${rows.join('\n')}`;
}

const base = (f: string) => f.slice(f.lastIndexOf('/') + 1);
async function pool<T>(items: T[], n: number, f: (x: T) => Promise<void>): Promise<void> {
  let i = 0;
  await Promise.all(Array.from({ length: Math.min(n, items.length) }, async () => { while (i < items.length) await f(items[i++]); }));
}

/** an archive container's files into the tree: texmf-dist/… as is (RELOC containers hold it at their root), tlpkg/tlpobj */
function place(stage: string, p: Pkg) {
  const reloc = p.runfiles.some((f) => f.startsWith('RELOC/'));
  for (const f of filesUnder(stage)) {
    const rel = relative(stage, f);
    const dest = rel.startsWith('tlpkg/') ? rel : reloc && !rel.startsWith('texmf-dist/') ? `texmf-dist/${rel}` : rel;
    if (!dest.startsWith('texmf-dist/') && !dest.startsWith('tlpkg/tlpobj/')) continue;    // texmf-dist only (no bin/, no texmf-var)
    mkdirSync(dirname(join(TREE, dest)), { recursive: true });
    renameSync(f, join(TREE, dest));
  }
}

/** one package's record out of a tlpdb (an installed tree keeps it as tlpkg/tlpobj/<pkg>.tlpobj) */
function tlpobjOf(tlpdb: string, name: string): string {
  const at = tlpdb.indexOf(`\nname ${name}\n`);
  if (at < 0) return '';
  const end = tlpdb.indexOf('\n\n', at + 1);
  return tlpdb.slice(at + 1, end < 0 ? undefined : end + 1);
}

main().catch((e) => { console.error(e.message); process.exit(1); });
