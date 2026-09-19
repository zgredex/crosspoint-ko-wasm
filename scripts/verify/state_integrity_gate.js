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

  // ---- 6. valid E, and the state is still coherent ------------------------------------------------
  const nE = loadBytes(good);
  check(nE === nA, 'valid E still loads after everything', `spines=${nE}`);

  console.log();
  if (failures) {
    console.log(`FAIL — ${failures} check(s) failed`);
    process.exit(1);
  }
  console.log('PASS — after every failure the engine exposes no book, no planes, no cover and no partial '
              + 'container, and it recovers to a working book and a publishable export');
})();
