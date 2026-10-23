// SPDX-License-Identifier: AGPL-3.0-or-later
// Emscripten JS library for the engine (reflow/driver): fetches, logs and
// blocks (ttw_js_fetch, ttw_js_log, ttw_js_block) and display-sample snapshots (rtx_js_*).
addToLibrary({
  ttw_js_fetch__deps: ['malloc'],
  ttw_js_fetch: function (kind, namePtr, nameLen, outPtr, outLen) {
    const name = UTF8ToString(namePtr, nameLen);
    const data = Module.ttwFetch ? Module.ttwFetch(kind, name) : null;
    if (!data) return 0;
    const p = data.length ? _malloc(data.length) : 0;
    if (data.length) HEAPU8.set(data, p);
    HEAPU32[outPtr >> 2] = p;
    HEAPU32[outLen >> 2] = data.length;
    return 1;
  },
  ttw_js_log: function (level, msgPtr, len) {
    const text = UTF8ToString(msgPtr, len);
    if (Module.ttwLog) Module.ttwLog(level, text);
    else (level ? console.warn : console.log)(text);
  },
  ttw_js_block: function (outPtr, outLen, logPtr, logLen, xdvPtr, xdvLen) {
    return Module.ttwBlock ? Module.ttwBlock(outPtr, outLen, logPtr, logLen, xdvPtr, xdvLen) : 0;
  },
});

// reflowtex capture: display samples without fork. The engine
// asks for a snapshot of linear memory at main_control's loop head, runs a
// sample child to the display's end, stashes its sample here, and asks for
// the snapshot back (xetex-rtxcapture.c, wasm_next).
addToLibrary({
  rtx_js_snapshot: function () {
    Module.rtxSnap = HEAPU8.slice(); Module.rtxStash = []; Module.rtxNext = 0;
    Module.rtxStats = Module.rtxStats ?? { snapshots: 0, restores: 0, ms: 0 };
    Module.rtxStats.snapshots++;
  },
  rtx_js_restore: function () {
    const t = performance.now(), s = Module.rtxSnap;
    HEAPU8.set(s);
    if (HEAPU8.length > s.length) HEAPU8.fill(0, s.length);   // fresh memory reads as zeros (calloc)
    Module.rtxStats.restores++; Module.rtxStats.ms += performance.now() - t;
  },
  rtx_js_next: function () { return Module.rtxNext++; },
  rtx_js_stash: function (p, n) { Module.rtxStash.push(HEAPU8.slice(p, p + n)); },
  rtx_js_stash_count: function () { return Module.rtxStash.length; },
  rtx_js_stash_size: function (i) { return Module.rtxStash[i].length; },
  rtx_js_stash_copy: function (i, p) { HEAPU8.set(Module.rtxStash[i], p); },
  rtx_js_drop: function () { Module.rtxSnap = null; Module.rtxStash = null; },
});
