#!/usr/bin/env node
// state_integrity_gate.js — the invariant this pass is built around:
//
//   A failed operation must leave either the LAST EXPLICITLY COMMITTED object intact, or nothing.
//   Never a half-book, a stale frame, a partial container, a stale plane, or a stale cover.
//
// It walks the sequence the audit asked for — valid -> corrupt -> valid -> induced failure -> valid — and
// after EVERY failure it sweeps the exported accessors. A sweep that only checks the call that failed
// would miss exactly the bug class this is for: state left readable through a DIFFERENT accessor.
//
// Usage: node scripts/verify/state_integrity_gate.js [book.epub]

const fs = require('fs');
const path = require('path');

const WASM_DIR = path.join(__dirname, '..', '..', 'build-wasm');   // scripts/verify/ -> repo root
const BOOK = process.argv[2] || path.join(__dirname, '..', '..', 'web', 'demo-png.epub');

let failures = 0;
function check(ok, label, detail) {
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${label}${!ok && detail ? ' — ' + detail : ''}`);
  if (!ok) failures++;
}

(async () => {
  const createKoEngine = require(path.join(WASM_DIR, 'ko_xtch_wasm.js'));
  const api = await createKoEngine();
  api._ko_init(464, 764);

  const errText = () => (api.UTF8ToString(api._ko_error()) || '');

  // Load bytes through the copying entry point (ownership stays with us).
  const loadBytes = (bytes) => {
    const p = api._malloc(bytes.length);
    api.HEAPU8.set(bytes, p);
    const n = api._ko_load_epub(p, bytes.length, '/book.epub');
    api._free(p);
    return n;
  };

  const good = fs.readFileSync(BOOK);
  const truncated = good.slice(0, Math.max(1, Math.floor(good.length / 3)));

  // ---- the sweeps ---------------------------------------------------------------------------------
  // Every accessor that can expose bytes from a book. Called after each failure.
  const sweepNoBook = (label) => {
    let title = '';
    try { const tb = api._malloc(512); api._ko_get_title(tb, 512); title = api.UTF8ToString(tb); api._free(tb); } catch (_) {}
    check(title === '', `${label}: title is empty`, `got "${title}"`);
    check(api._ko_build_spine ? api._ko_build_spine(0) < 0 : true, `${label}: build_spine refused`);
    check(api._ko_render_page(0) < 0, `${label}: render_page refused`);
    check(api._ko_generate_cover(0) < 0, `${label}: generate_cover refused`);
    check(api._ko_plane_size(0) === 0, `${label}: plane 0 size is 0`, `got ${api._ko_plane_size(0)}`);
    check(api._ko_plane_size(1) === 0, `${label}: plane 1 size is 0`, `got ${api._ko_plane_size(1)}`);
    check(api._ko_plane_ptr(0) === 0, `${label}: plane 0 pointer is null`);
    check(api._ko_compose_rgba(1) < 0, `${label}: compose refused`);
    check(api._ko_xtch_size() === 0, `${label}: container size is 0`, `got ${api._ko_xtch_size()}`);
    check(api._ko_xtch_ptr() === 0, `${label}: container pointer is null`);
    check(api._ko_cover_size ? api._ko_cover_size() === 0 : true, `${label}: cover size is 0`);
  };

  console.log('state-integrity gate — ' + path.basename(BOOK));

  // ---- 1. valid A ---------------------------------------------------------------------------------
  const nA = loadBytes(good);
  check(nA > 0, 'valid A loads', `spines=${nA}`);
  let titleOk = false;
  { const tb = api._malloc(512); api._ko_get_title(tb, 512); titleOk = api.UTF8ToString(tb).length > 0; api._free(tb); }
  check(titleOk, 'valid A has a title');

  // ---- 2. corrupt B: hasBook() must be false, through every accessor ------------------------------
  const nB = loadBytes(truncated);
  check(nB < 0, 'corrupt B is rejected', `returned ${nB}`);
  sweepNoBook('after corrupt B');
  // The specific bug this guards: a failed openEpub() used to leave epub_ non-null, so hasBook() said
  // true and every requireBook() guard passed against a half-initialised Epub.
  check(api._ko_spine_pages_estimated ? api._ko_spine_pages_estimated() === 0 : true,
        'after corrupt B: no page estimate is reported');

  // ---- 3. valid C after the failure ---------------------------------------------------------------
  const nC = loadBytes(good);
  check(nC === nA, 'valid C loads after the failure', `spines=${nC} vs ${nA}`);

  // ---- 4. export poisoning ------------------------------------------------------------------------
  // finish() with no active export must refuse rather than finalize an empty writer.
  check(api._ko_export_finish() < 0, 'export_finish refuses with no export active');
  check(api._ko_xtch_size() === 0, 'nothing was published by the refused finish');

  const begun = api._ko_export_begin();
  check(begun > 0, 'export_begin succeeds on a valid book', `spines=${begun}`);
  const badSpine = api._ko_export_spine(99999);
  check(badSpine < 0, 'export_spine on a non-existent spine fails', `returned ${badSpine}`);
  const fin = api._ko_export_finish();
  check(fin < 0, 'export_finish REFUSES to finalize a failed export', `returned ${fin}`);
  check(/cannot finalize failed export/.test(errText()),
        'the refusal names the reason', `error="${errText()}"`);
  check(api._ko_xtch_size() === 0, 'no partial container was published', `size=${api._ko_xtch_size()}`);

  // abort must clear the poison, or one bad export would poison the session
  api._ko_export_abort();
  const again = api._ko_export_begin();
  check(again > 0, 'a fresh export begins after an abort', `spines=${again}`);
  for (let s = 0; s < again; s++) api._ko_export_spine(s);
  const finished = api._ko_export_finish();
  check(finished > 0, 'the fresh export finalizes', `pages=${finished}`);
  check(api._ko_xtch_size() > 0 && api._ko_xtch_ptr() !== 0,
        'the fresh container is published', `size=${api._ko_xtch_size()}`);

  // ---- 5. one-shot convenience path fails fast ----------------------------------------------------
  api._ko_export_abort();
  const oneshot = api._ko_render_xtch();
  check(oneshot > 0 || oneshot === 0, 'ko_render_xtch succeeds on a healthy book', `pages=${oneshot}`);

  // ---- 6. a failed render must invalidate the COMPOSED frame too ----------------------------------
  // The planes and the RGBA framebuffer are committed separately: clearing g_page on failure used to leave
  // the previous page's pixels readable through ko_rgba_ptr().
  loadBytes(good);
  check(api._ko_build_spine(0) >= 0, 'build spine 0 for the rgba test');
  check(api._ko_render_page(0) === 0, 'render page 0');
  check(api._ko_compose_rgba(1) === 0, 'compose page 0');
  const rgbaGood = api._ko_rgba_ptr();
  check(rgbaGood !== 0, 'rgba_ptr is non-null after a successful compose');
  check(api._ko_render_page(99999) < 0, 'render of an out-of-range page fails');
  check(api._ko_plane_size(0) === 0, 'failed render cleared the planes');
  check(api._ko_rgba_ptr() === 0, 'failed render ALSO invalidated the composed frame');
  check(api._ko_compose_rgba(1) < 0, 'compose after the failed render is refused');

  // ---- 6b. layout mutation invalidates both pagination and rendered output ----------------------
  loadBytes(good);
  check(api._ko_build_spine(0) >= 0, 'build portrait spine for layout invalidation');
  check(api._ko_render_page(0) === 0 && api._ko_compose_rgba(0) === 0,
        'commit a portrait page before changing layout');
  check(api._ko_set_orientation(1) === 0, 'orientation changes to landscape CW');
  check(api._ko_plane_size(0) === 0 && api._ko_rgba_ptr() === 0,
        'orientation change clears planes and composed frame');
  check(api._ko_render_page(0) < 0,
        'orientation change invalidates the previously built section/current spine');
  check(api._ko_logical_width() === 800 && api._ko_logical_height() === 480,
        'landscape renderer geometry is active');
  check(api._ko_build_spine(0) >= 0 && api._ko_render_page(0) === 0,
        'landscape section must be rebuilt before rendering');
  const oldVw = api._ko_viewport_width(), oldVh = api._ko_viewport_height();
  check(api._ko_set_margins(5000, 5000, 5000, 5000) < 0,
        'oversized margins are refused');
  check(api._ko_viewport_width() === oldVw && api._ko_viewport_height() === oldVh,
        'refused margins leave the viewport unchanged');
  check(api._ko_set_orientation(0) === 0, 'orientation restored to portrait');

  // ---- 7. invalid spine indices are refused everywhere (they used to clamp to spine 0) ------------
  loadBytes(good);
  const count = api._ko_load_epub ? loadBytes(good) : 0;
  for (const bad of [-1, count, 2147483647]) {
    check(api._ko_build_spine(bad) < 0, `build_spine(${bad}) refused`);
    check(api._ko_start_spine(bad, 1) < 0, `start_spine(${bad}) refused`);
    check(api._ko_encode_spine(bad) < 0, `encode_spine(${bad}) refused`);
    check(api._ko_export_begin() > 0, 'export_begin for the invalid-spine case');
    check(api._ko_export_spine(bad) < 0, `export_spine(${bad}) refused`);
    check(api._ko_export_finish() < 0, `export_finish refused after export_spine(${bad})`);
    check(api._ko_xtch_size() === 0, `no container after export_spine(${bad})`);
    api._ko_export_abort();
  }

  // ---- 8. a replacement kills an in-flight export ------------------------------------------------
  check(api._ko_export_begin() > 0, 'export_begin for the replacement case');
  loadBytes(good);                                        // simulates the book being replaced mid-export
  check(api._ko_export_finish() < 0, 'export_finish refused after a book replacement');
  check(api._ko_xtch_size() === 0, 'no container after the replacement-invalidated export');
  api._ko_export_abort();

  // ---- 9. the assembler is a transaction --------------------------------------------------------
  // This module exposes HEAPU8 only, so 32-bit fields are written through a view over the heap buffer.
  const u32 = (ptr, n = 1) => new Uint32Array(api.HEAPU8.buffer, ptr, n);

  // A same-name replacement must use a distinct backing path: a malformed B
  // cannot overwrite/remove the live A.  A later book replacement must then
  // unregister A before clearing its storage.
  const makeFont = (bitmap) => {
    const b = new Uint8Array(32 + 12 + 16 + 1);
    const d = new DataView(b.buffer);
    d.setUint32(0, 0x46445045, true); d.setUint16(4, 1, true);
    d.setUint8(6, 1); d.setUint8(8, 20); d.setInt8(9, 15); d.setInt8(10, -5);
    d.setUint32(12, 1, true); d.setUint32(16, 1, true);
    d.setUint32(20, 32, true); d.setUint32(24, 44, true); d.setUint32(28, 60, true);
    d.setUint32(32, 0x41, true); d.setUint32(36, 0x41, true); d.setUint32(40, 0, true);
    d.setUint8(44, 1); d.setUint8(45, 1); d.setUint8(46, 1);
    d.setInt16(48, 0, true); d.setInt16(50, 1, true);
    d.setUint32(52, 1, true); d.setUint32(56, 0, true); d.setUint8(60, bitmap);
    return b;
  };
  const fontA = makeFont(0), badFontB = makeFont(0xff);
  new DataView(badFontB.buffer).setUint32(56, 0xffffffff, true);
  const fontPtr = api._malloc(fontA.length), badFontPtr = api._malloc(badFontB.length);
  const namePtr = api._malloc(5);
  api.HEAPU8.set(fontA, fontPtr); api.HEAPU8.set(badFontB, badFontPtr);
  api.HEAPU8.set([0x73, 0x61, 0x6d, 0x65, 0], namePtr);
  check(api._ko_load_epdfont(fontPtr, fontA.length, namePtr) === 0,
        'custom font A loads under a UI name');
  check(api._ko_load_epdfont(badFontPtr, badFontB.length, namePtr) < 0,
        'malformed same-name font B is rejected');
  check(api._ko_has_font(-999999) === 1 && api._ko_set_font(-999999) === 0,
        'same-name failure leaves custom font A live');
  loadBytes(good);
  check(api._ko_has_font(-999999) === 0 && api._ko_font() !== -999999,
        'book replacement explicitly unregisters the runtime custom font');
  api._free(fontPtr); api._free(badFontPtr); api._free(namePtr);

  const rec = new Uint8Array(22 + 96000);
  rec.set([0x58, 0x54, 0x48, 0x00, 0xe0, 0x01, 0x20, 0x03, 0x00, 0x00,
           0x00, 0x77, 0x01, 0x00], 0);  // XTH, 480x800, 96,000-byte two-plane payload
  const recPtr = api._malloc(rec.length); api.HEAPU8.set(rec, recPtr);
  const offPtr = api._malloc(4); u32(offPtr)[0] = 0;
  const lenPtr = api._malloc(4); u32(lenPtr)[0] = rec.length;
  check(api._ko_assemble_begin(1) === 0, 'assemble_begin on a valid book');
  check(api._ko_assemble_add_spine(recPtr, rec.length, 1, offPtr, lenPtr) > 0, 'one valid page record');
  // a truncated record: off + len deliberately past the end of the buffer
  u32(offPtr)[0] = rec.length;                           // off == size, so off + len > size
  check(api._ko_assemble_add_spine(recPtr, rec.length, 1, offPtr, lenPtr) < 0,
        'a truncated page record is refused');
  check(api._ko_assemble_finish() < 0, 'assemble_finish REFUSES after a bad record');
  check(api._ko_xtch_size() === 0, 'no container published by the failed assembly');
  check(api._ko_assemble_begin(1) === 0,
        'a failed assembly cleans itself up; a new one begins without an external abort');
  api._ko_export_abort();
  api._free(recPtr); api._free(offPtr); api._free(lenPtr);

  // ---- 10. the prefix planner is a transaction --------------------------------------------------
  const expectedGrayRecord = 22 + 2 * 48000;
  const lens = new Uint32Array([expectedGrayRecord, expectedGrayRecord]);
  const lensPtr = api._malloc(lens.length * 4); u32(lensPtr, lens.length).set(lens);
  check(api._ko_plan_begin(1) === 0, 'plan_begin on a valid book');
  check(api._ko_plan_add_spine(lensPtr, 2) > 0, 'two planned pages');
  check(api._ko_plan_add_spine(0, 1) < 0, 'a null length array is refused');
  check(api._ko_plan_finish() < 0, 'plan_finish REFUSES after a bad add');
  check(api._ko_plan_prefix_size() === 0, 'no prefix published by the failed plan');
  check(api._ko_plan_begin(1) === 0,
        'a failed plan cleans itself up; a new one begins without an external abort');
  api._ko_export_abort();
  api._free(lensPtr);

  // ---- 11. the 16-bit page limit is enforced, not wrapped ---------------------------------------
  const many = 65536;
  const bigPtr = api._malloc(many * 4);
  { const view = u32(bigPtr, many); for (let i = 0; i < many; i++) view[i] = expectedGrayRecord; }
  api._ko_plan_begin(1);
  check(api._ko_plan_add_spine(bigPtr, many) < 0,
        '65536 planned pages are refused (the header count is 16 bits)');
  check(api._ko_plan_finish() < 0, 'the over-limit plan cannot finish');
  check(api._ko_plan_prefix_size() === 0, 'no prefix for an over-limit plan');
  api._free(bigPtr);

  // ---- 11b. the container BYTE budget, which is a separate ceiling from the page count -----------
  // 12,000 pages is legal by the format (under 65,535) but describes 12,000 x 96,022 = 1.15 GiB of 2-bit
  // container, above the 1 GiB budget the module can actually hold. The page-count limit cannot catch this.
  const budgetPages = 12000;
  const budgetPtr = api._malloc(budgetPages * 4);
  { const view = u32(budgetPtr, budgetPages); for (let i = 0; i < budgetPages; i++) view[i] = expectedGrayRecord; }
  api._ko_plan_begin(1);
  check(api._ko_plan_add_spine(budgetPtr, budgetPages) < 0,
        'a LEGAL page count whose declared bytes exceed the budget is refused');
  check(api._ko_plan_finish() < 0, 'the over-budget plan cannot finish');
  check(api._ko_plan_prefix_size() === 0, 'no prefix published for an over-budget plan');

  // The planner accepts exactly the one record length the selected profile/mode can produce.
  const prefixEdge = new Uint32Array([expectedGrayRecord - 1]);
  const prefixEdgePtr = api._malloc(4); u32(prefixEdgePtr)[0] = prefixEdge[0];
  api._ko_export_abort();
  check(api._ko_plan_begin(1) === 0, 'plan_begin for prefix-budget edge');
  check(api._ko_plan_add_spine(prefixEdgePtr, 1) < 0,
        'planner rejects a non-canonical page-record length');
  check(api._ko_plan_finish() < 0, 'invalid-record plan cannot finish');
  api._ko_export_abort();

  // The add fits without chapters; finish must include the actual chapter table and reject before allocation.
  const chapterEdgePages = 11180;
  const chapterEdgePtr = api._malloc(chapterEdgePages * 4);
  { const view = u32(chapterEdgePtr, chapterEdgePages);
    for (let i = 0; i < chapterEdgePages; i++) view[i] = expectedGrayRecord; }
  check(api._ko_plan_begin(1) === 0, 'plan_begin for chapter-budget edge');
  check(api._ko_plan_add_spine(chapterEdgePtr, chapterEdgePages) === chapterEdgePages,
        'record plus fixed/index bytes fit just below the budget');
  let chapterRangesOk = true;
  for (let i = 0; i < 383; i++) {
    if (api._ko_plan_add_fallback(i, 0, 1) !== 0) chapterRangesOk = false;
  }
  check(chapterRangesOk, 'chapter metadata ranges are accepted before the aggregate finish check');
  check(api._ko_plan_finish() < 0,
        'plan_finish includes actual chapter-table bytes in the budget');
  check(api._ko_plan_prefix_size() === 0, 'chapter-over-budget plan allocates no prefix');
  api._ko_export_abort();
  api._free(prefixEdgePtr); api._free(chapterEdgePtr);

  const recPtr2 = api._malloc(64);
  const offPtr2 = api._malloc(budgetPages * 4);
  const lenPtr2 = api._malloc(budgetPages * 4);
  { const o = u32(offPtr2, budgetPages), l = u32(lenPtr2, budgetPages);
    for (let i = 0; i < budgetPages; i++) { o[i] = 0; l[i] = expectedGrayRecord; } }
  api._ko_assemble_begin(1);
  check(api._ko_assemble_add_spine(recPtr2, 64, budgetPages, offPtr2, lenPtr2) < 0,
        'the assembler refuses a page count above the byte budget');
  check(api._ko_assemble_finish() < 0, 'the over-budget assembly cannot finish');
  check(api._ko_xtch_size() === 0, 'no container published by the over-budget assembly');
  api._ko_export_abort();
  api._free(budgetPtr); api._free(recPtr2); api._free(offPtr2); api._free(lenPtr2);

  // Metadata arithmetic is checked locally and a bad range terminates the transaction.
  const onePtr = api._malloc(4); u32(onePtr)[0] = expectedGrayRecord;
  check(api._ko_plan_begin(1) === 0 && api._ko_plan_add_spine(onePtr, 1) === 1,
        'one-page plan starts for metadata range checks');
  check(api._ko_plan_add_toc(2147483647, 0, 2147483647) < 0,
        'overflowing TOC page arithmetic is rejected');
  check(api._ko_plan_begin(1) === 0,
        'metadata rejection also rolls the plan back');
  api._ko_export_abort();
  check(api._ko_plan_begin(1) === 0 && api._ko_plan_add_spine(onePtr, 2147483647) < 0,
        'absurd planner count is rejected before dereferencing the length array');
  check(api._ko_plan_begin(1) === 0,
        'absurd-count rejection also rolls the plan back');
  api._ko_export_abort();
  api._free(onePtr);

  // ---- 12. with no book, the assembler and planner refuse ---------------------------------------
  loadBytes(truncated);
  check(api._ko_assemble_begin(1) < 0, 'assemble_begin refused with no book');
  check(api._ko_plan_begin(1) < 0, 'plan_begin refused with no book');

  // ---- 13. valid E, and the state is still coherent ------------------------------------------------
  const nE = loadBytes(good);
  check(nE === nA, 'valid E still loads after everything', `spines=${nE}`);

  // Raw ABI setters enforce the same positive enums as the worker boundary.
  check(api._ko_set_line_compression(NaN) < 0, 'NaN line compression is rejected at the C ABI');
  check(api._ko_set_paragraph_alignment(999) < 0, 'invalid alignment is rejected at the C ABI');
  check(api._ko_set_image_dither(-500) < 0, 'invalid dither is rejected at the C ABI');
  check(api._ko_set_image_tone_depth(3) < 0, 'invalid tone depth is rejected at the C ABI');

  check(api._ko_render_xtch() >= 0, 'a complete container exists for XTCZ state checks');
  check(api._ko_xtcz_wrap() === 0, 'one XTCZ wrap succeeds');
  const onceWrapped = api._ko_xtch_size();
  check(api._ko_xtcz_wrap() < 0, 'a second XTCZ wrap is explicitly rejected');
  check(api._ko_xtch_size() === onceWrapped, 'double-wrap rejection leaves the committed XTCZ intact');

  // ---- 14. repeated init is a complete lifecycle reset ------------------------------------------
  check(api._ko_set_device_profile(3) === 0, 'switch to X3 before repeated init');
  check(api._ko_set_orientation(1) === 0, 'switch to landscape before repeated init');
  check(api._ko_init(464, 764) === 0, 'repeated ko_init succeeds');
  check(api._ko_device_profile() === 4 && api._ko_orientation() === 0,
        'repeated init restores the default X4 portrait spec');
  check(api._ko_logical_width() === 480 && api._ko_logical_height() === 800,
        'repeated init restores X4 renderer geometry');
  sweepNoBook('after repeated init');

  console.log();
  if (failures) {
    console.log(`FAIL — ${failures} check(s) failed`);
    process.exit(1);
  }
  console.log('PASS — after every failure the engine exposes no book, no planes, no cover and no partial '
              + 'container, and it recovers to a working book and a publishable export');
})();
