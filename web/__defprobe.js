// Browser-side probe for the defensive correctness pass — stages 1, 2 and 3.
//
// Everything here drives the REAL page and the REAL worker: `window.__call` is the page's own call(),
// and the stage-1 page-half and the stage-2 checks go through the actual file input, because those code
// paths live in loadBook() and cannot be reached by calling the worker directly.
//
// Not timing-sensitive: the assertions are about committed STATE (which book the worker holds, which
// title the page shows, which affordances are enabled), not about how long anything took. The only
// timing-shaped input is that A is a bigger book than B, which makes A the slower load and therefore the
// interesting order to lose.
//
// usage:  await defensiveProbe('demo-images.epub', 'demo-png.epub')
(() => {
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  const title = () => (document.querySelector('#bookTitle') || {}).textContent || '';
  const downloadDisabled = () => !!(document.querySelector('#downloadBtn') || {}).disabled;
  const prevDisabled = () => !!(document.querySelector('#prevBtn') || {}).disabled;
  const chapterCount = () => {
    const s = document.querySelector('select');
    return s ? s.options.length : 0;
  };

  async function waitForApp() {
    for (let i = 0; i < 400 && typeof window.__call !== 'function'; i++) await sleep(25);
  }

  async function dispatch(name) {
    const file = new File([await (await fetch(name)).arrayBuffer()], name, { type: 'application/epub+zip' });
    const dt = new DataTransfer();
    dt.items.add(file);
    const input = document.querySelector('input[type=file]');
    input.files = dt.files;
    input.dispatchEvent(new Event('change', { bubbles: true }));
  }

  async function dispatchCorrupt(name) {
    const bytes = new Uint8Array(4096);
    for (let i = 0; i < bytes.length; i++) bytes[i] = (i * 131) & 0xff;   // not a ZIP
    const file = new File([bytes], name, { type: 'application/epub+zip' });
    const dt = new DataTransfer();
    dt.items.add(file);
    const input = document.querySelector('input[type=file]');
    input.files = dt.files;
    input.dispatchEvent(new Event('change', { bubbles: true }));
  }

  async function settle(ms) {
    // Let the page finish whatever it started. State assertions follow, so a slow machine only delays
    // the verdict, it does not change it.
    const start = performance.now();
    while (performance.now() - start < ms) {
      await sleep(50);
      if (!document.querySelector('#status').textContent.includes('분석')) await sleep(150);
    }
  }

  // ---- stage 1: two replacements back to back, the LAST must win --------------------------------
  async function lastSelectionWins(a, b) {
    await waitForApp();
    const titleB = { name: b };
    window.__koOpenSuperseded = 0;
    await dispatch(a);                      // slow, larger book
    await sleep(10);                        // same tick-ish: B is requested while A is in flight
    await dispatch(b);                      // smaller book
    await settle(6000);

    const worker = await window.__call('ping', {}, null, 20000);
    return {
      pageTitle: title(),
      superseded: window.__koOpenSuperseded || 0,
      chapters: chapterCount(),
      workerSpiders: worker && worker.spines,
      note: 'worker identity is read through the frame the page is showing, below',
    };
  }

  // ---- stage 1 (worker half): the engine must end up holding the LAST book requested ------------
  async function workerLastWriterWins(a, b) {
    await waitForApp();
    const blobA = await (await fetch(a)).blob();
    const blobB = await (await fetch(b)).blob();
    // Both posted without awaiting: the worker's queue must serialize them, and B must be the book the
    // engine ends up holding. Renders after this must show B, not A.
    const pA = window.__call('openPreview', { blob: blobA, mode: 1 }, null, 120000);
    const pB = window.__call('openPreview', { blob: blobB, mode: 1 }, null, 120000);
    const [rA, rB] = await Promise.all([pA.catch((e) => ({ error: String(e) })),
                                        pB.catch((e) => ({ error: String(e) }))]);
    const frame = await window.__call('render', { spine: 0, page: 0, mode: 1 }, null, 120000);
    const bytes = new Uint8Array(frame.image);
    const d = await crypto.subtle.digest('SHA-256', bytes);
    return {
      spineA: rA.spineCount, titleA: rA.title, errA: rA.error || null,
      spineB: rB.spineCount, titleB: rB.title, errB: rB.error || null,
      frameSha: [...new Uint8Array(d)].map((x) => x.toString(16).padStart(2, '0')).join('').slice(0, 16),
    };
  }

  // ---- stage 2: a failed replacement must not leave the previous book on screen ------------------
  async function failClosed(a) {
    await waitForApp();
    await dispatch(a);
    await settle(6000);
    const good = { pageTitle: title(), download: downloadDisabled(), chapters: chapterCount() };

    await dispatchCorrupt('corrupt.epub');
    await settle(6000);
    const after = {
      pageTitle: title(),
      downloadDisabled: downloadDisabled(),
      prevDisabled: prevDisabled(),
      chapters: chapterCount(),
      fallback: window.__koOpenFallback || null,
      status: (document.querySelector('#status') || {}).textContent || '',
      renderAttempt: null,
    };
    // The engine must have no book to render: this is the "cannot accidentally render A" assertion.
    try {
      await window.__call('render', { spine: 0, page: 0, mode: 1 }, null, 15000);
      after.renderAttempt = 'rendered (unexpected)';
    } catch (e) {
      after.renderAttempt = 'refused: ' + String(e.message || e).slice(0, 60);
    }
    return { beforeFailure: good, afterFailure: after };
  }

  // ---- spinner gate: the overlay must be gone once the call it describes has settled --------------
  // The reported symptom was "EPUB 분석 중… 53s" over a book that had already opened. The overlay counts
  // from a page-side timer, so it cannot itself say whether the engine is alive — these checks therefore
  // assert the two things that CAN be checked: the overlay is cleared after a settled open (success AND
  // failure), and a superseded load never clears a newer load's spinner.
  async function spinnerClears(a) {
    await waitForApp();
    const out = {};

    await dispatch(a);
    await settle(8000);
    out.afterSuccess = {
      pending: !!window.__koOpenPending,
      visible: window.__koLoadingVisible ? window.__koLoadingVisible() : null,
      assert: (() => { try { return window.__koAssertOverlayClear('successful open'); }
                       catch (e) { return 'THREW: ' + e.message; } })(),
      status: (document.querySelector('#status') || {}).textContent || '',
    };

    await dispatchCorrupt('corrupt.epub');
    await settle(8000);
    out.afterFailure = {
      pending: !!window.__koOpenPending,
      visible: window.__koLoadingVisible ? window.__koLoadingVisible() : null,
      assert: (() => { try { return window.__koAssertOverlayClear('failed open'); }
                       catch (e) { return 'THREW: ' + e.message; } })(),
    };

    // A (slow) superseded by B: while B is still loading, A finishing must not hide B's spinner.
    window.__koOpenSuperseded = 0;
    const bBlob = (await (await fetch('demo-png.epub')).blob());
    const seenDuringB = [];
    await dispatch('demo.epub');                     // slow
    await sleep(10);
    const pB = dispatch('demo-png.epub');            // fast book, but still a full open
    for (let i = 0; i < 12; i++) {
      await sleep(120);
      seenDuringB.push(window.__koLoadingVisible ? window.__koLoadingVisible() : null);
    }
    await pB;
    await settle(6000);
    out.overlap = {
      superseded: window.__koOpenSuperseded || 0,
      overlaySeenWhileWaiting: seenDuringB.some((v) => v === true),
      finalPending: !!window.__koOpenPending,
      finalVisible: window.__koLoadingVisible ? window.__koLoadingVisible() : null,
      finalAssert: (() => { try { return window.__koAssertOverlayClear('overlap'); }
                            catch (e) { return 'THREW: ' + e.message; } })(),
    };
    return out;
  }

  window.__defensive = { lastSelectionWins, workerLastWriterWins, failClosed, spinnerClears,
                         dispatch, dispatchCorrupt, settle };
  return 'defensive probe ready';
})()
