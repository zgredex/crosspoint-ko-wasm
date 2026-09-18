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
// Composed-frame cache: a navigation that repeats a (spine, page, mode, spec, font) tuple
// reuses the frame instead of re-rendering and re-composing. Bounded by bytes because a frame
// is 1.5 MB (480*800*4) and the wasm heap is 2 GB - 128 frames is ~192 MB.
const FRAME_CACHE_BUDGET = 192 * 1024 * 1024;
const frameCache = new Map();          // key -> { data: Uint8ClampedArray }, insertion ordered
let frameCacheBytes = 0;

function frameCacheGet(key) {
  const hit = frameCache.get(key);
  if (!hit) return null;
  frameCache.delete(key);              // refresh recency
  frameCache.set(key, hit);
  return hit;
}

function frameCachePut(key, img) {
  // Store a COPY: the buffer handed to post() is transferred, which detaches it.
  const copy = new Uint8ClampedArray(img.data.length);
  copy.set(img.data);
  if (frameCache.has(key)) frameCacheBytes -= frameCache.get(key).data.length;
  frameCache.set(key, { data: copy });
  frameCacheBytes += copy.length;
  while (frameCacheBytes > FRAME_CACHE_BUDGET && frameCache.size > 1) {
    const oldest = frameCache.keys().next().value;
    frameCacheBytes -= frameCache.get(oldest).data.length;
    frameCache.delete(oldest);
  }
}

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

// ---------------------------------------------------------------------------
// Text-viewport derivation for the EXPORTED FILE.
//
// GfxRenderer::getOrientedViewableTRBL() supplies the viewable margins (portrait
// 9/3/3/3) and the reader adds screenMargin to every side:
//
//   t += screenMargin;  r += screenMargin;  b += screenMargin;  l += screenMargin;
//   viewport = screen - (l+r, t+b)
//
// DELIBERATE DIVERGENCE — and it is a hard product constraint: the device's EPUB
// reader ALSO reserves the status-bar lane (EpubReaderActivity::render() does
// `b += max(screenMargin, statusBarHeight)`, statusBarHeight = 19 for shipped
// defaults). We do NOT, because an XTC/XTCH page is a finished BITMAP and the UI
// must never be encoded into it:
//
//   * nothing of the UI is drawn into the page — that is the point,
//   * the device composites its own chrome at read time (XtcReaderActivity +
//     xtcStatusBarMode, default XTC_STATUS_BAR_HIDE),
//   * reserving the lane here would bake a UI-driven gap into page geometry, so
//     one book would paginate differently based on a UI setting the file does
//     not contain.
//
// Net: default margins 14/8/8/8 → viewport 464x778, matching the official
// converter byte-for-byte. The reader's live-layout numbers (464x764) are a
// *preview* concern only — chrome composited by the browser, never exported.
// ---------------------------------------------------------------------------
const VIEWABLE_MARGIN = { top: 9, right: 3, bottom: 3, left: 3 };  // GfxRenderer::VIEWABLE_MARGIN_*
const SCREEN_W = 480;
const SCREEN_H = 800;

function marginsFor(spec) {
  const m = spec.screenMargin;
  return {
    top: VIEWABLE_MARGIN.top + m,
    right: VIEWABLE_MARGIN.right + m,
    left: VIEWABLE_MARGIN.left + m,
    bottom: VIEWABLE_MARGIN.bottom + m,
  };
}

function viewportFor(spec) {
  const mg = marginsFor(spec);
  return { width: SCREEN_W - mg.left - mg.right, height: SCREEN_H - mg.top - mg.bottom, margins: mg };
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
const GRAY = [255, 128, 205, 0]; // white, dark-grey, light-grey, black


// 1-bit (XTC / XTG) preview: the file stores ONLY the BW plane — no AA greys.
// Bit 0 = black on the device; this is byte-exactly what addMonoPage() packs.
// The pixels are composed inside the engine now: ko_compose_rgba() fills an engine-owned
// 480x800 RGBA buffer and we wrap it as a ZERO-COPY view. No JS pixel loop exists.
// The buffer can move if wasm memory grows, so the pointer is fetched AFTER composing and
// the view is rebuilt every call. putImageData reads it synchronously, so the view stays valid.
// tick/tock are the worker's existing stage timers; used defensively so this stays valid if
// they are not in scope here.
function _stage(name, fn) {
  const has = typeof tick === 'function' && typeof tock === 'function';
  if (has) tick(name);
  try { return fn(); } finally { if (has) tock(name); }
}

// A view over wasm memory CANNOT be transferred through postMessage - the browser throws
// 'cannot transfer WebAssembly ArrayBuffer', because wasm memory is non-detachable by spec.
// So the frame is copied out into a normal ArrayBuffer here: one native typed-array copy
// (~1.5 MB), not per-pixel work. The engine-side compose and its single 32-bit store per
// pixel are unaffected.
function _frameFromEngine() {
  const src = new Uint8ClampedArray(api.HEAPU8.buffer, api._ko_rgba_ptr(), 480 * 800 * 4);
  const img = new ImageData(480, 800);
  img.data.set(src);
  return img;
}

function composeMono() {
  _stage('compose.mono', () => {
    if (api._ko_compose_rgba(1) !== 0) throw new Error('engine compose failed (mono)');
  });
  return _stage('compose.view', _frameFromEngine);
}

function composePage() {
  _stage('compose.page', () => {
    if (api._ko_compose_rgba(0) !== 0) throw new Error('engine compose failed');
  });
  return _stage('compose.view', _frameFromEngine);
}

async function init() {
  // Emscripten MODULARIZE: ko_xtch_wasm.js defines createKoEngine in scope.
  const factory = self.createKoEngine;
  // Cache-bust the engine binaries: the emscripten glue fetches the .wasm with no version
  // query, so without this the edge serves a previously cached engine indefinitely and no
  // engine change can ever reach a returning browser.
  Module = await factory({ locateFile: (path) => (path.indexOf('.wasm') >= 0 ? path + '?v=13' : path) });
  api = Module;
  // deterministic viewport: engine computes from margins; init with full logical
  api._ko_init(464, 778); // will be fixed up by ko_set_margins on first spec
  return true;
}

// Engine build-state invalidation. currentSpine/currentPages/builtKey describe
// what the ENGINE has built; a whole-book export runs every spine through the
// engine, so its end state is undefined — force a real rebuild next render.
function invalidateEngine() {
  frameCache.clear();
  frameCacheBytes = 0;
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
        // Firmware gates hyphenation on word-wrap mode (CrossPointSettings::
        // readerRenderSpec): hyphenationEnabled = hyphenationEnabled && characterWrap == 0.
        // Character wrap can already break anywhere, so hyphenation is inert there.
        api._ko_set_hyphenation((currentSpec.hyphenation && currentSpec.characterWrap === 0) ? 1 : 0);
        api._ko_set_embedded_style(currentSpec.embeddedStyle);
        api._ko_set_image_rendering(currentSpec.imageRendering);
        api._ko_set_text_aa(currentSpec.textAa);
        tick('applyFont');
        applyFont(currentSpec.font);   // reader face (default ridibatang)
        tock('applyFont');
        // margins: viewable area + screenMargin on all four sides (no UI reserve)
        const mg = marginsFor(currentSpec);
        api._ko_set_margins(mg.top, mg.right, mg.bottom, mg.left);
        tock('spec');
        post(id, true, {
          specKey: specKey(currentSpec),
          margins: mg,
          viewport: { width: SCREEN_W - mg.left - mg.right, height: SCREEN_H - mg.top - mg.bottom },
        });
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
        // Repeat navigation: the composed frame is a pure function of these five inputs, and
        // the cached copy is already out of wasm memory.
        const frameKey = currentSpine + ':' + page + ':' + (wantMono ? 1 : 0) + ':' + builtKey + ':' + builtFontStamp;
        const cachedFrame = frameCacheGet(frameKey);
        if (cachedFrame) {
          const reply = new Uint8ClampedArray(cachedFrame.data.length);
          reply.set(cachedFrame.data);
          post(id, true, { page, pages: currentPages, image: reply.buffer, mono: wantMono, cached: true },
               [reply.buffer]);
          break;
        }

        tick('renderPage');
        const rc = api._ko_render_page(page);
        tock('renderPage');
        if (rc !== 0) { post(id, false, { error: 'render failed' }); return; }
        // No plane copies: the compose runs inside the engine and reads the engine's own
        // planes, so copying 3 x 48 KB out of wasm here was pure waste.
        tick('compose');
        // compose through the SAME quantization the encoder uses for the chosen
        // mode: 1-bit → BW plane only (no AA greys), 2-bit → full 4-level
        const img = wantMono ? composeMono() : composePage();
        tock('compose');
        frameCachePut(frameKey, img);
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
