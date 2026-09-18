// app.js — UI glue for the CrossPoint-KO EPUB preview. Talks to ko.worker.js.
(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);

  const els = {
    file: $('epubFile'), status: $('status'),
    bookTitle: $('bookTitle'), bookChange: $('bookChange'),
    pageStatus: $('pageStatus'), sidebar: $('sidebar'), drawerToggle: $('drawerToggle'),
    exportJump: $('exportJump'), exportSummary: $('exportSummary'),
    exportProgress: $('exportProgress'),
    page: $('page'), loading: $('loading'), pagerEl: $('pager'),
    prevBtn: $('prevBtn'), nextBtn: $('nextBtn'), pageInfo: $('pageInfo'),
    spineSel: $('spineSel'), coverBtn: $('coverBtn'),
    lineCompression: $('lineCompression'), paragraphAlignment: $('paragraphAlignment'),
    paragraphIndent: $('paragraphIndent'), extraParagraphSpacing: $('extraParagraphSpacing'),
    characterWrap: $('characterWrap'), hyphenation: $('hyphenation'),
    embeddedStyle: $('embeddedStyle'), textAa: $('textAa'), imageDither: $('imageDither'),
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
    const w = new Worker(base + 'ko.worker.js?v=44');
    w.onmessage = (ev) => {
      const m = ev.data;
      // worker progress reports carry no id — surface them live
      if (m && m.progress && els.exportStatus) {
        els.exportProgress.textContent = '생성 중 ' + m.spine + '/' + m.ofSpines + ' ' +
          '(' + m.pages + '쪽 완료)…';
        // set once: re-assigning the same string still mutates the live region
        if (els.exportStatus.textContent !== '기기 파일 생성 중…')
          els.exportStatus.textContent = '기기 파일 생성 중…';
        return;
      }
      const p = pending.get(m.id);
      if (!p) return;
      pending.delete(m.id);
      clearTimeout(p.timer);
      if (m.fatal) {
        // engine aborted (OOM etc.) — respawn, then reject this call
        setStatus('⚠ 엔진이 중단되어 자동으로 다시 시작합니다…', true);
        p.reject(new Error(m.error || '엔진 중단'));
        respawn();
        return;
      }
      m.ok ? p.resolve(m) : p.reject(new Error(m.error || 'worker error'));
    };
    w.onerror = (e) => {
      e.preventDefault();
      setStatus('⚠ 엔진 워커가 충돌해 자동으로 다시 시작합니다…', true);
      respawn();
    };
    w.onmessageerror = () => {
      setStatus('⚠ 엔진 워커 메시지 오류 — 자동 재시작…', true);
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
      // §8: prefer the remembered File — it survives a cancelled picker and is populated by
      // drag/drop, neither of which the input's FileList can do.
      const f = currentBookFile || (els.file.files && els.file.files[0]) || null;
      if (f) {
        f.arrayBuffer().then((buf) => { pendingBookFile = f; return loadBook(buf, f.name); });
      } else {
        const q = new URLSearchParams(location.search);
        const auto = q.get('epub');
        if (auto) fetch(auto).then((r) => r.arrayBuffer()).then((buf) => { pendingBookFile = null; return loadBook(buf, auto.split('/').pop()); });
      }
    }).catch(() => { respawning = false; });
  }

  function call(cmd, payload = {}, transfer, timeoutMs) {
    const id = nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        pending.delete(id);
        setStatus('⚠ 엔진 호출 "' + cmd + '" 시간 초과 ' +
                  Math.round((timeoutMs || CALL_TIMEOUT_MS) / 1000) + 's — 엔진 재시작…', true);
        respawn();
        reject(new Error('timeout: ' + cmd));
      }, timeoutMs || CALL_TIMEOUT_MS);
      pending.set(id, { resolve, reject, timer });
      worker.postMessage(Object.assign({ id, cmd }, payload), transfer || []);
    });
  }
  window.__call = call;   // debug hook (stats etc.)

  // ---- Korean, actionable error copy (item 8) ------------------------------
  // The raw WASM/JS message is never shown to the user; it goes to the console and onto the
  // element's title so a bug report can still quote it.
  function describeError(e, what) {
    const raw = String((e && e.message) || e || '');
    const m = raw.toLowerCase();
    if (/encrypt|drm|rights|obfuscat|password/.test(m))
      return 'DRM이 걸린 EPUB으로 보입니다. DRM이 없는 파일로 다시 시도해 주세요.';
    // 'Epub::load failed' is the engine's catch-all for a container it cannot parse: in
    // practice that is a corrupt download or a DRM-wrapped file, so say both.
    if (/zip|central directory|end of central|not a zip|invalid epub|corrupt|truncat|epub::load|load failed|failed to open/.test(m))
      return 'EPUB 파일을 열 수 없습니다. 파일이 손상되었거나 DRM이 적용되어 있는지 확인해 주세요.';
    if (/spine|no content|empty book|no pages/.test(m))
      return '이 EPUB에서 본문을 찾을 수 없습니다. 다른 파일로 시도해 주세요.';
    if (/font|freetype|glyph|epdfont|face/.test(m))
      return '글꼴 변환에 실패했습니다. 다른 TTF/OTF 파일로 시도해 주세요.';
    if (/memory|oom|allocation|out of bounds|grow/.test(m))
      return '메모리가 부족합니다. 더 작은 EPUB으로 시도하거나 브라우저 탭을 정리해 주세요.';
    if (/image|jpeg|png|decode|bitmap/.test(m))
      return '이미지를 처리할 수 없습니다. 지원하지 않는 이미지 형식일 수 있습니다.';
    if (/css|html|xml|parse|xhtml/.test(m))
      return 'EPUB 내부의 HTML/CSS를 해석할 수 없습니다. 파일이 손상되었을 수 있습니다.';
    if (/export|writer|plane|container/.test(m))
      return '기기 파일을 만드는 중 문제가 발생했습니다. 설정을 기본값으로 되돌린 뒤 다시 시도해 주세요.';
    return (what ? what + ' 중 문제가 발생했습니다.' : '변환 중 문제가 발생했습니다.') +
           ' 잠시 후 다시 시도해 주세요.';
  }

  function reportError(e, what) {
    console.error(what || 'xtcko', e);
    const msg = describeError(e, what);
    setStatus(msg, true);
    if (els.status) els.status.title = String((e && e.message) || e || '');
  }

  // §4: EPUB metadata and filenames can arrive decomposed (NFD Hangul), which renders as
  // broken-looking jamo. Display normalization only — byte data is never touched.
  function normalizeDisplayText(value) {
    try { return String(value == null ? '' : value).normalize('NFC'); }
    catch (e) { return String(value == null ? '' : value); }
  }

  // §1: two explicit UI states drive everything (CSS keys off the attribute)
  function setAppState(next) {
    document.body.dataset.appState = next;
    if (next === 'loaded') {
      updateZoomCss();          // the preview box only has its real size once visible
    }
  }

  // §9: below 760 px #status is a toast, so a success message should not sit there forever and
  // eat the bottom of a phone screen. On desktop the class toggles nothing and the line persists.
  var statusTimer = 0;
  function setStatus(msg, isErr) {
    // A polite region for progress, assertive for failures (item 4). Toggling aria-live is the
    // supported way to raise severity on one region without double-announcing the message.
    if (els.status) els.status.setAttribute('aria-live', isErr ? 'assertive' : 'polite');
    els.status.textContent = msg;
    // long titles truncate in the header — hover reveals the full message
    els.status.title = (msg || '').replace(/\s+/g, ' ').trim();
    els.status.className = 'status' + (isErr ? ' err' : ' ok');
    // §7: while the modal drawer is open a toast would sit on top of it (60 over 40).
    if (!document.body.classList.contains('drawer-open')) {
      document.body.classList.add('status-active');
    }
    clearTimeout(statusTimer);
    if (!isErr) {
      statusTimer = setTimeout(() => document.body.classList.remove('status-active'), 4000);
    }
  }

  async function bootEngine() {
    const r = await call('ping', {}, [], 15000);
    setStatus('엔진 ' + r.version + ' 준비됨 — EPUB을 불러오세요');
    // Cold-boot accounting, on the page's own timeline. `navToEngineReady` is the number that
    // matters for "is this fast enough to start": everything else here exists to say WHERE the
    // time went (network vs compile) so an optimisation can be aimed instead of guessed.
    try {
      const b = await call('bootstats', {}, [], 5000);
      const navStartWall = Date.now() - performance.now();
      const stats = {
        ...b,
        navigationStartWall: navStartWall,
        navToEngineReady: b.engineReadyWall ? b.engineReadyWall - navStartWall : null,
        domContentLoaded: performance.getEntriesByType('navigation')[0]
          ? performance.getEntriesByType('navigation')[0].domContentLoadedEventEnd : null,
      };
      window.__koBootStats = stats;
      if (window.console && console.table && stats.phases) {
        console.table({ ...stats.phases, navToEngineReady: stats.navToEngineReady });
        if (stats.resource) console.table(stats.resource);
      }
    } catch (e) { /* diagnostics must never break boot */ }
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
      // Image dither model + the tone depth this export needs (4 tones for a 2-bit
      // page, 2 for a 1-bit one). The depth is part of the spec on purpose: changing
      // the output mode must re-render, because a 1-bit page is dithered straight to
      // two tones from the source instead of halftoning a 4-level intermediate.
      imageDither: parseInt(els.imageDither.value, 10),
      imageToneDepth: state.mode === 0 ? 2 : 4,
      textAa: els.textAa.checked ? 1 : 0,
      screenMargin: parseInt(els.screenMargin.value, 10),
      imageRendering: parseInt(els.imageRendering.value, 10),
      // 'custom' only when a runtime font is actually loaded; otherwise the
      // worker would silently keep the default while the UI claims custom.
      font: els.fontPreset.value === 'custom'
        ? (customFontLoaded ? 'custom' : 'kopub')
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
    els.screenMarginOut.textContent = '5 px';
    // Reference geometry. The reader reserves a 19 px status-bar lane
    // (UITheme::getStatusBarHeight() on shipped defaults) and screenMargin is
    // added to all four sides, bottom being max(screenMargin, 19) — see
    // marginsFor() in ko.worker.js. The lane is a reservation only: no
    // status-bar pixels are ever written into an exported page, the device
    // composites its own chrome at read time. It is here so text lands on the
    // same y the reference reader would use.
    els.imageRendering.value = '0';
    els.imageDither.value = '2';              // blue noise (the firmware's model)
    els.fontPreset.value = 'kopub';           // CrossPointSettings::getReaderFontId() default
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
    // These assignments do not fire change events, so the dependent-visibility rule has
    // to be re-applied by hand: resetting to the defaults puts character wrap back ON,
    // which means the hyphenation row must disappear again.
    syncDependentControls();
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
    busy('표지 생성 중');
    try {
      const r = await call('cover', { kind: 0 });
      const img = bmpToImageData(r.cover);
      drawCoverFitted(img);          // letterbox into the fixed 480×800 screen
      els.pageStatus.textContent = '표지';
      els.page.setAttribute('aria-label', '도서 표지 미리보기');
    } catch (e) {
      reportError(e, '표지 렌더링');
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

  // Surface the derived text viewport. It is the firmware's own arithmetic (viewable margins +
  // screenMargin + status-bar lane), so showing it makes pagination behaviour auditable instead of
  // mysterious. The worker returns it on the render reply now that the spec travels with the render.
  function showViewport(r) {
    if (!els.viewportOut || !r || !r.viewport || !r.margins) return;
    const m = r.margins;
    els.viewportOut.textContent =
      '본문 영역: ' + r.viewport.width + '×' + r.viewport.height +
      ' px · 여백 T/R/B/L ' + m.top + '/' + m.right + '/' + m.bottom + '/' + m.left +
      ' · 파일에 UI 없음';
  }

  // §2: the spec only has to be pushed when it actually changed. Every spec push used to run
  // applyFont() in the worker, which for a custom font re-malloc'ed and re-parsed the whole
  // .epdfont — on every single page turn. Ordinary navigation is now one worker request.
  let lastPushedSpecKey = null;
  let warmSkippedKey = null;      // outputKey of a book we decided is too large to warm (§3)
  function currentSpecKey() {
    try { return JSON.stringify(readSpec()); } catch (_) { return null; }
  }

  async function refresh(keepPage = true, quiet = false) {
    if (!book) return;
    const tok = ++renderToken;
    viewingCover = false;
    if (!quiet) busy('미리보기 생성 중');   // scrub-path re-renders stay silent
    try {
      // §8 of the 1.1 audit: the spec rides on the render request when it changed, so a settings
      // change is ONE worker round trip instead of two (the reply carries the fresh viewport and
      // margins for the readout). The chosen export mode travels with the render too, so the preview
      // is quantized exactly like the file the mode produces.
      const key = currentSpecKey();
      const spec = key !== lastPushedSpecKey ? readSpec() : null;
      const spine = Math.min(state.spine, book.spineCount - 1);
      const page = keepPage ? state.page : 0;
      const payload = { spine, page, mode: state.mode };
      if (spec) payload.spec = spec;
      const r = await call('render', payload);
      if (spec) { lastPushedSpecKey = key; showViewport(r); }
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
      // Routine navigation must not flood a live region: the page counter is plain text
      // (readable on demand), while #status keeps only meaningful events.
      const chapter = els.spineSel.value ? (Number(els.spineSel.value) + 1) + '장 ' : '';
      els.pageStatus.textContent = chapter + (r.page + 1) + '/' + r.pages +
                                   (r.mono ? ' · 1-bit' : '');
      // §15: a screen reader cannot read rasterized text, but it can say what this object is
      els.page.setAttribute('aria-label', '도서 미리보기, ' + chapter + (r.page + 1) + '쪽');
    } catch (e) {
      if (tok === renderToken) reportError(e, '페이지 렌더링');
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
    els.pageInfo.textContent = viewingCover ? '표지' : (state.pages > 0
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
    // native radios: checked state only — no aria-checked, no click plumbing
    seg.querySelectorAll('input[name=fontPresetRadio]').forEach((r) => { r.checked = r.value === v; });
  }

  function toggleFontPanel() {
    els.fontUpload.classList.toggle('hidden', els.fontPreset.value !== 'custom');
  }

  function fontStatus(msg, isErr) {
    els.fontConvStatus.textContent = msg;
    els.fontConvStatus.className = 'fontStatus' + (isErr ? ' err' : ' ok');
  }

  // ---- backend capability ------------------------------------------------
  // Font conversion runs in the browser now (worker: FreeType wasm + web/epdfont.js), so
  // nothing here is "missing" on a static deploy any more. This probe only decides whether
  // the legacy PYTHON endpoint is available to fall back to (local development).
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
        return fontBackend;
      })();
    }
    return fontBackendProbe;
  }

  // Convert the chosen font + tuning knobs to .epdfont.
  //
  // The conversion now runs IN THE BROWSER (worker: FreeType wasm + web/epdfont.js), so the
  // hosted build needs nothing installed on the user's machine and the font bytes never leave
  // the tab. server.py is kept only as a fallback for development: if the in-browser
  // converter fails to load, the old endpoint is tried when it exists.
  let fontIdByFile = {};    // file fingerprint → registered fontId (server fallback only)
  let fontConvIsLocal = true;  // false only if the in-browser converter failed and the
                               // Python endpoint took over (see the ladder guard above)
  const fontFp = (f) => (f ? f.name + '|' + f.size + '|' + (f.lastModified || 0) : '');

  async function convertFontLocal(fontFile) {
    const raw = await fontFile.arrayBuffer();
    const iv = els.fontIntervals.value.trim();
    const sp = parseInt(els.fontSpacePx.value, 10);
    const name = els.fontName.value.trim() || 'custom';
    const size = parseInt(els.fontSize.value, 10) || 14;
    const weight = parseInt(els.fontWeight.value, 10) || 500;
    const t0 = performance.now();
    const r = await call('convertFont', {
      font: raw,
      name: name,
      size: size,
      weight: weight,
      twoBit: true,             // reader fonts are always 2-bit; the XTC/XTCH output depth
                                // is the export mode, applied at pack time
      noHangul: !els.fontHangul.checked,
      extraIntervals: iv ? iv.split(/[\s,]+/).filter(Boolean) : [],
      spacePx: isNaN(sp) ? undefined : sp,
    }, [raw], 120000);
    r._bytes = r.epdfont;
    r.bytes = r.epdfont.byteLength;
    r.ms = Math.round(performance.now() - t0);
    // flatten the worker's meta so the callers' status text reads the same as it did for
    // the server response (name/size/weight/glyphs/weightMode/bytes)
    Object.assign(r, r.meta || {});
    r.name = name;
    r.size = size;
    r.weight = r.effectiveWeight || weight;
    r.inBrowser = true;
    return r;
  }

  async function convertFontServer(fontFile) {
    fontConvIsLocal = false;   // the ladder may warm the server cache again
    const fp = fontFp(fontFile);
    const fd = new FormData();
    const known = fontIdByFile[fp];
    if (known) fd.append('fontId', known);
    else fd.append('font', fontFile);
    fd.append('name', els.fontName.value.trim() || 'custom');
    fd.append('size', String(parseInt(els.fontSize.value, 10) || 14));
    fd.append('weight', String(parseInt(els.fontWeight.value, 10) || 500));
    fd.append('twoBit', '1');
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
        return convertFontServer(fontFile);
      }
      const why = j.error || j.stderr || ('HTTP ' + resp.status);
      throw new Error('conversion failed: ' + String(why).slice(0, 400));
    }
    if (j.fontId) fontIdByFile[fp] = j.fontId;
    const bin = atob(j.epdfont);
    const buf = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
    j._bytes = buf.buffer;
    return j;
  }

  async function convertFont(fontFile) {
    try {
      return await convertFontLocal(fontFile);
    } catch (e) {
      // Fall back to the local dev server only if this deploy actually has one; a static
      // host 404s the probe. Either way the error that reaches the user is the in-browser
      // one, which is the path that is supposed to work everywhere.
      if (await hasFontBackend().catch(() => false)) return convertFontServer(fontFile);
      throw e;
    }
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
      const label = normalizeDisplayText(h.replace(/\.(xhtml|html|htm)$/i, '')
                        .replace(/[_]+/g, ' ')) || ('spine ' + i);
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
    // §2 of the 1.2 audit: cancel the warm BEFORE asking the engine to load. Cancelling afterwards
    // left the warm free to resume between spines against a book that had already been replaced (the
    // worker also waits for it to stop now, but the request must not be sent first either).
    invalidateWarm();
    setStatus('EPUB 분석 중…');
    busy('EPUB 분석 중');
    try {
      const r = await call('load', { epub: buf }, [buf], 120000);
      // §5: canonicalize ONCE, here. The header, the export filename and every later comparison
      // then use one composed string. An NFD metadata title used to reach the filename sanitizer
      // raw, where only composed syllables ([가-힣]) are allowed, and a book with no metadata title
      // exported as "book" even though the header showed the filename.
      // The engine falls back to the FILENAME when the EPUB has no <dc:title>, so strip a trailing
      // .epub from whichever source won — otherwise "untitled.epub" lands in the export filename.
      const stripExt = (v) => String(v || '').replace(/\.epub$/i, '').trim();
      const canonicalTitle = normalizeDisplayText(
        stripExt(r.title) || stripExt(name) || '제목 없음'
      );
      bookEpoch++;                 // §3: a new book invalidates any remembered warm key/skip
      book = { title: canonicalTitle, spineCount: r.spineCount, hrefs: r.hrefs };
      currentBookFile = pendingBookFile;   // §8: only a successful load promotes the File
      pendingBookFile = null;
      populateSpines(r.hrefs);
      state = { spine: 0, page: 0, pages: 0, mode: state.mode };  // keep output mode
      els.coverBtn.disabled = false;
      setAppState('loaded');
      const shownTitle = canonicalTitle;
      els.bookTitle.textContent = shownTitle;
      els.bookTitle.hidden = false;
      els.bookTitle.title = shownTitle;
      setStatus('✓ EPUB 열기 완료');
      els.downloadBtn.disabled = exporting;
      els.exportStatus.textContent = '';
      invalidateWarm();           // warm bytes (if any) belong to a previous book
      lastPushedSpecKey = null;   // §2: a fresh engine needs the spec once
      await refresh(false);
      scheduleWarm(3000);          // pre-convert the fresh book once idle
    } catch (e) {
      reportError(e, 'EPUB 열기');
      idle();
    }
  }

  // ---- events ----
  // §8: remember the loaded File here instead of reading it back off <input>. Clearing the input as
  // the dialog opens (so re-picking the same path fires change) also wipes its FileList when the
  // user then cancels, and drag/dropped files never populate it at all — both used to break the
  // worker-respawn recovery, which reloaded from els.file.
  let pendingBookFile = null;    // the File behind the in-flight load
  let currentBookFile = null;    // the File behind the book that is actually loaded

  function startLoad(f) {
    if (!f) return;
    if (!/\.epub$/i.test(f.name || '') && f.type !== 'application/epub+zip') {
      setStatus('EPUB 파일만 열 수 있습니다.', true);
      return;
    }
    // §4: the button label stays "EPUB 파일 선택". It used to be replaced by the filename, which
    // turned the primary action into a multi-line block for a long Korean name — and left it that
    // way permanently if parsing then failed. The name goes in its own one-line ellipsized field.
    pendingBookFile = f;
    const picked = document.getElementById('pickedFile');
    if (picked) {
      picked.textContent = '선택됨: ' + normalizeDisplayText(f.name);
      picked.hidden = false;
      picked.title = normalizeDisplayText(f.name);
    }
    f.arrayBuffer().then((buf) => loadBook(buf, f.name));
  }

  // §3: the picker does not fire change when the same path is chosen again (e.g. the user
  // replaced the EPUB on disk and reopened it), so clear the input as the dialog opens. Clearing
  // the value does not touch the currently loaded book, and cancelling still changes nothing.
  els.file.addEventListener('click', () => { els.file.value = ''; });

  els.file.addEventListener('change', () => {
    startLoad(els.file.files && els.file.files[0]);
  });

  // Drag & drop is a first-class desktop path: drop anywhere on the page.
  const dropZone = document.getElementById('dropZone');
  const stop = (e) => { e.preventDefault(); e.stopPropagation(); };
  ['dragenter', 'dragover'].forEach((ev) => document.addEventListener(ev, (e) => {
    stop(e);
    if (dropZone) dropZone.classList.add('drag');
  }));
  ['dragleave', 'dragend'].forEach((ev) => document.addEventListener(ev, (e) => {
    stop(e);
    if (e.relatedTarget === null && dropZone) dropZone.classList.remove('drag');
  }));
  document.addEventListener('drop', (e) => {
    stop(e);
    if (dropZone) dropZone.classList.remove('drag');
    const f = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
    startLoad(f);
  });

  els.coverBtn.addEventListener('click', () => loadCover());
  els.bookChange.addEventListener('click', () => els.file.click());

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
  // §3 of the 1.5 audit: the key must be self-contained. warmSpecKey is cleared on load, but a
  // remembered SKIP has to survive book switches, so the book epoch is part of the key itself —
  // otherwise a book A skip could suppress a legitimate warm for book B.
  let bookEpoch = 0;
  function outputKey() {
    const s = readSpec();
    s._fontSig = s.font === 'custom' ? fontKnobSig() : '';
    return JSON.stringify({ book: bookEpoch, spec: s, mode: state.mode,
                            xtcz: els.lz4Wrap.checked ? 1 : 0 });
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
  // §1 of the 1.2 audit: 'exporting' only guarded a few knob handlers in JS and disabled nothing, so
  // the sidebar stayed fully operable during a foreground export. Disable the mutating controls for
  // real. The worker enforces the same invariant independently — this is a courtesy, not the guarantee.
  function setExportLocked(locked) {
    exporting = locked;
    document.querySelectorAll('#sidebar input, #sidebar select, #sidebar button, #bookChange')
      .forEach((el) => { if (el !== els.downloadBtn) el.disabled = locked; });
    els.downloadBtn.disabled = locked || !book;
  }

  function scheduleWarm(delayMs) {
    if (!book || exporting) return;
    // §3: a book we already decided is too large must not be probed again on every settings change
    const key = outputKey();
    if (warmSpecKey === key || warmSkippedKey === key) return;
    clearTimeout(warmSettleTimer);
    warmSettleTimer = setTimeout(fireWarm, delayMs == null ? 2000 : delayMs);
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
      // §3 of the 1.3 audit: the warm carries the spec it is producing. Otherwise it silently
      // depends on a preview render having already applied the same settings to the engine.
      const warmSpec = readSpec();
      const r = await call('warm', { spec: warmSpec, mode: state.mode, xtcz: els.lz4Wrap.checked, token: tok },
                            null, 900000);
      if (tok !== warmVersion) return;    // settings changed mid-warm → stale
      if (r && r.warm === 'ready') {
        warmSpecKey = key;
        warmSkippedKey = null;
        els.exportStatus.textContent = '✓ 미리 변환 완료 — 내보내면 바로 저장됩니다';
        // Surface the conversion's own timings: the warm is the common path in the app, so a report
        // that only rides the explicit-export response is invisible exactly when someone is looking at
        // the conversion they just waited for.
        if (r && r.spineTimes && r.spineTimes.length) {
          const ms = r.spineTimes.map((x) => x.ms).slice().sort((a, b) => a - b);
          const sum = ms.reduce((a, b) => a + b, 0);
          const heaviest = ms[ms.length - 1];
          const speedup = (n) => sum / Math.max(sum / n, heaviest);
          window.__koExportStats = {
            source: 'warm', spines: r.spineTimes.length, pages: r.pages,
            spineMs: { min: ms[0], p50: ms[Math.floor(ms.length * 0.5)],
                       p90: ms[Math.floor(ms.length * 0.9)], max: heaviest, sum: +sum.toFixed(1) },
            ceiling: { w4: +speedup(4).toFixed(2), w8: +speedup(8).toFixed(2) },
            preflight: r.preflight || null,
            spineTimes: r.spineTimes,
          };
        }
      }
      // busy → a previous warm still finishing; it will supersede itself, so
      // just re-schedule once it has had time to stop
      else if (r && r.warm === 'skipped-large') {
        // §3: remember the decision for this exact output state, or the next settings change (and the
        // post-export reschedule) starts probing the oversized warm all over again
        warmSkippedKey = key;
        els.exportStatus.textContent = '대용량 도서는 내보낼 때 변환됩니다';
      }
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
    // Nothing to warm when conversion is local: the ladder exists to pre-fill the PYTHON
    // server's memo cache so a weight scrub does not pay a round trip per step. In-browser
    // conversion takes ~150-400 ms for a full Hangul font, straight from the picked file,
    // so fourteen extra conversions would only burn worker time for no benefit — the first
    // scrub converts on demand instead.
    if (fontConvIsLocal) return;
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
    fontStatus('글꼴 변환 중… ' + (f.name || ''), false);
    try {
      const meta = await convertFont(f);
      // superseded while converting → drop; a later run will use newer knobs
      if (tok !== fontConvertToken) return;
      // knobs moved again while converting → drop; scheduleFontConvert queued
      if (want !== fontKnobSig()) { scheduleFontConvert(); return; }
      customFontLoaded = true;
      lastFontSig = want;
      const wm = meta.weightMode === 'wght-instance' ? 'wght 인스턴스 @' + meta.weight :
                  meta.weightMode === 'embolden' ? '합성 볼드 +' + (meta.emboldenPx64 / 64).toFixed(2) + 'px' :
                  meta.weightMode === 'native' ? '원본 굵기 이하 (' + meta.weight + ' 무시)' : '굵기 해당 없음';
      fontStatus('✓ ' + (meta.name || 'custom') + ' ' + meta.size + 'pt · w' + meta.weight +
                 ' [' + wm + '] · ' + meta.glyphs + '글리프 — 적용 중…', false);
      await applyCustomFont(meta);
      fontStatus('✓ ' + (meta.name || 'custom') + ' ' + meta.size + 'pt 활성 (' +
                 meta.glyphs + '글리프, ' + Math.round(meta.bytes / 1024) + ' KB' +
                 (meta.inBrowser ? ', 브라우저에서 ' + meta.ms + ' ms 만에 변환 — ' +
                   (meta.ftVersion || 'FreeType') : '') + ')', false);
      invalidateWarm();
      scheduleWarm(2000);
      scheduleWeightLadder();   // warm neighbor weights for the first scrub
    } catch (e) {
      fontStatus('✗ ' + describeError(e, '글꼴 변환'), true);
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

  // ---- remembered preferences (item 22) ------------------------------------
  // Settings, never the book. A chosen custom font cannot be restored (the file is not kept),
  // so that one preset falls back to the built-in face on load.
  const PREFS_KEY = 'xtcko.prefs.v1';
  const PREFS_IDS = ['lineCompression', 'paragraphAlignment', 'paragraphIndent',
                     'extraParagraphSpacing', 'characterWrap', 'hyphenation', 'embeddedStyle',
                     'textAa', 'imageRendering', 'imageDither', 'screenMargin', 'fontPreset',
                     'fontSize', 'fontWeight', 'fontSpacePx', 'fontHangul', 'fontIntervals',
                     'lz4Wrap', 'exportName'];

  function savePrefs() {
    try {
      const o = {};
      for (const id of PREFS_IDS) {
        const el = els[id] || document.getElementById(id);
        if (!el) continue;
        o[id] = el.type === 'checkbox' ? el.checked : el.value;
      }
      o.zoom = els.zoom ? els.zoom.value : '100';
      o.mode = state && state.mode === 0 ? 0 : 1;
      localStorage.setItem(PREFS_KEY, JSON.stringify(o));
    } catch (e) { /* private mode / quota: preferences are a convenience */ }
  }

  function loadPrefs() {
    let o = null;
    try { o = JSON.parse(localStorage.getItem(PREFS_KEY) || 'null'); } catch (e) { o = null; }
    if (!o || typeof o !== 'object') return false;
    for (const id of PREFS_IDS) {
      const el = els[id] || document.getElementById(id);
      if (!el || !(id in o)) continue;
      if (el.type === 'checkbox') el.checked = !!o[id]; else el.value = o[id];
    }
    if (o.fontPreset === 'custom') {          // the .epdfont itself is not remembered
      const p = els.fontPreset; if (p) p.value = 'kopub';
    }
    syncFontSeg();                            // the radios mirror the (restored) select
    toggleFontPanel();
    if (els.zoom && o.zoom) els.zoom.value = o.zoom;
    if (state) {                       // the seg buttons are driven by state.mode
      state.mode = o.mode === 0 ? 0 : 1;
      syncExportSeg();
      syncAaToMode();
    }
    // §6: refresh EVERY slider's <output> with the same formatters the sliders use, so the label
    // and the value cannot drift. Restoring only the margin meant a saved 18 pt / 600 / 12 px
    // profile reopened with those values applied while the labels still read 14 pt / 500 / 9 px —
    // and the margin lost its unit ('5' instead of '5 px').
    refreshSliderOutputs();
    if (els.zoomOut) els.zoomOut.textContent = els.zoom.value + '%';
    return true;
  }

  function clearPrefs() {
    try { localStorage.removeItem(PREFS_KEY); } catch (e) { /* ignore */ }
  }

  // ---- typography / page knobs: rAF-coalesced visible-page repaint ----
  const KNOB_IDS = [
    'lineCompression', 'paragraphAlignment', 'paragraphIndent',
    'extraParagraphSpacing', 'characterWrap', 'hyphenation',
    'embeddedStyle', 'textAa', 'imageRendering', 'imageDither',
  ];
  KNOB_IDS.forEach((id) => els[id].addEventListener('change', () => {
    // imageDither does not move a single page break (image size is fixed), but the
    // EXPORTED PIXELS change, so the warmed file is stale either way.
    invalidateWarm();            // pagination or pixels changed → warm bytes stale
    requestRepaint(60);
    scheduleWarm(2000);
    savePrefs();
  }));

  // ---- controls that are inert in the current state are not shown -------------
  // Two cases, both measured rather than assumed (scripts/verify/settings_effect.js
  // exports the whole book once per variant and diffs the page payloads):
  //
  //  * Hyphenation is masked by the engine while character wrap is on:
  //    toReaderSpec() sets `hyphenationEnabled && characterWrap == 0`, so with wrap on
  //    flipping it changes 0 of 2034 page payloads (and 0 of 14 on the image book).
  //    Upstream states the same rule: "Hyphenation only applies in word-wrap mode:
  //    character wrap can break anywhere already." So the row is hidden while wrap is
  //    on, and appears the moment wrap is switched off - when it does something
  //    (1125/2084 pages on the Korean book, 5/14 on the English one).
  //  * The custom-font tuning knobs all funnel through runFontConvert(), which returns
  //    early unless a font FILE is loaded, so they are inert (and now hidden) until one
  //    is. See #fontTuning in index.html.
  function syncDependentControls() {
    const hLab = document.getElementById('hyphenationLab');
    if (hLab) hLab.classList.toggle('hidden', els.characterWrap.checked);
    // ...and say so: a setting that silently has no effect looks broken.
    const note = document.getElementById('wrapNote');
    if (note) note.classList.toggle('hidden', !els.characterWrap.checked);
    const tuning = document.getElementById('fontTuning');
    if (tuning) {
      const hasFile = !!(els.fontFile.files && els.fontFile.files[0]);
      tuning.classList.toggle('hidden', !hasFile);
    }
  }
  els.characterWrap.addEventListener('change', syncDependentControls);
  els.fontFile.addEventListener('change', syncDependentControls);
  syncDependentControls();
  // live slider outputs on input (per-frame); repaint + warm on change
  function refreshSliderOutputs() {
    SLIDER_ROWS.forEach(([id, outId, fmt]) => {
      const el = els[id] || document.getElementById(id);
      const out = document.getElementById(outId);
      if (el && out) out.textContent = fmt(el.value);
    });
  }

  const SLIDER_ROWS = [
    ['screenMargin', 'screenMarginOut', (v) => v + ' px'],
    ['fontSize', 'fontSizeOut', (v) => v + ' pt'],
    ['fontWeight', 'fontWeightOut', (v) => v],
    ['fontSpacePx', 'fontSpacePxOut', (v) => v + ' px'],
  ];
  SLIDER_ROWS.forEach(([id, outId, fmt]) => {
    const el = els[id], out = document.getElementById(outId);
    el.addEventListener('input', () => { if (out) out.textContent = fmt(el.value); });
  });
  els.screenMargin.addEventListener('change', () => {
    invalidateWarm(); requestRepaint(60); scheduleWarm(2000);
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
  els.fontPreset.closest('.segRow').querySelector('.seg').addEventListener('change', (e) => {
    const seg = e.target.closest('input[name=fontPresetRadio]');
    if (!seg || seg.value === els.fontPreset.value) return;
    els.fontPreset.value = seg.value;
    syncFontSeg();
    toggleFontPanel();
    if (els.fontPreset.value !== 'custom') {
      // preset swap re-paginates through the spec path
      invalidateWarm(); requestRepaint(60); scheduleWarm(2000);
    } else if (customFontLoaded) {
      // already have a runtime font → keep it active, hot-apply current knobs
      invalidateWarm(); requestRepaint(60); scheduleWarm(2000);
      const f = els.fontFile.files && els.fontFile.files[0];
      if (f && lastFontSig !== fontKnobSig()) scheduleFontConvert();
    } else {
      // nothing uploaded yet → engine stays on default; hint the user
      fontStatus('위에서 OTF/TTF를 선택하세요 — 자동으로 변환해 적용합니다', false);
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
  els.zoom.addEventListener('change', savePrefs);
  window.addEventListener('resize', () => updateZoomCss());
  els.resetBtn.addEventListener('click', () => {
    clearPrefs();                // 되돌리기 means forget the remembered preferences too
    applyDefaults();
    invalidateWarm();
    refresh(true);
    scheduleWarm(2000);
  });

  // ---- output mode: 1-bit XTC vs 2-bit XTCH (drives preview + export) ----
  // The engine always renders the full 3-plane content; the chosen mode decides
  // how it is quantized. The worker composes the preview through the same
  // quantization the encoder applies, so preview == file by construction.
  function updateExportSummary() {
    if (!els.exportSummary) return;
    const mode = state && state.mode === 0 ? 'XTC · 1비트 · 작은 파일' : 'XTCH · 2비트 · 이미지 품질 우선';
    els.exportSummary.textContent = mode + (els.lz4Wrap && els.lz4Wrap.checked ? ' + LZ4 (.xtcz)' : '');
  }

  function syncExportSeg() {
    els.exportSeg.querySelectorAll('input[name=exportModeRadio]').forEach((r) => {
      r.checked = Number(r.value) === state.mode;
    });
  }
  els.exportSeg.addEventListener('change', (e) => {
    const seg = e.target.closest('input[name=exportModeRadio]');
    if (!seg || Number(seg.value) === state.mode) return;
    state.mode = Number(seg.value);
    syncExportSeg();
    syncAaToMode();
    updateExportSummary();
    savePrefs();
    // re-render the current page so the preview shows the chosen mode
    if (book) { invalidateWarm(); refresh(true); scheduleWarm(2000); }
  });

  // The AA switch, live in both modes:
  //   2-bit XTCH — text anti-aliasing, exactly as the firmware does it: the grey
  //                (lsb/msb) planes carry 2-bit glyph coverage; off renders text
  //                1-bit (the engine skips the text grey passes).
  //   1-bit XTC  — images are dithered to 2 tones by the Image dither model, in ONE
  //                pass from the source image. The AA switch decides whether TEXT is
  //                halftoned too (on) or left as crisp 1-bit ink (off, which also stops
  //                the writer thinning solid ink).
  // The engine, the writer and the mono preview all read the same spec switch, so the
  // preview shows the page the file will carry in either position.
  function syncAaToMode() {
    const hint = document.querySelector('.aaHint');
    if (hint) hint.textContent = state.mode === 0 ? '(텍스트 하프톤 · 이미지는 이미지 디더링 모델)' : '';
    const lab = document.getElementById('textAaLab');
    if (lab) lab.classList.remove('disabled');
  }

  // ---- whole-book export (1 book → 1 device file) ----
  function exportFilename(xtcz) {
    let n = (book ? book.title : 'book').replace(/[^\w\s가-힣\-\[\]]+/g, '').trim();
    if (!n) n = 'book';
    const pre = normalizeDisplayText((els.exportName.value || '').trim());   // §5: NFC first
    const ext = xtcz ? '.xtcz' : (state.mode === 0 ? '.xtc' : '.xtch');
    // the prefix keeps Hangul (w and the \w class do not cover it) and the result is trimmed,
    // otherwise a prefix made only of non-ASCII characters left a stray leading space
    const cleanPre = pre.replace(/[^\w\-\[\]가-힣]/g, '').trim();
    return (cleanPre ? cleanPre + ' ' : '') + n + ext;
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
      setExportLocked(true);
      els.downloadBtn.disabled = true;
      try {
        const res = await call('fetchWarm', {}, null, 60000);
        const u8 = new Uint8Array(res.file);
        const filename = exportFilename(res.xtcz !== undefined ? res.xtcz : xtcz);
        const ratio = res.xtcz && res.rawBytes ? ' (원본의 ' + (100 * u8.byteLength / res.rawBytes).toFixed(0) + '%)' : '';
        els.exportStatus.textContent = '✓ ' + filename + ' — ' + res.pages + '쪽, ' +
          (u8.byteLength / 1048576).toFixed(1) + ' MB' + ratio + ' (사전 변환)';
        setStatus('✓ 내보내기 완료 — ' + filename);   // §4: resolve the header, not just the panel
        saveBlob(u8, filename);
        warmSpecKey = null;   // bytes handed over; next warm refills
        refresh(true);        // engine was invalidated by the warm pass
      } catch (e) {
        els.exportStatus.textContent = '✗ 미리 변환본을 불러오지 못했습니다. 미리보기를 한 번 넘긴 뒤 다시 시도해 주세요.';
        setStatus('✗ 내보내기에 실패했습니다. 미리보기를 한 번 넘긴 뒤 다시 시도해 주세요.', true);
      } finally {
        setExportLocked(false);
        els.downloadBtn.disabled = !book;
      }
      return;
    }
    setExportLocked(true);
    els.downloadBtn.disabled = true;
    busy((mode === 0 ? 'XTC' : 'XTCH') + ' 생성 중 (책 전체)');
    const depthLabel = mode === 0 ? 'XTC 1-bit' : 'XTCH 2-bit';
    setStatus('책 전체를 ' + depthLabel + (xtcz ? ' + LZ4 (.xtcz)' : '') + '로 생성 중' +
              ' — 현재 설정으로 모든 페이지를 다시 렌더링합니다');
    els.exportStatus.textContent = '생성 중…';
    try {
      // Push the spec first: the export must use exactly the switch positions on
      // screen (in 1-bit the AA switch decides the blue-noise dither), and a knob
      // changed inside the repaint-coalescing window would otherwise not have
      // reached the engine yet. Idempotent and cheap.
      // §3: one message = one transaction. The worker applies this snapshot after taking the export
      // lock, so the file cannot be built from a mixture of settings.
      const res = await call('exportBook', { spec: readSpec(), mode, xtcz }, null, 600000);
      // Export cost, on the page, for measurement: per-spine ms proves where the time goes and how
      // unevenly it is distributed, which is the input a spine pool needs.
      if (res && res.spineTimes) {
        const totalMs = res.spineTimes.reduce((n, x) => n + x.ms, 0);
        const times = res.spineTimes.map((x) => x.ms).sort((a, b) => a - b);
        window.__koExportStats = {
          spines: res.spineTimes.length,
          pages: res.pages,
          bytes: res.bytes,
          spineMs: { min: times[0], p50: times[Math.floor(times.length * 0.5)],
                     p90: times[Math.floor(times.length * 0.9)], max: times[times.length - 1],
                     sum: +totalMs.toFixed(1) },
          spineTimes: res.spineTimes,
          preflight: res.preflight,
        };
      }
      const u8 = new Uint8Array(res.file);
      const filename = exportFilename(xtcz);
      const ratio = xtcz && res.rawBytes ? ' (원본의 ' + (100 * u8.byteLength / res.rawBytes).toFixed(0) + '%)' : '';
      els.exportStatus.textContent = '✓ ' + filename + ' — ' + res.pages + '쪽, ' +
        (u8.byteLength / 1048576).toFixed(1) + ' MB' + ratio;
      saveBlob(u8, filename);
      setStatus('✓ 내보내기 완료 — ' + filename);   // §4: otherwise desktop says "생성 중" forever
      // the export ran every spine through the engine; the next preview render
      // rebuilds the current spine so the page stays in sync with the book
      refresh(true);
      invalidateWarm();
      scheduleWarm(2000);
    } catch (e) {
      els.exportStatus.textContent = '✗ ' + describeError(e, '내보내기');
      setStatus('✗ ' + describeError(e, '내보내기'), true);
    } finally {
      setExportLocked(false);
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
    if (book) scheduleWarm(2000);
    updateExportSummary();
    savePrefs();
  });
  syncExportSeg();   // reflect initial mode (XTCH 2-bit) on the toggle
  syncAaToMode();    // enable AA (2-bit default mode)
  window.__exportPeek = null;   // (removed with the file-view mode)

  // keyboard paging
  // §3: the old guard skipped only INPUT/SELECT, so a focused <summary> (the ? help toggles), a
  // button, or anything inside the settings dialog paged the book at the same time — and with the
  // modal drawer open the book behind it is inert, so paging it is plainly wrong.
  const pagingSuppressed = (t) => {
    if (document.body.classList.contains('drawer-open')) return true;
    if (!t || !t.tagName) return true;
    const tag = t.tagName;
    if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return true;
    if (t.isContentEditable) return true;
    return !!(t.closest && t.closest('button, summary, a, label, [contenteditable], details'));
  };
  document.addEventListener('keydown', (e) => {
    if (pagingSuppressed(e.target)) return;
    if (e.key === 'ArrowLeft') els.prevBtn.click();
    else if (e.key === 'ArrowRight') els.nextBtn.click();
  });

  // ---- settings drawer (tablet/mobile): the sidebar becomes an off-canvas panel ----
  const drawerBackdrop = document.createElement('div');
  drawerBackdrop.className = 'drawerBackdrop';
  drawerBackdrop.hidden = true;
  document.body.appendChild(drawerBackdrop);

  // §7: below 1150 px this is visually modal — a dark backdrop blocks the preview, focus is moved
  // inside and Escape closes it — so it is now semantically modal too: role=dialog + aria-modal,
  // the background is inert, and Tab is contained. Before this a keyboard user could tab behind the
  // visible panel. §5: opened from 내보내기 it focuses the export action, not the first setting.
  // §6: focus returns to whichever control opened it, not always to 설정.
  const drawerOverlay = () => window.matchMedia('(max-width: 1150px)').matches;
  const drawerFocusables = (root) => Array.from((root || els.sidebar).querySelectorAll(
    'button:not([disabled]), select:not([disabled]), input:not([disabled]), summary, [href]'
  )).filter((el) => el.offsetParent !== null);
  let drawerReturnFocus = null;

  function setDrawer(open, opener, focusTarget, opts) {
    const isOpen = !!open;
    if (isOpen) document.body.classList.remove('status-active');   // §7
    document.body.classList.toggle('drawer-open', isOpen);
    els.drawerToggle.setAttribute('aria-expanded', isOpen ? 'true' : 'false');
    drawerBackdrop.hidden = !isOpen;
    const appbar = document.getElementById('appbar');
    const preview = document.getElementById('previewPane');
    if (isOpen) {
      if (opener) drawerReturnFocus = opener;
      if (drawerOverlay()) {
        els.sidebar.setAttribute('role', 'dialog');
        els.sidebar.setAttribute('aria-modal', 'true');
        if (appbar) appbar.setAttribute('inert', '');
        if (preview) preview.setAttribute('inert', '');
      }
      // default focus goes to the first SETTING; the close button stays reachable with Shift+Tab
      // and is part of the Tab wrap below
      const target = focusTarget || drawerFocusables(els.sidebar.querySelector('.sidebarScroll'))[0];
      if (target) target.focus({ preventScroll: true });
    } else {
      els.sidebar.removeAttribute('role');
      els.sidebar.removeAttribute('aria-modal');
      if (appbar) appbar.removeAttribute('inert');
      if (preview) preview.removeAttribute('inert');
      // remove inert before restoring focus, or the opener cannot take it
      const back = drawerReturnFocus || els.drawerToggle;
      drawerReturnFocus = null;
      // opts.keepFocus: normalizing after a breakpoint change must not yank focus around
      if (!(opts && opts.keepFocus) && back) back.focus({ preventScroll: true });
    }
  }

  // §2: the dialog's own close control
  const drawerClose = document.getElementById('drawerClose');
  if (drawerClose) drawerClose.addEventListener('click', () => setDrawer(false));

  // §1: crossing 1150 px with the drawer open used to leave drawer-open, role="dialog",
  // aria-modal and the inert attributes behind. CSS turns the sidebar back into a desktop column,
  // but the app bar and preview would stay inert — a dead UI. Normalize on the boundary.
  const drawerMq = window.matchMedia('(max-width: 1150px)');
  const onDrawerBoundary = () => { if (!drawerMq.matches) setDrawer(false, null, null, { keepFocus: true }); };
  if (drawerMq.addEventListener) drawerMq.addEventListener('change', onDrawerBoundary);
  else if (drawerMq.addListener) drawerMq.addListener(onDrawerBoundary);   // Safari < 14

  els.drawerToggle.addEventListener('click', (e) =>
    setDrawer(!document.body.classList.contains('drawer-open'), e.currentTarget));
  drawerBackdrop.addEventListener('click', () => setDrawer(false));
  document.addEventListener('keydown', (e) => {
    if (!document.body.classList.contains('drawer-open')) return;
    if (e.key === 'Escape') {
      e.preventDefault();
      setDrawer(false);
      return;
    }
    if (e.key === 'Tab' && drawerOverlay()) {          // contain focus inside the open drawer
      const f = drawerFocusables();
      if (!f.length) return;
      const first = f[0], last = f[f.length - 1];
      if (e.shiftKey && document.activeElement === first) {
        e.preventDefault();
        last.focus();
      } else if (!e.shiftKey && document.activeElement === last) {
        e.preventDefault();
        first.focus();
      }
    }
  });
  els.exportJump.addEventListener('click', (e) => {
    // the user asked for export: put focus on the export action itself
    setDrawer(true, e.currentTarget, els.downloadBtn && !els.downloadBtn.disabled ? els.downloadBtn : null);
  });

  // ---- boot ----
  // Restore remembered preferences before the engine sees a spec, so the first render already
  // uses the user's settings. Nothing about a book is ever stored.
  const restored = loadPrefs();
  if (restored) syncDependentControls();
  setAppState(book ? 'loaded' : 'empty');
  updateExportSummary();
  // §7 of the 1.1 audit: no eager probe here. convertFont() already probes the backend, and only
  // if in-browser conversion fails — doing it at boot meant a pointless POST to /api/convert-font on
  // every visit to discover that the Python dev endpoint is not deployed.
  worker = spawnWorker();
  // Readiness probe, immediate. This used to run behind
  // `requestIdleCallback(start, { timeout: 1500 })`, which deferred ONLY the ping that fills the
  // status line — the worker (and therefore the wasm fetch/compile) starts at the line above, and
  // ko.worker.js runs init() as soon as it is constructed. So the idle gate never delayed boot; it
  // delayed TELLING the user the engine was up, for up to 1.5 s, which is a UI that lies about its
  // own state. Removed for that reason. Boot time is unaffected, and this is not a boot
  // optimisation — the measured phases come from the worker's `bootstats`, not from here.
  const bootEngineWhenIdle = () => bootEngine().catch((e) => reportError(e, '엔진 시작'));
  bootEngineWhenIdle().then(() => {
    // convenience: /?epub=path triggers fetch+load (for local dev/test)
    const q = new URLSearchParams(location.search);
    const auto = q.get('epub');
    if (auto) {
      fetch(auto).then((r) => r.arrayBuffer()).then((buf) => {
        pendingBookFile = null;   // §8: no File behind a fetch-loaded book
        return loadBook(buf, auto.split('/').pop());
      });
    }
  });
})();
