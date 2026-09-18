// app.js — UI glue for the CrossPoint-KO EPUB preview. Talks to ko.worker.js.
(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);

  const els = {
    file: $('epubFile'), openBtn: $('openBtn'), status: $('status'),
    page: $('page'), loading: $('loading'), pagerEl: $('pager'),
    prevBtn: $('prevBtn'), nextBtn: $('nextBtn'), pageInfo: $('pageInfo'),
    spineSel: $('spineSel'), coverBtn: $('coverBtn'),
    lineCompression: $('lineCompression'), paragraphAlignment: $('paragraphAlignment'),
    paragraphIndent: $('paragraphIndent'), extraParagraphSpacing: $('extraParagraphSpacing'),
    characterWrap: $('characterWrap'), hyphenation: $('hyphenation'),
    embeddedStyle: $('embeddedStyle'), textAa: $('textAa'),
    viewportOut: $('viewportOut'),
    screenMargin: $('screenMargin'), screenMarginOut: $('screenMarginOut'),
    imageRendering: $('imageRendering'), zoom: $('zoom'), zoomOut: $('zoomOut'),
    resetBtn: $('resetBtn'),
    fontPreset: $('fontPreset'), fontUpload: $('fontUpload'),
    fontFile: $('fontFile'), fontName: $('fontName'), fontSize: $('fontSize'), fontWeight: $('fontWeight'),
    fontSizeOut: $('fontSizeOut'), fontWeightOut: $('fontWeightOut'),
    fontHangul: $('fontHangul'),
    fontIntervals: $('fontIntervals'), fontSpacePx: $('fontSpacePx'), fontSpacePxOut: $('fontSpacePxOut'),
    fontConvStatus: $('fontConvStatus'),
    exportSeg: $('exportPanel').querySelector('.exportSeg'),
    exportName: $('exportName'), exportStatus: $('exportStatus'),
    downloadBtn: $('downloadBtn'), lz4Wrap: $('lz4Wrap'),
  };

  let book = null;                // {title, spineCount, hrefs}
  let state = { spine: 0, page: 0, pages: 0, mode: 1 };  // mode: 0=1-bit XTC, 1=2-bit XTCH
  let viewingCover = false;       // canvas shows cover BMP, not a page
  let renderToken = 0;            // invalidates stale renders
  let repaintTimer = null;        // trailing debounce → visible-page repaint
  let customFontLoaded = false;   // a runtime .epdfont is active in the engine
  let exporting = false;          // export in flight → knobs disabled

  // ---- custom-font hot path ----
  // Every font knob change (size/weight/space/hangul/intervals/file) schedules
  // an auto re-conversion after the user stops moving (trailing debounce) and
  // then re-renders the visible page. Server-side convert is memoized by full
  // param set, so scrubbing back to a previous value is an instant cache hit.
  let fontTimer = null;           // trailing debounce for auto re-conversion
  let fontConvertToken = 0;       // supersede: stale conversions are dropped
  let fontConverting = false;     // a conversion+apply is in flight
  let lastFontSig = '';           // signature of the face currently applied

  // ---- worker plumbing (hardened: timeouts + crash surfacing + respawn) ----
  const CALL_TIMEOUT_MS = 90000;  // generous: giant spine rebuilds take a while
  let nextId = 1;
  const pending = new Map();      // id -> {resolve, reject, timer}
  let worker = null;

  function spawnWorker() {
    // Resolve against the page's directory, not the page file — opening
    // /index.html vs / must both yield /ko.worker.js.
    const base = location.pathname.slice(0, location.pathname.lastIndexOf('/') + 1);
    const w = new Worker(base + 'ko.worker.js?v=10');
    w.onmessage = (ev) => {
      const m = ev.data;
      // worker progress reports carry no id — surface them live
      if (m && m.progress && els.exportStatus) {
        els.exportStatus.textContent = 'exporting spine ' + m.spine + '/' + m.ofSpines +
          ' (' + m.pages + ' pages so far)…';
        return;
      }
      const p = pending.get(m.id);
      if (!p) return;
      pending.delete(m.id);
      clearTimeout(p.timer);
      if (m.fatal) {
        // engine aborted (OOM etc.) — respawn, then reject this call
        setStatus('⚠ engine aborted (' + (m.error || '') + ') — auto-restarting…', true);
        p.reject(new Error(m.error || 'engine aborted'));
        respawn();
        return;
      }
      m.ok ? p.resolve(m) : p.reject(new Error(m.error || 'worker error'));
    };
    w.onerror = (e) => {
      e.preventDefault();
      setStatus('⚠ engine worker crashed (' + (e.message || 'unknown') + ') — auto-restarting…', true);
      respawn();
    };
    w.onmessageerror = () => {
      setStatus('⚠ engine worker message error — auto-restarting…', true);
      respawn();
    };
    return w;
  }

  let respawning = false;
  function respawn() {
    if (respawning) return;
    respawning = true;
    // kill worker + reject in-flight calls so no promise hangs forever
    try { worker.terminate(); } catch (_) {}
    worker = spawnWorker();
    const err = new Error('engine restarted');
    for (const [, p] of pending) { clearTimeout(p.timer); p.reject(err); }
    pending.clear();
    book = null;
    bootEngine().then(() => {
      respawning = false;
      if (els.file.files && els.file.files[0]) {
        els.file.files[0].arrayBuffer().then((buf) => loadBook(buf, els.file.files[0].name));
      } else {
        const q = new URLSearchParams(location.search);
        const auto = q.get('epub');
        if (auto) fetch(auto).then((r) => r.arrayBuffer()).then((buf) => loadBook(buf, auto.split('/').pop()));
      }
    }).catch(() => { respawning = false; });
  }

  function call(cmd, payload = {}, transfer, timeoutMs) {
    const id = nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        pending.delete(id);
        setStatus('⚠ engine call "' + cmd + '" timed out after ' +
                  Math.round((timeoutMs || CALL_TIMEOUT_MS) / 1000) + 's — restarting engine…', true);
        respawn();
        reject(new Error('timeout: ' + cmd));
      }, timeoutMs || CALL_TIMEOUT_MS);
      pending.set(id, { resolve, reject, timer });
      worker.postMessage(Object.assign({ id, cmd }, payload), transfer || []);
    });
  }
  window.__call = call;   // debug hook (stats etc.)

  function setStatus(msg, isErr) {
    els.status.textContent = msg;
    // long titles truncate in the header — hover reveals the full message
    els.status.title = (msg || '').replace(/\s+/g, ' ').trim();
    els.status.className = 'status' + (isErr ? ' err' : ' ok');
  }

  async function bootEngine() {
    const r = await call('ping', {}, [], 15000);
    setStatus('engine ' + r.version + ' ready — load an EPUB');
  }

  // ---- spec snapshot ----
  function readSpec() {
    return {
      lineCompression: parseFloat(els.lineCompression.value),
      paragraphAlignment: parseInt(els.paragraphAlignment.value, 10),
      paragraphIndent: els.paragraphIndent.checked ? 1 : 0,
      extraParagraphSpacing: els.extraParagraphSpacing.checked ? 1 : 0,
      characterWrap: els.characterWrap.checked ? 1 : 0,
      hyphenation: els.hyphenation.checked ? 1 : 0,
      embeddedStyle: els.embeddedStyle.checked ? 1 : 0,
      textAa: els.textAa.checked ? 1 : 0,
      screenMargin: parseInt(els.screenMargin.value, 10),
      imageRendering: parseInt(els.imageRendering.value, 10),
      // 'custom' only when a runtime font is actually loaded; otherwise the
      // worker would silently keep the default while the UI claims custom.
      font: els.fontPreset.value === 'custom'
        ? (customFontLoaded ? 'custom' : 'ridibatang')
        : els.fontPreset.value,
    };
  }

  function applyDefaults() {
    els.lineCompression.value = '1.20';
    els.paragraphAlignment.value = '0';
    els.paragraphIndent.checked = false;      // device default: off
    els.extraParagraphSpacing.checked = true;
    els.characterWrap.checked = true;
    els.hyphenation.checked = false;
    els.embeddedStyle.checked = true;
    els.textAa.checked = true;                // device default: on
    els.screenMargin.value = '5';
    els.screenMarginOut.textContent = '5';
    // NOTE: the device's status-bar settings deliberately do NOT feed layout.
    // The reader reserves a 19 px status-bar lane on-device, but an XTC/XTCH
    // page is a finished bitmap and the UI must not be encoded into it — the
    // device composites its own chrome at read time (xtcStatusBarMode, default
    // hidden). Keeping the file UI-free is why there is no status-bar control
    // in this panel.
    els.imageRendering.value = '0';
    els.fontPreset.value = 'ridibatang';
    els.fontSize.value = '14';
    els.fontSizeOut.textContent = '14 pt';
    els.fontWeight.value = '500';
    els.fontWeightOut.textContent = '500';
    els.fontSpacePx.value = '9';
    els.fontSpacePxOut.textContent = '9 px';
    els.fontHangul.checked = true;
    els.fontIntervals.value = '';
    els.fontName.value = 'custom';
    syncFontSeg();
    toggleFontPanel();
  }

  // ---- rendering ----
  // Parse the fork's 2bpp top-down BMP (palette: 0 black 1 dark 2 light 3 white).
  // Returns ImageData (cover 540x800, cropped-cover aspect).
  // BMP palette 0=black..3=white, packed as little-endian RGBA words so each pixel is one
  // 32-bit store. Previously the loop allocated a fresh [0,85,170,255] array per pixel and
  // did four byte stores.
  const BMP_GRAY32 = new Uint32Array([0xFF000000, 0xFF555555, 0xFFAAAAAA, 0xFFFFFFFF]);

  // Parse the fork's 2bpp top-down BMP (palette: 0 black 1 dark 2 light 3 white).
  // Returns ImageData (cover 540x800, cropped-cover aspect).
  function bmpToImageData(buf) {
    const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
    const view = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    const w = view.getInt32(18, true);
    const h = view.getInt32(22, true); // negative = top-down; writer uses -h
    const topDown = h < 0;
    const absH = Math.abs(h);
    const dataOff = view.getUint32(10, true);
    const bytesPerRow = Math.ceil((w * 2) / 8);
    const rowPad = (bytesPerRow % 4) ? (4 - (bytesPerRow % 4)) : 0;
    const img = new ImageData(w, absH);
    const rgba = new Uint32Array(img.data.buffer);
    for (let y = 0; y < absH; y++) {
      const srcY = topDown ? y : (absH - 1 - y);
      const rowStart = dataOff + srcY * (bytesPerRow + rowPad);
      let out = y * w;
      for (let x = 0; x < w; x++, out++) {
        const b = u8[rowStart + (x >> 2)];
        rgba[out] = BMP_GRAY32[(b >> (6 - ((x & 3) << 1))) & 3];
      }
    }
    return img;
  }

  let busyTicker = null;
  function busy(msg) {
    els.loading.classList.remove('hidden');
    const t0 = Date.now();
    clearInterval(busyTicker);
    const tick = () => {
      const sec = Math.round((Date.now() - t0) / 1000);
      els.loading.textContent = msg + '… ' + sec + 's';
    };
    tick();
    busyTicker = setInterval(tick, 1000);
  }
  function idle() {
    clearInterval(busyTicker);
    busyTicker = null;
    els.loading.classList.add('hidden');
  }

  async function loadCover() {
    if (!book) return;
    ++renderToken;
    viewingCover = true;
    setStatus('cover…');
    busy('표지 generating cover');
    try {
      const r = await call('cover', { kind: 0 });
      const img = bmpToImageData(r.cover);
      drawCoverFitted(img);          // letterbox into the fixed 480×800 screen
      setStatus(book.title + ' — cover (표지)');
    } catch (e) {
      setStatus('cover error: ' + e.message, true);
      viewingCover = false;
    } finally {
      idle();
    }
  }

  // Draw a cover BMP scaled to fit inside the device screen (480×800 logical),
  // centered with white margins — mirrors how the X4 shows a full-screen cover.
  // The canvas size NEVER changes: the preview is a 1:1 device screen.
  function drawCoverFitted(img) {
    const ctx = els.page.getContext('2d');
    const SW = 480, SH = 800;
    if (els.page.width !== SW || els.page.height !== SH) {
      els.page.width = SW;
      els.page.height = SH;
    }
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(0, 0, SW, SH);
    const sc = Math.min(SW / img.width, SH / img.height);
    const dw = Math.max(1, Math.round(img.width * sc));
    const dh = Math.max(1, Math.round(img.height * sc));
    const dx = Math.round((SW - dw) / 2);
    const dy = Math.round((SH - dh) / 2);
    const t = document.createElement('canvas');
    t.width = img.width;
    t.height = img.height;
    t.getContext('2d').putImageData(img, 0, 0);
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(t, dx, dy, dw, dh);
    updateZoomCss();
  }

  function drawImageDirect(img) {
    const ctx = els.page.getContext('2d');
    ctx.putImageData(img, 0, 0);
  }

  async function pushSpec() {
    const r = await call('spec', { spec: readSpec() });
    // Surface the derived text viewport. It is the firmware's own arithmetic
    // (viewable margins + screenMargin + status-bar lane), so showing it makes
    // pagination behaviour auditable instead of mysterious.
    if (els.viewportOut && r && r.viewport) {
      const m = r.margins || {};
      els.viewportOut.textContent =
        'Text viewport 본문 영역: ' + r.viewport.width + '×' + r.viewport.height +
        ' px  ·  margins T/R/B/L ' + m.top + '/' + m.right + '/' + m.bottom + '/' + m.left +
        '  ·  no UI baked into the file (파일에는 UI 미포함)';
    }
  }

  async function refresh(keepPage = true, quiet = false) {
    if (!book) return;
    const tok = ++renderToken;
    viewingCover = false;
    if (!quiet) busy('rendering');   // scrub-path re-renders stay silent
    try {
      // spec first (idempotent cheap), then render (worker rebuilds spine on
      // change). The chosen export mode travels with the render so the preview
      // is quantized exactly like the file the mode produces.
      await pushSpec();
      const spine = Math.min(state.spine, book.spineCount - 1);
      const page = keepPage ? state.page : 0;
      const r = await call('render', { spine, page, mode: state.mode });
      if (tok !== renderToken) return;          // superseded
      state.spine = spine;
      state.page = r.page;
      state.pages = r.pages;
      if (els.page.width !== 480 || els.page.height !== 800) {
        els.page.width = 480;
        els.page.height = 800;
      }
      drawImage(r.image);
      updateZoomCss();
      updatePager();
      setStatus(book.title + ' — page ' + (r.page + 1) + '/' + r.pages +
                (r.mono ? ' · 1-bit preview' : ''));
    } catch (e) {
      if (tok === renderToken) setStatus('render error: ' + e.message, true);
    } finally {
      if (tok === renderToken) idle();
    }
  }

  function drawImage(buf) {
    const img = new ImageData(new Uint8ClampedArray(buf), 480, 800);
    drawImageDirect(img);
  }

  function updateZoomCss() {
    // View zoom, applied to the fixed 480x800 logical canvas:
    //   100%  → page sized to FIT the scroll viewport (whole page visible)
    //   <100% → page shrinks below fit
    //   >100% → page magnifies beyond fit; #canvasScroll scrolls to pan
    // This is a pure CSS view-scale — never a re-render, and the exported
    // file stays 480x800 device pixels at every zoom level.
    const z = (els.zoom.value || 100) / 100;
    const view = els.page.closest ? els.page.closest('#canvasScroll') : null;
    let fit = 0.6;
    if (view) {
      const w = view.clientWidth - 16;   // padding
      const h = view.clientHeight - 16;
      if (w > 0 && h > 0) fit = Math.min(w / 480, h / 800);
    }
    const s = fit * z;
    if (s <= 0) return;
    els.page.style.width = Math.round(480 * s) + 'px';
    els.page.style.height = Math.round(800 * s) + 'px';
  }

  function updatePager() {
    // page counter is spine-local (matches the device's chapter page display);
    // prev/next themselves cross chapter boundaries in goPage().
    els.pageInfo.textContent = viewingCover ? 'cover' : (state.pages > 0
      ? (state.page + 1) + ' / ' + state.pages
      : '– / –');
    const atBookStart = state.spine <= 0 && state.page <= 0;
    const atBookEnd = !!book && state.spine >= book.spineCount - 1 &&
                      state.pages > 0 && state.page >= state.pages - 1;
    els.prevBtn.disabled = !book || viewingCover || atBookStart;
    els.nextBtn.disabled = !book || atBookEnd;
  }

  // ---- custom font conversion & runtime load ----
  // Face preset is a segmented button row that drives a hidden <select>
  // (#fontPreset keeps the value; buttons only mirror it). Clicking a segment
  // updates the select then fires its change handler.
  function syncFontSeg() {
    const v = els.fontPreset.value;
    const seg = els.fontPreset.closest('.segRow');
    if (!seg) return;
    seg.querySelectorAll('.segBtn').forEach((b) => {
      const on = b.dataset.value === v;
      b.classList.toggle('active', on);
      b.setAttribute('aria-checked', on ? 'true' : 'false');
    });
  }

  function toggleFontPanel() {
    els.fontUpload.classList.toggle('hidden', els.fontPreset.value !== 'custom');
  }

  function fontStatus(msg, isErr) {
    els.fontConvStatus.textContent = msg;
    els.fontConvStatus.className = 'fontStatus' + (isErr ? ' err' : ' ok');
  }

  // ---- backend capability ------------------------------------------------
  // Custom-font conversion (TTF/OTF → .epdfont) is done by the local Python
  // server (server.py → tools/ttf_to_epdfont_fast.py, freetype). A static
  // deploy has no such endpoint, so probe once and say so plainly rather than
  // surfacing a bare "HTTP 404" in the middle of a knob scrub.
  let fontBackend = null;        // null = unknown, true = present, false = static deploy
  let fontBackendProbe = null;
  function hasFontBackend() {
    if (fontBackend !== null) return Promise.resolve(fontBackend);
    if (!fontBackendProbe) {
      fontBackendProbe = (async () => {
        try {
          const r = await fetch('/api/convert-font', { method: 'POST', body: new FormData() });
          // server.py answers this endpoint with JSON (even for a bad request);
          // a static host answers with its own 404 page
          fontBackend = (r.headers.get('content-type') || '').includes('application/json');
        } catch (_) {
          fontBackend = false;
        }
        if (!fontBackend) {
          fontBackendProbe = Promise.resolve(false);
          const note = document.getElementById('fontBackendNote');
          if (note) note.classList.remove('hidden');
          els.fontUpload.classList.add('noBackend');
        }
        return fontBackend;
      })();
    }
    return fontBackendProbe;
  }

  // POST the chosen font + tuning knobs to the local converter; returns the
  // .epdfont ArrayBuffer (or throws with the server's message).
  // Hot-path protocol: the FIRST convert of a picked file uploads the bytes and
  // the server registers a fontId (sha256); every later knob change sends only
  // the tiny params + fontId, so scrubbing never re-uploads the 3–10 MB font.
  let fontIdByFile = {};    // file fingerprint → registered fontId
  const fontFp = (f) => (f ? f.name + '|' + f.size + '|' + (f.lastModified || 0) : '');
  async function convertFont(fontFile) {
    if (!(await hasFontBackend())) {
      throw new Error('custom font conversion needs the local server (server.py) — '
        + '커스텀 폰트 변환은 로컬 서버에서만 동작합니다. '
        + 'This hosted build ships the WASM engine only.');
    }
    const fp = fontFp(fontFile);
    const fd = new FormData();
    const known = fontIdByFile[fp];
    if (known) fd.append('fontId', known);
    else fd.append('font', fontFile);
    fd.append('name', els.fontName.value.trim() || 'custom');
    fd.append('size', String(parseInt(els.fontSize.value, 10) || 14));
    fd.append('weight', String(parseInt(els.fontWeight.value, 10) || 500));
    fd.append('twoBit', '1');   // reader fonts are always 2-bit; output depth
                                // (XTC 1-bit vs XTCH 2-bit) is the export mode,
                                // applied at pack time — never at font build
    if (!els.fontHangul.checked) fd.append('noHangul', '1');
    const iv = els.fontIntervals.value.trim();
    if (iv) fd.append('extraIntervals', iv);
    const sp = parseInt(els.fontSpacePx.value, 10);
    if (!isNaN(sp) && sp >= 0) fd.append('spacePx', String(sp));
    const resp = await fetch('/api/convert-font', { method: 'POST', body: fd });
    const j = await resp.json().catch(() => ({}));
    if (!resp.ok || !j.ok) {
      // the server may have evicted the registered font → re-upload once
      if (known && /fontId|missing 'font'/.test(j.error || '')) {
        delete fontIdByFile[fp];
        return convertFont(fontFile);
      }
      const why = j.error || j.stderr || ('HTTP ' + resp.status);
      throw new Error('conversion failed: ' + String(why).slice(0, 400));
    }
    if (j.fontId) fontIdByFile[fp] = j.fontId;
    const b64 = j.epdfont;
    const bin = atob(b64);
    const buf = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
    j._bytes = buf.buffer;
    return j;
  }

  async function applyCustomFont(meta) {
    // hand the .epdfont bytes to the engine (worker caches + re-applies after loads)
    const r = await call('loadFont', { epdfont: meta._bytes, name: meta.name }, [meta._bytes]);
    els.fontPreset.value = 'custom';
    syncFontSeg();
    // update the face field so spec() pushes the right font; quiet re-render —
    // a font apply is usually one of a rapid scrub sequence, no busy flash
    await refresh(true, true);
    return r;
  }
  function populateSpines(hrefs) {
    els.spineSel.innerHTML = '';
    hrefs.forEach((h, i) => {
      const label = h.replace(/\.(xhtml|html|htm)$/i, '').replace(/[_]+/g, ' ') || ('spine ' + i);
      const opt = document.createElement('option');
      opt.value = i;
      opt.textContent = (i + 1) + '. ' + label;
      els.spineSel.appendChild(opt);
    });
    els.spineSel.disabled = false;
    els.spineSel.selectedIndex = 0;
  }

  async function loadBook(buf, name) {
    renderToken++;                      // kill in-flight renders
    setStatus('parsing…');
    busy('parsing EPUB');
    try {
      const r = await call('load', { epub: buf }, [buf], 120000);
      book = { title: r.title, spineCount: r.spineCount, hrefs: r.hrefs };
      populateSpines(r.hrefs);
      state = { spine: 0, page: 0, pages: 0, mode: state.mode };  // keep output mode
      els.openBtn.disabled = false;
      els.coverBtn.disabled = false;
      els.downloadBtn.disabled = exporting;
      els.exportStatus.textContent = '';
      invalidateWarm();           // warm bytes (if any) belong to a previous book
      await refresh(false);
      scheduleWarm(500);          // pre-convert the fresh book once idle
    } catch (e) {
      setStatus('load error: ' + e.message, true);
      idle();
    }
  }

  // ---- events ----
  els.file.addEventListener('change', () => {
    const f = els.file.files && els.file.files[0];
    if (!f) return;
    // reflect the chosen file on the picker label (native "no file chosen" text
    // belongs to the hidden input — the label is what the user sees)
    const lab = document.querySelector('label[for=epubFile]');
    if (lab) lab.textContent = f.name;
    f.arrayBuffer().then((buf) => loadBook(buf, f.name));
  });

  els.openBtn.addEventListener('click', () => els.file.click());
  els.coverBtn.addEventListener('click', () => loadCover());

  // ---- unified settings scheduler (rAF-paced hot path) ----
  // Every knob change funnels here. Sliders update their <output> live on
  // 'input' (per-frame, no engine work); the value commits on 'change'
  // (release / keyboard step). Engine work is coalesced to ONE visible-page
  // repaint per settle — the worker rebuilds the spine then renders the single
  // requested page, so the visible page is always what we re-render. When the
  // user STOPS changing (no knob activity), a background whole-book pre-
  // conversion (warm) runs in the worker so export is instant later.
  let warmKeyAtFire = null;      // outputKey the in-flight warm will produce
  let warmSpecKey = null;        // outputKey the warm bytes match (null = none)
  const fontFileFp = () => {
    const f = els.fontFile.files && els.fontFile.files[0];
    return f ? f.name + '|' + f.size + '|' + (f.lastModified || 0) : '';
  };
  const fontKnobSig = () => JSON.stringify({
    file: fontFileFp(), name: els.fontName.value.trim(),
    size: els.fontSize.value, weight: els.fontWeight.value,
    hangul: els.fontHangul.checked, intervals: els.fontIntervals.value.trim(),
    spacePx: els.fontSpacePx.value,
  });
  function outputKey() {
    const s = readSpec();
    s._fontSig = s.font === 'custom' ? fontKnobSig() : '';
    return JSON.stringify(s) + '|mode' + state.mode + '|xtcz' + (els.lz4Wrap.checked ? 1 : 0);
  }

  let lastRepaintKey = '';       // (reserved) outputKey of state on canvas
  let rafPending = false;
  let repaintRaf = 0;
  function requestRepaint(delayMs) {
    // coalesce rapid knob events: trailing delay, then ONE rAF → refresh
    clearTimeout(repaintTimer);
    repaintTimer = setTimeout(() => {
      if (rafPending) return;
      rafPending = true;
      cancelAnimationFrame(repaintRaf);
      repaintRaf = requestAnimationFrame(() => {
        rafPending = false;
        refresh(true);           // visible page only — engine rebuilds on change
      });
    }, delayMs == null ? 60 : delayMs);
  }

  // Bump this on ANY settings change: an in-flight background warm is then
  // stale (worker aborts between spines) and will be re-fired after settling.
  let warmVersion = 0;
  let warmSettleTimer = null;
  function scheduleWarm(delayMs) {
    if (!book || exporting) return;
    clearTimeout(warmSettleTimer);
    warmSettleTimer = setTimeout(fireWarm, delayMs == null ? 700 : delayMs);
  }
  function invalidateWarm() {
    warmSpecKey = null;
    warmVersion++;               // supersedes + cancels any in-flight warm
    // tell the WORKER too: an in-flight warm holds the engine; abort it at the
    // next spine yield so a font change doesn't queue behind a whole-book pass
    if (worker) {
      try { worker.postMessage({ id: 'warmCancel', cmd: 'warmCancel' }); } catch (_) {}
    }
  }
  async function fireWarm() {
    if (!book || exporting || !worker) return;
    const key = outputKey();
    if (warmSpecKey === key) return;      // already converted for this state
    if (warmKeyAtFire === key) return;    // a matching warm is already running
    warmKeyAtFire = key;
    const tok = warmVersion;
    try {
      const r = await call('warm', { mode: state.mode, xtcz: els.lz4Wrap.checked, token: tok }, null, 900000);
      if (tok !== warmVersion) return;    // settings changed mid-warm → stale
      if (r && r.warm === 'ready') {
        warmSpecKey = key;
        els.exportStatus.textContent = '✓ whole book pre-converted — export is instant';
      }
      // busy → a previous warm still finishing; it will supersede itself, so
      // just re-schedule once it has had time to stop
      else if (r && r.warm === 'busy') scheduleWarm(400);
    } catch (_) { /* engine busy/restarting — try again later */ scheduleWarm(1500); }
    finally { warmKeyAtFire = null; }
  }

  // ---- custom-font hot path ----
  // Any font knob / file change schedules an auto re-conversion (server-side,
  // memoized by full param set) once the user stops moving; the result is
  // pushed to the engine and the visible page re-renders. Stale conversions
  // are superseded; only the latest knob state wins.
  let fontPendingSig = null;     // knob sig the next conversion must produce
  function scheduleFontConvert() {
    fontPendingSig = fontKnobSig();
    clearTimeout(fontTimer);
    fontTimer = setTimeout(runFontConvert, 150);   // settle after last change
  }

  // ---- weight-ladder pre-bake ----
  // After a conversion lands, quietly pre-convert neighboring weights (server
  // memoizes by full param set) so the user's FIRST scrub of the weight slider
  // mostly hits the cache instead of paying ~1 s per never-seen weight. The
  // ladder only warms the server cache — results are discarded client-side.
  let ladderActive = false;
  function scheduleWeightLadder() {
    if (ladderActive || !book) return;
    ladderActive = true;
    const f = els.fontFile.files && els.fontFile.files[0];
    const run = async () => {
      try {
        if (!f || els.fontPreset.value !== 'custom') return;
        const fp = fontFp(f);
        if (!fontIdByFile[fp]) return;        // not registered yet → no ladder
        const cur = parseInt(els.fontWeight.value, 10) || 500;
        const steps = [];                       // ascending unique, cur excluded
        for (const d of [25, 50, 75, 100, 150, 200, 250, 300, 350, 400]) {
          for (const s of [cur - d, cur + d]) {
            if (s >= 100 && s <= 900 && s !== cur && !steps.includes(s)) steps.push(s);
          }
        }
        steps.sort((a, b) => Math.abs(a - cur) - Math.abs(b - cur));
        // serialize quietly, stop the moment a real conversion starts
        for (const w of steps.slice(0, 14)) {
          if (fontConverting || els.fontWeight.value !== String(cur)) return;
          try {
            const fd = new FormData();
            fd.append('fontId', fontIdByFile[fp]);
            fd.append('name', els.fontName.value.trim() || 'custom');
            fd.append('size', String(parseInt(els.fontSize.value, 10) || 14));
            fd.append('weight', String(w));
            fd.append('twoBit', '1');
            if (!els.fontHangul.checked) fd.append('noHangul', '1');
            const iv = els.fontIntervals.value.trim();
            if (iv) fd.append('extraIntervals', iv);
            const sp = parseInt(els.fontSpacePx.value, 10);
            if (!isNaN(sp) && sp >= 0) fd.append('spacePx', String(sp));
            await fetch('/api/convert-font', { method: 'POST', body: fd });
          } catch (_) { /* ladder is best-effort — drop it */ }
        }
      } finally {
        ladderActive = false;
      }
    };
    setTimeout(run, 250);
  }

  async function runFontConvert() {
    const f = els.fontFile.files && els.fontFile.files[0];
    if (!f || els.fontPreset.value !== 'custom') return;
    if (fontConverting) { scheduleFontConvert(); return; }  // join the tail
    const want = fontPendingSig || fontKnobSig();
    fontPendingSig = null;
    if (want === lastFontSig) return;           // already applied exactly this
    fontConverting = true;
    const tok = ++fontConvertToken;
    fontStatus('converting… ' + (f.name || ''), false);
    try {
      const meta = await convertFont(f);
      // superseded while converting → drop; a later run will use newer knobs
      if (tok !== fontConvertToken) return;
      // knobs moved again while converting → drop; scheduleFontConvert queued
      if (want !== fontKnobSig()) { scheduleFontConvert(); return; }
      customFontLoaded = true;
      lastFontSig = want;
      const wm = meta.weightMode === 'wght-instance' ? 'wght-instanced @' + meta.weight :
                  meta.weightMode === 'embolden' ? 'embolden +' + (meta.emboldenPx64 / 64).toFixed(2) + 'px' :
                  meta.weightMode === 'native' ? 'weight ≤ native (' + meta.weight + ' no-op)' : 'weight n/a';
      fontStatus('✓ ' + (meta.name || 'custom') + ' ' + meta.size + 'pt · w' + meta.weight +
                 ' [' + wm + '] · ' + meta.glyphs + ' glyphs — applying…', false);
      await applyCustomFont(meta);
      fontStatus('✓ ' + (meta.name || 'custom') + ' ' + meta.size + 'pt active (' +
                 meta.glyphs + ' glyphs, ' + Math.round(meta.bytes / 1024) + ' KB)', false);
      invalidateWarm();
      scheduleWarm(700);
      scheduleWeightLadder();   // warm neighbor weights for the first scrub
    } catch (e) {
      fontStatus('✗ ' + e.message, true);
    } finally {
      fontConverting = false;
    }
  }

  // ---- continuous paging across the WHOLE book ----
  // The device's XTC reader treats the file as one continuous page stream:
  // chapters are TOC markers, not paging barriers. prev/next cross spine
  // boundaries automatically. The page counter stays spine-local (matching the
  // device status bar's chapter-page display), but the user never has to touch
  // the chapter dropdown to read on.
  function goPage(delta) {
    if (!book) return;
    // from the cover, next goes into the book proper (spine 0, page 0)
    if (viewingCover) {
      if (delta > 0) { state.spine = 0; state.page = 0; refresh(true); }
      return;
    }
    let target = state.page + delta;
    let targetSpine = state.spine;
    if (target < 0) {
      if (targetSpine <= 0) return;               // already at book start
      targetSpine -= 1;
      target = 1 << 30;   // worker clamps over-range → that spine's last page
    } else if (target >= state.pages) {
      if (targetSpine >= book.spineCount - 1) return;  // at book end
      targetSpine += 1;
      target = 0;
    }
    state.spine = targetSpine;
    state.page = target;
    if (els.spineSel.value !== String(targetSpine)) {
      els.spineSel.value = String(targetSpine);   // keep the chapter picker honest
    }
    refresh(true);
  }
  els.prevBtn.addEventListener('click', () => goPage(-1));
  els.nextBtn.addEventListener('click', () => goPage(1));
  els.spineSel.addEventListener('change', () => {
    state.spine = parseInt(els.spineSel.value, 10);
    state.page = 0;
    refresh(false);
  });

  // ---- typography / page knobs: rAF-coalesced visible-page repaint ----
  const KNOB_IDS = [
    'lineCompression', 'paragraphAlignment', 'paragraphIndent',
    'extraParagraphSpacing', 'characterWrap', 'hyphenation',
    'embeddedStyle', 'textAa', 'imageRendering',
  ];
  KNOB_IDS.forEach((id) => els[id].addEventListener('change', () => {
    invalidateWarm();            // pagination changed → warm bytes stale
    requestRepaint(60);
    scheduleWarm(800);
  }));
  // live slider outputs on input (per-frame); repaint + warm on change
  const SLIDER_ROWS = [
    ['screenMargin', 'screenMarginOut', (v) => v],
    ['fontSize', 'fontSizeOut', (v) => v + ' pt'],
    ['fontWeight', 'fontWeightOut', (v) => v],
    ['fontSpacePx', 'fontSpacePxOut', (v) => v + ' px'],
  ];
  SLIDER_ROWS.forEach(([id, outId, fmt]) => {
    const el = els[id], out = document.getElementById(outId);
    el.addEventListener('input', () => { if (out) out.textContent = fmt(el.value); });
  });
  els.screenMargin.addEventListener('change', () => {
    invalidateWarm(); requestRepaint(60); scheduleWarm(800);
  });
  // font sliders commit → hot re-conversion of the custom face. React to
  // 'input' too (mid-drag pauses re-render at that weight) — the 150 ms
  // trailing debounce coalesces the burst and the convert token supersedes, so
  // only the value the drag pauses/releases on actually applies.
  ['fontSize', 'fontWeight', 'fontSpacePx'].forEach((id) => {
    const el = els[id];
    el.addEventListener('input', () => { invalidateWarm(); scheduleFontConvert(); });
    el.addEventListener('change', () => { invalidateWarm(); scheduleFontConvert(); });
  });

  // ---- font face + custom conversion ----
  // segmented face preset: click a segment → mirror into the hidden select →
  // re-render (preset faces) or hot-apply (custom, if a font is loaded)
  els.fontPreset.closest('.segRow').querySelector('.seg').addEventListener('click', (e) => {
    const seg = e.target.closest('.segBtn');
    if (!seg || seg.dataset.value === els.fontPreset.value) return;
    els.fontPreset.value = seg.dataset.value;
    syncFontSeg();
    toggleFontPanel();
    if (els.fontPreset.value !== 'custom') {
      // preset swap re-paginates through the spec path
      invalidateWarm(); requestRepaint(60); scheduleWarm(800);
    } else if (customFontLoaded) {
      // already have a runtime font → keep it active, hot-apply current knobs
      invalidateWarm(); requestRepaint(60); scheduleWarm(800);
      const f = els.fontFile.files && els.fontFile.files[0];
      if (f && lastFontSig !== fontKnobSig()) scheduleFontConvert();
    } else {
      // nothing uploaded yet → engine stays on default; hint the user
      fontStatus('choose an OTF/TTF above — it converts and applies automatically', false);
    }
  });
  els.fontFile.addEventListener('change', () => {
    const f = els.fontFile.files && els.fontFile.files[0];
    if (f) {
      const lab = document.querySelector('label[for=fontFile]');
      if (lab) lab.textContent = f.name;
      // auto-switch to the custom preset + hot-apply the new face
      if (els.fontPreset.value !== 'custom') {
        els.fontPreset.value = 'custom';
        syncFontSeg();
        toggleFontPanel();
      }
      invalidateWarm();
      scheduleFontConvert();
    }
  });
  syncFontSeg();   // reflect the hidden select's initial value on the buttons
  // extra-intervals textbox + hangul checkbox + name → hot re-conversion
  els.fontIntervals.addEventListener('change', () => { invalidateWarm(); scheduleFontConvert(); });
  els.fontHangul.addEventListener('change', () => { invalidateWarm(); scheduleFontConvert(); });
  els.fontName.addEventListener('change', () => { invalidateWarm(); scheduleFontConvert(); });
  els.zoom.addEventListener('input', () => {
    els.zoomOut.textContent = els.zoom.value + '%';
    updateZoomCss();
  });
  window.addEventListener('resize', () => updateZoomCss());
  els.resetBtn.addEventListener('click', () => {
    applyDefaults();
    invalidateWarm();
    refresh(true);
    scheduleWarm(800);
  });

  // ---- output mode: 1-bit XTC vs 2-bit XTCH (drives preview + export) ----
  // The engine always renders the full 3-plane content; the chosen mode decides
  // how it is quantized. The worker composes the preview through the same
  // quantization the encoder applies, so preview == file by construction.
  function syncExportSeg() {
    els.exportSeg.querySelectorAll('.segBtn').forEach((b) => {
      const on = b.dataset.value === String(state.mode);
      b.classList.toggle('active', on);
      b.setAttribute('aria-checked', on ? 'true' : 'false');
    });
  }
  els.exportSeg.addEventListener('click', (e) => {
    const seg = e.target.closest('.segBtn');
    if (!seg || Number(seg.dataset.value) === state.mode) return;
    state.mode = Number(seg.dataset.value);
    syncExportSeg();
    syncAaToMode();
    // re-render the current page so the preview shows the chosen mode
    if (book) { invalidateWarm(); refresh(true); scheduleWarm(700); }
  });

  // Text anti-aliasing only exists in the grey (lsb/msb) planes — a 1-bit XTC
  // file carries the BW plane only, so AA is a no-op there. Disable the knob
  // (and force it off for the render) when XTC 1-bit is selected.
  function syncAaToMode() {
    const disabled = state.mode === 0;
    els.textAa.disabled = disabled;
    // hint + dim the row so the disabled state reads as "n/a in 1-bit", not
    // as a broken control
    const hint = document.querySelector('.aaHint');
    if (hint) hint.textContent = disabled ? '(only in 2-bit)' : '';
    const lab = document.getElementById('textAaLab');
    if (lab) lab.classList.toggle('disabled', disabled);
    if (disabled && els.textAa.checked) {
      // keep the user's AA preference for when they switch back to 2-bit
      els.textAa.dataset.aaPref = '1';
      els.textAa.checked = false;
    } else if (!disabled && els.textAa.dataset.aaPref === '1') {
      els.textAa.checked = true;
      els.textAa.dataset.aaPref = '';
    }
  }

  // ---- whole-book export (1 book → 1 device file) ----
  function exportFilename(xtcz) {
    let n = (book ? book.title : 'book').replace(/[^\w\s가-힣\-\[\]]+/g, '').trim();
    if (!n) n = 'book';
    const pre = (els.exportName.value || '').trim();
    const ext = xtcz ? '.xtcz' : (state.mode === 0 ? '.xtc' : '.xtch');
    return (pre ? pre.replace(/[^\w\-\[\]]/g, '') + ' ' : '') + n + ext;
  }

  // Export the whole book at the current mode, then hand the browser the file.
  // Fast path: if the background warm already produced bytes for exactly this
  // output key, pull them from the worker (no re-render) and download.
  async function exportAndDownload() {
    if (!book || exporting) return;
    if (!(await call('ping', {}, [], 10000))) return;
    const xtcz = els.lz4Wrap.checked;
    const mode = state.mode;
    const key = outputKey();
    if (warmSpecKey === key) {
      // warm bytes match the current settings → instant, zero re-render
      exporting = true;
      els.downloadBtn.disabled = true;
      try {
        const res = await call('fetchWarm', {}, null, 60000);
        const u8 = new Uint8Array(res.file);
        const filename = exportFilename(res.xtcz !== undefined ? res.xtcz : xtcz);
        const ratio = res.xtcz && res.rawBytes ? ' (' + (100 * u8.byteLength / res.rawBytes).toFixed(0) + '% of raw)' : '';
        els.exportStatus.textContent = '✓ ' + filename + ' — ' + res.pages + ' pages, ' +
          (u8.byteLength / 1048576).toFixed(1) + ' MB' + ratio + ' (pre-converted)';
        saveBlob(u8, filename);
        warmSpecKey = null;   // bytes handed over; next warm refills
        refresh(true);        // engine was invalidated by the warm pass
      } catch (e) {
        els.exportStatus.textContent = '✗ warm fetch failed: ' + e.message;
      } finally {
        exporting = false;
        els.downloadBtn.disabled = !book;
      }
      return;
    }
    exporting = true;
    els.downloadBtn.disabled = true;
    busy('exporting (whole book)');
    const depthLabel = mode === 0 ? 'XTC 1-bit' : 'XTCH 2-bit';
    setStatus('exporting whole book as ' + depthLabel + (xtcz ? ' + LZ4 (.xtcz)' : '') +
              ' — re-renders every page with the current settings');
    els.exportStatus.textContent = 'exporting…';
    try {
      const res = await call('exportBook', { mode, xtcz }, null, 600000);
      const u8 = new Uint8Array(res.file);
      const filename = exportFilename(xtcz);
      const ratio = xtcz && res.rawBytes ? ' (' + (100 * u8.byteLength / res.rawBytes).toFixed(0) + '% of raw)' : '';
      els.exportStatus.textContent = '✓ ' + filename + ' — ' + res.pages + ' pages, ' +
        (u8.byteLength / 1048576).toFixed(1) + ' MB' + ratio;
      saveBlob(u8, filename);
      // the export ran every spine through the engine; the next preview render
      // rebuilds the current spine so the page stays in sync with the book
      refresh(true);
      invalidateWarm();
      scheduleWarm(700);
    } catch (e) {
      els.exportStatus.textContent = '✗ export failed: ' + e.message;
    } finally {
      exporting = false;
      els.downloadBtn.disabled = !book;
      idle();
    }
  }
  function saveBlob(u8, filename) {
    const blob = new Blob([u8], { type: 'application/octet-stream' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 4000);
  }
  els.downloadBtn.addEventListener('click', exportAndDownload);
  els.lz4Wrap.addEventListener('change', () => {
    // output container changes → warm bytes stale → pre-convert the new variant
    invalidateWarm();
    if (book) scheduleWarm(700);
  });
  syncExportSeg();   // reflect initial mode (XTCH 2-bit) on the toggle
  syncAaToMode();    // enable AA (2-bit default mode)
  window.__exportPeek = null;   // (removed with the file-view mode)

  // keyboard paging
  document.addEventListener('keydown', (e) => {
    if (e.target && (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT')) return;
    if (e.key === 'ArrowLeft') els.prevBtn.click();
    else if (e.key === 'ArrowRight') els.nextBtn.click();
  });

  // ---- boot ----
  hasFontBackend();   // one probe: is the server-side converter deployed here?
  worker = spawnWorker();
  bootEngine().catch((e) => {
    setStatus('engine failed to start: ' + e.message, true);
  }).then(() => {
    // convenience: /?epub=path triggers fetch+load (for local dev/test)
    const q = new URLSearchParams(location.search);
    const auto = q.get('epub');
    if (auto) {
      fetch(auto).then((r) => r.arrayBuffer()).then((buf) => loadBook(buf, auto.split('/').pop()));
    }
  });
})();
