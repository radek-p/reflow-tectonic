// SPDX-License-Identifier: AGPL-3.0-or-later
//! A thin driver that runs Tectonic's XeTeX and xdvipdfmx engines inside a
//! web browser (or Node), compiled for `wasm32-unknown-emscripten`.
//!
//! Tectonic's own top-level crate (`tectonic::driver::ProcessingSession`) is
//! not used: it pulls in the network stack (reqwest/curl), the on-disk bundle
//! cache and the CLI. Instead this driver wires the unmodified engine crates to
//! two I/O layers:
//!
//! * a memory layer (Tectonic's `MemoryIo`) holding the user's sources and
//!   everything the engines write (.log, .aux, .xdv, .pdf, .fmt);
//! * a "JS bundle" layer that asks JavaScript for TeX support files by name.
//!   JS answers synchronously (sync XHR inside a Web Worker, `fs` in Node),
//!   which is what makes on-demand "auto package install" possible without
//!   Asyncify: the engines never see a file that is not there yet.
//!
//! The C ABI below is what a JavaScript host calls (reflow/node/tectonic-api.js is a small one).
//!
//! Two additions for per-block compilation, both driver-level (no engine patch):
//!
//! * `rtx-block.tex` is never a file: opening it calls the JS import
//!   `ttw_js_block`, which answers with the next block's source. JS can
//!   pause there (Asyncify unwind -> memory snapshot) or block
//!   (Atomics.wait).
//! * `.log` and `.xdv` outputs are "live": their bytes are readable while
//!   the engine still runs (the block hook receives pointers to them), and
//!   after the run like any memory file.
//!
//! The capture API (this branch's engine_xetex patches) is switched on per run.
//! Capture flags come from bits 16..23 of `flags` (0 = the default 1|2|4|16);
//! FLAG_NO_CAPTURE (64) turns the capture off (PDF export).

mod memory;

use memory::MemoryIo;
use std::{
    cell::RefCell,
    collections::{HashMap, HashSet},
    ffi::CString,
    fmt::Arguments,
    hash::{Hash, Hasher},
    io::{Cursor, Read, SeekFrom},
    rc::Rc,
    time::{Duration, SystemTime},
};
use tectonic_bridge_core::{CoreBridgeLauncher, DriverHooks, SecuritySettings, SecurityStance};
use tectonic_engine_xdvipdfmx::XdvipdfmxEngine;
use tectonic_engine_xetex::{TexEngine, TexOutcome};
use tectonic_errors::Error;
use tectonic_io_base::{
    normalize_tex_path, InputFeatures, InputHandle, InputOrigin, IoProvider, OpenResult,
    OutputHandle,
};
use tectonic_status_base::{MessageKind, StatusBackend};

extern "C" {
    /// Implemented in `reflow/js/library.js`. `kind` is 0 for a bundle file, 1 for
    /// a format file. On success JS mallocs a buffer, stores its address and
    /// length in the out-parameters and returns 1; it returns 0 when the file
    /// does not exist.
    fn ttw_js_fetch(
        kind: i32,
        name: *const u8,
        name_len: usize,
        out_ptr: *mut *mut u8,
        out_len: *mut usize,
    ) -> i32;
    /// Implemented in `reflow/js/library.js`: a status message (0 note, 1 warning, 2 error).
    fn ttw_js_log(level: i32, msg: *const u8, len: usize);
    fn free(p: *mut u8);
    /// Implemented by the host (reflow/js/library.js calls it): the source of the next block
    /// (malloc'd by JS, like ttw_js_fetch), or 0 for "no more blocks". The
    /// live log and XDV so far are passed for option (c)'s per-block slicing.
    fn ttw_js_block(
        out_ptr: *mut *mut u8,
        out_len: *mut usize,
        log_ptr: *const u8,
        log_len: usize,
        xdv_ptr: *const u8,
        xdv_len: usize,
    ) -> i32;
}

fn js_log(level: i32, msg: &str) {
    unsafe { ttw_js_log(level, msg.as_ptr(), msg.len()) }
}

fn js_fetch(kind: i32, name: &str) -> Option<Vec<u8>> {
    let mut ptr: *mut u8 = std::ptr::null_mut();
    let mut len: usize = 0;
    let ok = unsafe { ttw_js_fetch(kind, name.as_ptr(), name.len(), &mut ptr, &mut len) };
    if ok == 0 {
        return None;
    }
    let data = if len == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(ptr, len) }.to_vec()
    };
    if !ptr.is_null() {
        unsafe { free(ptr) };
    }
    Some(data)
}

/// Status messages go to JS.
struct JsStatus;

impl StatusBackend for JsStatus {
    fn report(&mut self, kind: MessageKind, args: Arguments, err: Option<&Error>) {
        let level = match kind {
            MessageKind::Note => 0,
            MessageKind::Warning => 1,
            MessageKind::Error => 2,
        };
        let mut s = format!("{args}");
        if let Some(e) = err {
            for item in e.chain() {
                s.push_str(&format!("\n  caused by: {item}"));
            }
        }
        js_log(level, &s);
    }

    fn dump_error_logs(&mut self, output: &[u8]) {
        js_log(2, &String::from_utf8_lossy(output));
    }
}

/// A read-only view of a shared buffer. Bundle files and the format are
/// opened many times (every pass, every run); sharing them avoids copying,
/// which matters for wasm memory: the heap never shrinks.
struct SharedReader(Cursor<SharedBytes>);
struct SharedBytes(Rc<Vec<u8>>);

impl AsRef<[u8]> for SharedBytes {
    fn as_ref(&self) -> &[u8] {
        &self.0
    }
}

impl Read for SharedReader {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        self.0.read(buf)
    }
}

impl InputFeatures for SharedReader {
    fn get_size(&mut self) -> tectonic_errors::Result<usize> {
        Ok(self.0.get_ref().0.len())
    }
    fn try_seek(&mut self, pos: SeekFrom) -> tectonic_errors::Result<u64> {
        Ok(std::io::Seek::seek(&mut self.0, pos)?)
    }
}

fn shared_handle(name: &str, data: &Rc<Vec<u8>>) -> InputHandle {
    InputHandle::new_read_only(
        name.to_owned(),
        SharedReader(Cursor::new(SharedBytes(data.clone()))),
        InputOrigin::Other,
    )
}

/// The I/O stack the engines see.
/// An output whose bytes stay readable while the engine writes it.
struct LiveWriter(Rc<RefCell<Vec<u8>>>);

impl std::io::Write for LiveWriter {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.0.borrow_mut().extend_from_slice(buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

struct Io {
    /// .log and .xdv outputs, readable during the run (see LiveWriter).
    live: HashMap<String, Rc<RefCell<Vec<u8>>>>,
    mem: MemoryIo,
    /// Support files fetched from JS so far (reused across passes and runs).
    bundle: HashMap<String, Option<Rc<Vec<u8>>>>,
    /// Format files fetched from JS (kept out of `mem` so they are not
    /// snapshotted or listed as outputs).
    formats: HashMap<String, Rc<Vec<u8>>>,
    /// Name of the primary input; looked up in memory, then in the bundle
    /// (format generation reads `tectonic-format-latex.tex` from the bundle).
    primary: String,
    /// Names the engine tried to open during the current pass (hit or miss).
    probed: HashSet<String>,
}

impl Io {
    fn live_slice(&self, suffix: &str) -> (*const u8, usize) {
        match self.live.iter().find(|(k, _)| k.ends_with(suffix)) {
            Some((_, b)) => {
                let b = b.borrow();
                (b.as_ptr(), b.len())
            }
            None => (std::ptr::null(), 0),
        }
    }

    /// Ask JS for the next block (see ttw_js_block).
    fn block(&mut self) -> Option<Vec<u8>> {
        let (lp, ll) = self.live_slice(".log");
        let (xp, xl) = self.live_slice(".xdv");
        let mut ptr: *mut u8 = std::ptr::null_mut();
        let mut len: usize = 0;
        let ok = unsafe { ttw_js_block(&mut ptr, &mut len, lp, ll, xp, xl) };
        if ok == 0 {
            return None;
        }
        let data = if len == 0 {
            Vec::new()
        } else {
            unsafe { std::slice::from_raw_parts(ptr, len) }.to_vec()
        };
        if !ptr.is_null() {
            unsafe { free(ptr) };
        }
        Some(data)
    }

    fn bundle_open(&mut self, name: &str) -> OpenResult<InputHandle> {
        let entry = self
            .bundle
            .entry(name.to_owned())
            .or_insert_with(|| js_fetch(0, name).map(Rc::new));
        match entry {
            Some(data) => OpenResult::Ok(shared_handle(name, data)),
            None => OpenResult::NotAvailable,
        }
    }
}

impl IoProvider for Io {
    fn output_open_name(&mut self, name: &str) -> OpenResult<OutputHandle> {
        if name.ends_with(".log") || name.ends_with(".xdv") {
            let buf = Rc::new(RefCell::new(Vec::new()));
            self.mem.files.borrow_mut().remove(name);
            self.live.insert(name.to_owned(), buf.clone());
            return OpenResult::Ok(OutputHandle::new(name.to_owned(), LiveWriter(buf)));
        }
        self.mem.output_open_name(name)
    }

    fn output_open_stdout(&mut self) -> OpenResult<OutputHandle> {
        self.mem.output_open_stdout()
    }

    fn input_open_name(
        &mut self,
        name: &str,
        status: &mut dyn StatusBackend,
    ) -> OpenResult<InputHandle> {
        if name.is_empty() {
            return OpenResult::NotAvailable;
        }
        let norm = normalize_tex_path(name).into_owned();
        if norm == "rtx-block.tex" {
            return match self.block() {
                Some(data) => OpenResult::Ok(shared_handle(&norm, &Rc::new(data))),
                None => OpenResult::NotAvailable,
            };
        }
        self.probed.insert(norm.clone());
        match self.mem.input_open_name(&norm, status) {
            OpenResult::NotAvailable => self.bundle_open(&norm),
            r => r,
        }
    }

    fn input_open_primary(&mut self, status: &mut dyn StatusBackend) -> OpenResult<InputHandle> {
        let name = self.primary.clone();
        match self.mem.input_open_name(&name, status) {
            OpenResult::NotAvailable => self.bundle_open(&name),
            r => r,
        }
    }

    fn input_open_format(
        &mut self,
        name: &str,
        status: &mut dyn StatusBackend,
    ) -> OpenResult<InputHandle> {
        match self.mem.input_open_name(name, status) {
            OpenResult::NotAvailable => {}
            r => return r,
        }
        if !self.formats.contains_key(name) {
            match js_fetch(1, name) {
                Some(data) => {
                    self.formats.insert(name.to_owned(), Rc::new(data));
                }
                None => return OpenResult::NotAvailable,
            }
        }
        OpenResult::Ok(shared_handle(name, &self.formats[name]))
    }
}

struct Hooks {
    io: Io,
}

impl DriverHooks for Hooks {
    fn io(&mut self) -> &mut dyn IoProvider {
        &mut self.io
    }
}

thread_local! {
    static HOOKS: RefCell<Hooks> = RefCell::new(Hooks {
        io: Io {
            mem: MemoryIo::new(true),
            bundle: HashMap::new(),
            formats: HashMap::new(),
            primary: String::new(),
            probed: HashSet::new(),
            live: HashMap::new(),
        },
    });
    static LIST: RefCell<CString> = RefCell::new(CString::default());
}

/// Content hash for the rerun check (collisions only cost a missed rerun).
fn digest(data: &[u8]) -> u64 {
    let mut h = std::collections::hash_map::DefaultHasher::new();
    data.hash(&mut h);
    h.finish()
}

unsafe fn str_arg<'a>(p: *const u8, n: usize) -> &'a str {
    if n == 0 {
        return "";
    }
    std::str::from_utf8(std::slice::from_raw_parts(p, n)).unwrap_or("")
}

/// Put a file (e.g. the main .tex, or a precomputed `latex.fmt`) into memory.
#[no_mangle]
pub unsafe extern "C" fn ttw_add_file(name: *const u8, nlen: usize, data: *const u8, dlen: usize) {
    let name = str_arg(name, nlen).to_owned();
    let bytes = if dlen == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(data, dlen).to_vec()
    };
    HOOKS.with(|h| h.borrow_mut().io.mem.create_entry(&name, bytes));
}

/// Forget all files in memory (sources and outputs). Fetched support files stay cached.
#[no_mangle]
pub extern "C" fn ttw_clear_files() {
    HOOKS.with(|h| {
        let mut h = h.borrow_mut();
        h.io.mem.files.borrow_mut().clear();
        h.io.live.clear();
    });
}

/// Newline-separated names of all files in memory ("" is the terminal output).
#[no_mangle]
pub extern "C" fn ttw_file_list() -> *const libc_char {
    let names: Vec<String> =
        HOOKS.with(|h| h.borrow().io.mem.files.borrow().keys().cloned().collect());
    let joined = names.join("\n");
    LIST.with(|l| {
        *l.borrow_mut() = CString::new(joined).unwrap_or_default();
        l.borrow().as_ptr()
    })
}

#[allow(non_camel_case_types)]
type libc_char = std::os::raw::c_char;

/// Address of a file's bytes in memory, or null. Valid until the next call
/// into the driver. `out_len` receives the length.
#[no_mangle]
pub unsafe extern "C" fn ttw_file_data(name: *const u8, nlen: usize, out_len: *mut usize) -> *const u8 {
    let name = str_arg(name, nlen).to_owned();
    HOOKS.with(|h| {
        let h = h.borrow();
        let files = h.io.mem.files.borrow();
        match files.get(&name) {
            Some(f) => {
                *out_len = f.data.len();
                f.data.as_ptr()
            }
            None => {
                *out_len = 0;
                std::ptr::null()
            }
        }
    })
}

const FLAG_INITEX: u32 = 1;
const FLAG_PDF: u32 = 2;
const FLAG_SYNCTEX: u32 = 4;
const FLAG_KEEP_GOING: u32 = 8;
/// the reflowtex capture: on by default; this flag turns it off.
const FLAG_NO_CAPTURE: u32 = 64;
/// Display samples (route 2) by linear-memory snapshots: off by
/// default; unverified together with the Asyncify block pause.
const FLAG_RTX_SAMPLES: u32 = 128;
/// Default capture flags (xetex-rtxcapture.h): capture, pageless, math lists, source positions.
const RTX_FLAGS: i32 = 1 | 2 | 4 | 16;

/// Compile `main` (a file in memory).
///
/// * `FLAG_INITEX`: make a format; `main` is then a bundle file such as
///   `tectonic-format-latex.tex` and the result is stored as `<fmt_name>`.
/// * `FLAG_PDF`: run xdvipdfmx after TeX.
/// * `FLAG_SYNCTEX`: write `<stem>.synctex.gz`.
/// * `FLAG_KEEP_GOING`: do not stop at the first TeX error (like `-interaction=nonstopmode`
///   without halt-on-error); the default halts, as Tectonic does.
///
/// TeX is rerun (up to `max_passes`) while a file it tried to read during a
/// pass has different contents at the end of it (the .aux, .toc, ... cycle),
/// which is Tectonic's own rerun rule in simplified form.
///
/// Returns 0 spotless, 1 warnings, 2 TeX errors, 3 abort/failure.
#[no_mangle]
pub unsafe extern "C" fn ttw_compile(
    main: *const u8,
    mlen: usize,
    fmt_name: *const u8,
    flen: usize,
    flags: u32,
    max_passes: u32,
    build_date_unix: f64,
) -> i32 {
    let main = str_arg(main, mlen).to_owned();
    let fmt_name = str_arg(fmt_name, flen).to_owned();
    let build_date = SystemTime::UNIX_EPOCH + Duration::from_secs(build_date_unix as u64);
    let mut status = JsStatus;

    HOOKS.with(|cell| {
        let mut hooks = cell.borrow_mut();
        hooks.io.primary = main.clone();
        let initex = flags & FLAG_INITEX != 0;

        let stem = main
            .rsplit_once('.')
            .map(|(s, _)| s.to_owned())
            .unwrap_or(main.clone());

        let mut outcome = TexOutcome::Spotless;
        let passes = if initex { 1 } else { max_passes.max(1) };

        for pass in 0..passes {
            hooks.io.probed.clear();
            let before: HashMap<String, u64> = hooks
                .io
                .mem
                .files
                .borrow()
                .iter()
                .map(|(k, v)| (k.clone(), digest(&v.data)))
                .collect();

            let result = {
                let mut launcher = CoreBridgeLauncher::new_with_security(
                    &mut *hooks,
                    &mut status,
                    SecuritySettings::new(SecurityStance::DisableInsecures),
                );
                let mut engine = TexEngine::default();
                engine
                    .halt_on_error_mode(initex || flags & FLAG_KEEP_GOING == 0)
                    .initex_mode(initex)
                    .synctex(flags & FLAG_SYNCTEX != 0)
                    .build_date(build_date);
                if !initex && flags & FLAG_NO_CAPTURE == 0 {
                    let custom = ((flags >> 16) & 0xff) as i32;
                    let rtx = if custom != 0 { custom } else { RTX_FLAGS };
                    if flags & FLAG_RTX_SAMPLES != 0 {
                        engine.rtx_capture(rtx | 8, &[128 * 65536, 256 * 65536]);
                    } else {
                        engine.rtx_capture(rtx, &[]);
                    }
                }
                if initex {
                    engine.process(&mut launcher, "UNUSED.fmt", "texput")
                } else {
                    engine.process(&mut launcher, &fmt_name, &main)
                }
            };

            // Live outputs become ordinary memory files once the pass is over.
            let live: Vec<(String, Rc<RefCell<Vec<u8>>>)> = hooks.io.live.drain().collect();
            for (k, v) in live {
                let data = std::mem::take(&mut *v.borrow_mut());
                hooks.io.mem.create_entry(&k, data);
            }

            match result {
                Ok(o) => outcome = o,
                Err(e) => {
                    status.report(MessageKind::Error, format_args!("TeX engine failed"), Some(&e));
                    return 3;
                }
            }
            if let TexOutcome::Errors = outcome {
                return 2;
            }

            if initex {
                // Store the dumped format under the requested name.
                let fmt = hooks
                    .io
                    .mem
                    .files
                    .borrow()
                    .iter()
                    .find(|(k, _)| k.ends_with(".fmt"))
                    .map(|(k, v)| (k.clone(), v.data.clone()));
                match fmt {
                    Some((k, data)) => {
                        hooks.io.mem.files.borrow_mut().remove(&k);
                        hooks.io.mem.create_entry(&fmt_name, data);
                    }
                    None => {
                        js_log(2, "initex run produced no .fmt file");
                        return 3;
                    }
                }
                break;
            }

            // Rerun if a file TeX tried to read changed during the pass.
            let changed: Vec<String> = {
                let files = hooks.io.mem.files.borrow();
                hooks
                    .io
                    .probed
                    .iter()
                    .filter(|n| {
                        let now = files.get(*n).map(|f| digest(&f.data));
                        let then = before.get(*n).copied();
                        now.is_some() && now != then
                    })
                    .cloned()
                    .collect()
            };
            if changed.is_empty() || pass + 1 == passes {
                break;
            }
            js_log(0, &format!("rerunning TeX because {} changed", changed.join(", ")));
        }

        if !initex && flags & FLAG_PDF != 0 {
            let xdv = format!("{stem}.xdv");
            let pdf = format!("{stem}.pdf");
            let mut launcher = CoreBridgeLauncher::new_with_security(
                &mut *hooks,
                &mut status,
                SecuritySettings::new(SecurityStance::DisableInsecures),
            );
            let r = XdvipdfmxEngine::default()
                .build_date(build_date)
                .process(&mut launcher, &xdv, &pdf);
            if let Err(e) = r {
                status.report(MessageKind::Error, format_args!("xdvipdfmx failed"), Some(&e));
                return 3;
            }
        }

        match outcome {
            TexOutcome::Spotless => 0,
            TexOutcome::Warnings => 1,
            TexOutcome::Errors => 2,
        }
    })
}

fn main() {
    // Nothing: JS drives the exported functions. The runtime stays alive
    // (-sEXIT_RUNTIME=0) after main returns.
}
