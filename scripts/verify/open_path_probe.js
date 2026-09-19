// Open-path measurement harness: drives the PRODUCT's worker (ko.worker.js, unmodified) directly, so the
// same postMessage protocol the page uses is what gets measured.
//
// WHY NOT THE PAGE: app.js spawns its worker at page load, and by the time any script can dispatch a
// book the module is already initialised — initWaitMs comes back 0 and the read-ahead has nothing to
// overlap. To see the overlap you need a worker whose module is still booting at the moment the load
// arrives, which is the cold-first-visit case the read-ahead exists for.
//
// Each row creates its own worker with a cache-busting query on the ENGINE GLUE (the worker re-fetches
// and re-compiles the 1.38 MB module), posts the open IMMEDIATELY — without waiting for init — and reads
// the worker's own phase timings. `noEarlyRead: true` is the control: the read starts after initPromise
// instead of before it, which is what the worker did before the change. Same file, same book, one flag.
//
// usage: await openArm({ book: 'demo.epub', noEarlyRead: false, label: '12 MB read-ahead' })
(() => {
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  async function openArm({ book, noEarlyRead, bulkRead, label, timeoutMs = 180000 }) {
    const t0 = performance.now();
    const blob = await (await fetch(book)).blob();
    const fetchMs = performance.now() - t0;

    // Fresh worker per arm. The cache-buster is on the worker's own importScripts target so the browser
    // cannot reuse a previously compiled module — otherwise every arm measures a warm engine.
    const cb = Math.random().toString(36).slice(2, 8);
    const worker = new Worker('ko.worker.js?cb=' + cb);
    const startAt = performance.now();

    const reply = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('timeout')), timeoutMs);
      worker.onmessage = (ev) => {
        const m = ev.data;
        if (m && m.id === 1) {
          clearTimeout(timer);
          if (!m.ok) reject(new Error((m.error && m.error.toString()) || 'worker error'));
          else resolve(m.result || m);
        }
      };
      worker.onerror = (e) => { clearTimeout(timer); reject(new Error('worker onerror: ' + e.message)); };
      // Posted BEFORE the worker could have finished init: `new Worker()` returns immediately and the
      // message is queued behind module evaluation, so the read starts as early as the engine allows.
      worker.postMessage(Object.assign({ id: 1, cmd: 'openPreview', blob, mode: 1 },
                                       noEarlyRead ? { noEarlyRead: true } : {},
                                       bulkRead ? { bulkRead: true } : {}));
    });

    const wallMs = performance.now() - startAt;
    const t = (reply && reply.timing) || {};
    worker.terminate();
    return {
      label,
      book,
      blobMB: +(blob.size / 1048576).toFixed(1),
      pageFetchMs: +fetchMs.toFixed(1),
      wallMs: +wallMs.toFixed(1),
      initWaitMs: t.initWaitMs,
      blobReadMs: t.blobReadMs,
      blobWaitAfterInitMs: t.blobWaitAfterInitMs,
      blobBootOverlapMs: t.blobBootOverlapMs,
      wasmMemcpyMs: t.wasmMemcpyMs,
      engineLoadMs: t.engineLoadMs,
      engineSpanMs: t.engineSpanMs,
      earlyReadSkipReason: t.earlyReadSkipReason,
      externalCrossings: t.externalCrossings,
      externalPhysicalBytes: t.externalPhysicalBytes,
      externalPctOfFile: t.externalPctOfFile,
      externalRamReads: t.externalRamReads,
      externalReadMs: t.externalReadMs,
      heapAfterLoadBytes: t.heapAfterLoadBytes,
      buildMs: t.buildMs,
      renderMs: t.renderMs,
      composeMs: t.composeMs,
      totalOpenWorkerMs: t.totalOpenWorkerMs,
      path: t.path,
      spines: reply.spineCount,
    };
  }

  // The open-path A/B for the read-ahead, on the BULK path (the only one with a read to overlap).
  async function abPair(book) {
    const base = { book, bulkRead: true };
    const early = await openArm(Object.assign({}, base, { label: 'read-ahead ON', noEarlyRead: false }));
    const serial = await openArm(Object.assign({}, base, { label: 'read-ahead OFF (control)', noEarlyRead: true }));
    return {
      book,
      early,
      serial,
      overlapMs: +(early.blobBootOverlapMs || 0).toFixed(1),
      initWaitEarly: early.initWaitMs,
      initWaitSerial: serial.initWaitMs,
      readMs: early.blobReadMs,
    };
  }

  // Range-backed mount vs whole-file ingest: the P4 measurement.
  async function mountAB(book) {
    const ext = await openArm({ book, label: 'range-backed mount' });
    const bulk = await openArm({ book, label: 'whole-file ingest (control)', bulkRead: true });
    return {
      book,
      external: ext,
      bulk,
      openWorkerMsSaved: +((bulk.totalOpenWorkerMs || 0) - (ext.totalOpenWorkerMs || 0)).toFixed(1),
    };
  }

  window.__openArm = openArm;
  window.__openAB = abPair;
  window.__mountAB = mountAB;
  return 'harness ready';
})()
