// ko.worker.js — owns the CrossPoint-KO WASM module. Receives commands:
//   {id, cmd:'load', epub:ArrayBuffer}
//   {id, cmd:'spec', spec:{...}}
//   {id, cmd:'build', spine}
//   {id, cmd:'render', page, spine}   (render one page, rebuild spine if needed)
//   {id, cmd:'pageCount'}             → current spine page count
// Replies {id, ok, ...}. ImageData buffers are transferable copies.
/* eslint-env worker */
importScripts('ko_xtch_wasm.js');   // defines createKoEngine (MODULARIZE)

let Module = null;         // wasm module instance
let api = null;            // C-export surface
let spineCount = 0;
let currentSpine = -1;
let currentPages = 0;
let currentSpec = null;
let builtKey = null;   // specKey of the spine currently built in the engine
let builtFontStamp = 0; // fontStamp at last build (font swaps force a rebuild)

const FONT_IDS = {
  ridibatang: -183759873,   // RIDIBATANG_14_FONT_ID (KO build default)
  kopub: -1446433084,       // KOPUB_14_FONT_ID
  custom: -999999,          // CUSTOM_FONT_ID (runtime .epdfont)
};

let customFontBytes = null;   // cached .epdfont bytes, re-applied after book loads
let customFontName = 'custom';
// Monotonic font stamp: bumped on every loadFont/clearFont. Pagination is fixed
// at spine build time, and the worker spec (font: 'custom') does NOT change when
// a different custom face is loaded — without the stamp, render() would skip the
// rebuild and page breaks would stay stale (wrong wraps for a new size/weight).
let fontStamp = 0;

// Background whole-book conversion result (held in worker, pulled on demand).
let warmBytes = null;
let warmPages = 0;
let warmRaw = 0;
let warmMode = 1;
let warmXtcz = false;
let warmToken = 0;
let warmRunning = false;

// per-command wall-clock timing (ms) — n = cumulative invocations
let cmdTimes = {};
function tick(cmd) {
  if (!cmdTimes[cmd]) cmdTimes[cmd] = { n: 0 };
  cmdTimes[cmd].t0 = performance.now();
}
function tock(cmd) {
  const e = cmdTimes[cmd];
  if (e) { e.ms = performance.now() - e.t0; e.n += 1; }
}

function defaultSpec() {
  return {
    lineCompression: 1.2,
    paragraphIndent: 0,
    characterWrap: 1,
    paragraphAlignment: 0,   // JUSTIFIED
    extraParagraphSpacing: 1,
    hyphenation: 0,
    embeddedStyle: 1,
    imageRendering: 0,
    textAa: 1,
    screenMargin: 5,
    font: 'ridibatang',      // reader face preset
  };
}

function specKey(spec) {
  return JSON.stringify(spec);
}

function post(id, ok, payload, transfer) {
  self.postMessage(Object.assign({ id, ok }, payload), transfer || []);
}

// Copy a 48000-byte plane from wasm heap into a JS Uint8Array.
function copyPlane(kind) {
  const ptr = api._ko_plane_ptr(kind);
  const size = api._ko_plane_size(kind);
  if (!ptr || size <= 0) return null;
  return new Uint8Array(api.HEAPU8.buffer.slice(ptr, ptr + size));
}

// Compose the 3 physical (800x480) planes into a logical portrait 480x800
// grayscale image per the device decode contract:
//   value = !ink?white : lsb?dark-grey : msb?light-grey : black
//   logical(x,y) ← physical(phyX=y, phyY=479-x)
const colBytes = 100; // physical row width in bytes
const GRAY = [255, 128, 205, 0]; // white, dark-grey, light-grey, black

const bitAt = (buf, phyX, phyY) => (buf[phyY * colBytes + (phyX >> 3)] >> (7 - (phyX & 7))) & 1;

// 1-bit (XTC / XTG) preview: the file stores ONLY the BW plane — no AA greys.
// Bit 0 = black on the device; this is byte-exactly what addMonoPage() packs.
function composeMono(bw) {
  const img = new ImageData(480, 800);
  const d = img.data;
  for (let y = 0; y < 800; y++) {        // logical row
    for (let x = 0; x < 480; x++) {      // logical col
      const ink = bitAt(bw, y, 479 - x) === 0;
      const g = ink ? 0 : 255;           // black : white
      const o = (y * 480 + x) * 4;
      d[o] = g; d[o + 1] = g; d[o + 2] = g; d[o + 3] = 255;
    }
  }
  return img;
}

function composePage(bw, lsb, msb) {
  const img = new ImageData(480, 800);
  const d = img.data;

  for (let y = 0; y < 800; y++) {        // logical row
    for (let x = 0; x < 480; x++) {      // logical col
      const phyX = y;
      const phyY = 479 - x;
      const ink = bitAt(bw, phyX, phyY) === 0;
      let v;
      if (!ink) v = 0;
      else if (bitAt(lsb, phyX, phyY) === 1) v = 1;   // dark grey
      else if (bitAt(msb, phyX, phyY) === 1) v = 2;   // light grey
      else v = 3;                                      // black
      const o = (y * 480 + x) * 4;
      const g = GRAY[v];
      d[o] = g; d[o + 1] = g; d[o + 2] = g; d[o + 3] = 255;
    }
  }
  return img;
}

async function init() {
  // Emscripten MODULARIZE: ko_xtch_wasm.js defines createKoEngine in scope.
  const factory = self.createKoEngine;
  Module = await factory();
  api = Module;
  // deterministic viewport: engine computes from margins; init with full logical
  api._ko_init(464, 778); // will be fixed up by ko_set_margins on first spec
  return true;
}

// Engine build-state invalidation. currentSpine/currentPages/builtKey describe
// what the ENGINE has built; a whole-book export runs every spine through the
// engine, so its end state is undefined — force a real rebuild next render.
function invalidateEngine() {
  currentSpine = -1;
  currentPages = 0;
  builtKey = null;
}

// Export the whole loaded book with the current spec, spine by spine (progress
// callback), returning the finished container bytes for download.
// opts.xtcz=true wraps the container in XTZ4 (LZ4) before copying out; the
// pre-wrap container size is reported as rawBytes (no JS-side unwrap needed).
// opts.cancelCheck() is polled between spines — returning true aborts early so
// a superseded background warm stops burning CPU (the half-built container is
// discarded; the next export begin() resets it).
async function exportWholeBook(opts, onProgress) {
  const xtcz = !!(opts && opts.xtcz);
  const cancelCheck = (opts && opts.cancelCheck) || null;
  tick('exportBegin');
  const spines = api._ko_export_begin();
  tock('exportBegin');
  if (spines < 0) throw new Error('export_begin failed');
  let total = 0;
  for (let s = 0; s < spines; s++) {
    tick('exportSpine');
    const n = api._ko_export_spine(s);
    tock('exportSpine');
    if (n < 0) throw new Error('export spine ' + s + ' failed');
    total += n;
    if (onProgress) onProgress(s + 1, spines, total);
    // let the busy ticker breathe between spines on giant books
    if ((s & 3) === 3) {
      await new Promise((r) => setTimeout(r, 0));
      if (cancelCheck && cancelCheck()) return { cancelled: true, pages: total };
    }
  }
  tick('exportFinish');
  const pages = api._ko_export_finish();
  tock('exportFinish');
  if (pages < 0) throw new Error('export_finish failed');
  const rawBytes = api._ko_xtch_size();   // pre-wrap container size
  if (xtcz) {
    tick('xtczWrap');
    api._ko_xtcz_wrap();          // g_xtchOut → XTZ4 container in place
    tock('xtczWrap');
  }
  const ptr = api._ko_xtch_ptr();
  const size = api._ko_xtch_size();
  if (!ptr || size <= 0) throw new Error('export produced no bytes');
  const out = new Uint8Array(api.HEAPU8.buffer.slice(ptr, ptr + size));
  api._ko_xtch_release();
  return { file: out, pages, bytes: out.byteLength, spines, xtcz, rawBytes: rawBytes || 0 };
}

// init runs once; every handler awaits it before touching the api
const initPromise = init();

// Apply the reader-face choice to the engine. Custom fonts need their bytes
// re-sent after every book load (ko_load_epub clears the in-memory FS).
function applyFont(name) {
  const fontId = FONT_IDS[name];
  if (fontId === undefined) return false;
  if (name === 'custom') {
    if (!customFontBytes) return false;   // nothing uploaded yet → keep default
    const fp = api._malloc(customFontBytes.length);
    api.HEAPU8.set(customFontBytes, fp);
    const rc = api._ko_load_epdfont(fp, customFontBytes.length, customFontName);
    api._free(fp);
    return rc === 0;
  }
  return api._ko_set_font(fontId) === 0;
}

self.onmessage = async (ev) => {
  const { id, cmd } = ev.data;
  try {
    await initPromise;   // engine ready before any command
    switch (cmd) {      case 'ping': {
        const vp = api._ko_version();
        const ver = api.UTF8ToString ? api.UTF8ToString(vp) : String(vp);
        post(id, true, { version: ver });
        break;
      }

      case 'load': {
        if (!api) await init();
        const epub = ev.data.epub;               // ArrayBuffer
        const bytes = new Uint8Array(epub);
        const bufPtr = api._malloc(bytes.length);
        api.HEAPU8.set(bytes, bufPtr);
        spineCount = api._ko_load_epub(bufPtr, bytes.length, '/book.epub');
        api._free(bufPtr);
        if (spineCount < 0) {
          post(id, false, { error: 'Epub::load failed' });
          return;
        }
        // spine labels
        const hrefs = [];
        for (let s = 0; s < spineCount; s++) {
          const p = api._ko_get_spine_href(s);
          hrefs.push(api.UTF8ToString(p));
        }
        // title
        const tbuf = api._malloc(512);
        api._ko_get_title(tbuf, 512);
        const title = api.UTF8ToString(tbuf);
        api._free(tbuf);
        currentSpine = -1;
        currentPages = 0;
        // a new book cleared the in-memory FS: re-apply the custom font if one
        // is loaded, so the reader face survives chapter navigation
        if (customFontBytes && currentSpec && currentSpec.font === 'custom') {
          applyFont('custom');
        }
        post(id, true, { spineCount, hrefs, title });
        break;
      }

      case 'spec': {
        tick('spec');
        currentSpec = Object.assign(defaultSpec(), ev.data.spec || {});
        // forward every knob to the engine
        api._ko_set_line_compression(currentSpec.lineCompression);
        api._ko_set_paragraph_indent(currentSpec.paragraphIndent);
        api._ko_set_character_wrap(currentSpec.characterWrap);
        api._ko_set_paragraph_alignment(currentSpec.paragraphAlignment);
        api._ko_set_extra_paragraph_spacing(currentSpec.extraParagraphSpacing);
        api._ko_set_hyphenation(currentSpec.hyphenation);
        api._ko_set_embedded_style(currentSpec.embeddedStyle);
        api._ko_set_image_rendering(currentSpec.imageRendering);
        api._ko_set_text_aa(currentSpec.textAa);
        tick('applyFont');
        applyFont(currentSpec.font);   // reader face (default ridibatang)
        tock('applyFont');
        // margins: device = viewable (9/3/3/3) + screenMargin
        const m = currentSpec.screenMargin;
        api._ko_set_margins(9 + m, 3 + m, 3 + m, 3 + m);
        tock('spec');
        post(id, true, { specKey: specKey(currentSpec) });
        break;
      }

      case 'build': {
        tick('build');
        const n = api._ko_build_spine(ev.data.spine);
        tock('build');
        if (n < 0) {
          const ep = api._ko_error();
          const msg = api.UTF8ToString(ep) || 'build failed';
          post(id, false, { error: 'build failed: ' + msg });
          return;
        }
        currentSpine = ev.data.spine;
        currentPages = n;
        post(id, true, { pages: n });
        break;
      }

      case 'render': {
        const key = specKey(currentSpec || {});
        const wantMono = ev.data.mode === 0;   // 1-bit XTC preview (BW plane only)
        // rebuild if spine, spec, OR loaded font changed since the last build
        // (a custom-font swap changes glyphs/metrics but not the worker spec)
        if (ev.data.spine !== currentSpine || key !== builtKey ||
            fontStamp !== builtFontStamp) {
          tick('rebuild');
          const n = api._ko_build_spine(ev.data.spine);
          tock('rebuild');
          if (n < 0) {
            const ep = api._ko_error();
            const msg = api.UTF8ToString(ep) || 'build failed';
            post(id, false, { error: 'build failed: ' + msg });
            return;
          }
          currentSpine = ev.data.spine;
          currentPages = n;
          builtKey = key;
          builtFontStamp = fontStamp;
        }
        let page = ev.data.page;
        if (page < 0) page = 0;
        if (page >= currentPages) page = currentPages - 1;
        tick('renderPage');
        const rc = api._ko_render_page(page);
        tock('renderPage');
        if (rc !== 0) { post(id, false, { error: 'render failed' }); return; }
        tick('copyPlanes');
        const bw = copyPlane(0);
        const lsb = copyPlane(1);
        const msb = copyPlane(2);
        tock('copyPlanes');
        tick('compose');
        // compose through the SAME quantization the encoder uses for the chosen
        // mode: 1-bit → BW plane only (no AA greys), 2-bit → full 4-level
        const img = wantMono ? composeMono(bw) : composePage(bw, lsb, msb);
        tock('compose');
        const tx = img.data.buffer;
        post(id, true, { page, pages: currentPages, image: tx, mono: wantMono }, [tx]);
        break;
      }

      case 'cover': {
        // generate cover BMP in the engine and return raw BMP bytes
        const rc = api._ko_generate_cover(ev.data.kind || 0);
        if (rc !== 0) {
          const ep = api._ko_error();
          post(id, false, { error: 'cover gen: ' + (api.UTF8ToString(ep) || 'failed') });
          return;
        }
        const ptr = api._ko_cover_ptr();
        const size = api._ko_cover_size();
        const bytes = new Uint8Array(api.HEAPU8.buffer.slice(ptr, ptr + size));
        const tx = bytes.buffer;
        post(id, true, { cover: tx }, [tx]);
        break;
      }

      case 'loadFont': {
        // ev.data.epdfont: ArrayBuffer, ev.data.name: string
        tick('loadFont');
        customFontBytes = new Uint8Array(ev.data.epdfont);
        customFontName = (ev.data.name || 'custom').replace(/[^\w-]/g, '') || 'custom';
        fontStamp++;
        tick('applyFont');
        const ok = applyFont('custom');
        tock('applyFont');
        tock('loadFont');
        if (!ok) {
          const ep = api._ko_error();
          post(id, false, { error: 'epdfont load: ' + (api.UTF8ToString(ep) || 'failed') });
          return;
        }
        post(id, true, { font: 'custom', fontStamp });
        break;
      }

      case 'clearFont': {
        customFontBytes = null;
        fontStamp++;
        api._ko_clear_custom_font();
        currentSpec = Object.assign(currentSpec || defaultSpec(), { font: 'ridibatang' });
        post(id, true, { font: 'ridibatang', fontStamp });
        break;
      }

      case 'warm': {
        // Background whole-book conversion (settings settled → pre-render the
        // book so the file is ready when the user hits export). Mirrors the
        // exportBook pipeline but WITHOUT transferring megabytes back to the
        // app: the finished container stays in the worker, and the app pulls
        // it only on download via 'fetchWarm'. warmToken lets a newer warm
        // supersede an in-flight one.
        const myTok = ev.data.token || 0;
        if (myTok < warmToken) { post(id, true, { warm: 'superseded' }); break; }
        warmToken = Math.max(warmToken, myTok);   // adopt: newer warms supersede
        if (warmRunning) { post(id, true, { warm: 'busy' }); break; }
        warmRunning = true;
        // The warm pass drives the engine through every spine; the JS tracker
        // (currentSpine) can't follow, so any render that interleaves between
        // the warm's spine yields must force a rebuild of its own spine.
        invalidateEngine();
        try {
          tick('warm');
          api._ko_export_set_mode(ev.data.mode === 0 ? 0 : 1);
          const res = await exportWholeBook({
            xtcz: !!ev.data.xtcz,
            cancelCheck: () => myTok < warmToken,   // a newer warm superseded us
          });
          if (myTok < warmToken) { post(id, true, { warm: 'superseded' }); break; }
          if (res.cancelled) { post(id, true, { warm: 'cancelled' }); break; }
          warmBytes = res.file;
          warmPages = res.pages;
          warmRaw = res.rawBytes || 0;
          warmMode = ev.data.mode === 0 ? 0 : 1;
          warmXtcz = !!ev.data.xtcz;
          tock('warm');
          invalidateEngine();
          post(id, true, { warm: 'ready', pages: res.pages });
        } finally {
          warmRunning = false;
        }
        break;
      }

      case 'warmCancel': {
        // settings changed mid-warm → tell the in-flight warm to abort at the
        // next spine yield (it polls myTok < warmToken). No new warm starts;
        // the app will fire one again once the user settles.
        warmToken++;
        post(id, true, { warmCancel: true });
        break;
      }

      case 'fetchWarm': {
        if (!warmBytes) { post(id, false, { error: 'no warm bytes' }); break; }
        const tx = warmBytes.buffer.slice(warmBytes.byteOffset,
                                          warmBytes.byteOffset + warmBytes.byteLength);
        const meta = { pages: warmPages, mode: warmMode, xtcz: warmXtcz, rawBytes: warmRaw };
        warmBytes = null;   // hand over once
        post(id, true, Object.assign({ file: tx }, meta), [tx]);
        break;
      }

      case 'warmState': {
        post(id, true, { has: !!warmBytes, pages: warmPages, mode: warmMode, xtcz: warmXtcz });
        break;
      }

      case 'exportBook': {
        // whole-book export with the current spec; 1-bit XTC or 2-bit XTCH per
        // mode (xtcz=true wraps in XTZ4/LZ4); progress posts (no op id)
        // A background warm may be driving the engine: it shares the C export
        // buffer, so supersede it and wait for it to stop (it polls the token
        // between spines and aborts within a few yields).
        if (warmRunning) {
          warmToken++;
          while (warmRunning) await new Promise((r) => setTimeout(r, 15));
        }
        api._ko_export_set_mode(ev.data.mode === 0 ? 0 : 1);
        const opts = { xtcz: !!ev.data.xtcz };
        const res = await exportWholeBook(opts, (spine, ofSpines, pages) => {
          self.postMessage({ progress: true, spine, ofSpines, pages });
        });
        // the export loop built+rendered every spine through the engine, so
        // per-spine engine state is undefined afterwards — force a rebuild on
        // the next engine render (keeps page flips after export safe)
        invalidateEngine();
        // hand the finished bytes to the app as a transferable for download
        const tx = res.file.buffer.slice(res.file.byteOffset,
                                         res.file.byteOffset + res.file.byteLength);
        post(id, true, { file: tx, mode: ev.data.mode === 0 ? 0 : 1,
                         pages: res.pages, spines: res.spines,
                         xtcz: res.xtcz, rawBytes: res.rawBytes }, [tx]);
        break;
      }

      case 'pageCount': {
        post(id, true, { pages: currentPages, spine: currentSpine });
        break;
      }

      case 'stats': {
        post(id, true, { times: cmdTimes });
        break;
      }

      default:
        post(id, false, { error: 'unknown cmd ' + cmd });
    }
  } catch (e) {
    // A throw here usually means the wasm runtime aborted (OOM / bad_alloc
    // under -fno-exceptions). The engine is poisoned after an abort — flag
    // fatal so the app respawns the worker instead of retrying a dead engine.
    const msg = String(e && e.message || e);
    const fatal = /abort|runtime|out of memory|allocation/i.test(msg);
    try { post(id, false, { error: msg, fatal }); } catch (_) {}
    if (fatal) {
      // give the reply a moment to flush, then die loudly (app respawns)
      setTimeout(() => { throw new Error('engine aborted: ' + msg); }, 0);
    }
  }
};
