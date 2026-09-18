// Split-engine behavioural probe. Paste into the devtools console on a page that has a book open
// (the app's own ?epub=<name> auto-load is fine), then read the returned object.
//
// WHY A PROBE AND NOT A TEST RUNNER: the properties that matter here only exist in a browser — two
// real Workers, a real WASM heap each, and a real transformable Blob. There is no headless runner in
// this repo, and inventing one to avoid a paste would add a dependency the project does not otherwise
// need. The static half (scripts/verify/split_engine_gate.py) runs anywhere; this half is the
// evidence, and its results belong in the commit message when the split changes.
//
// SETUP: put a metrically different TTF where the page can fetch it, e.g.
//   cp "/System/Library/Fonts/Supplemental/Courier New.ttf" web/__probe.ttf
// and set PROBE_FONT below to that name. Delete it afterwards — it must not be committed.
//
// WHAT IT ASSERTS (each caught a real regression):
//   A. the export engine refuses font:"custom" when it has no such font   (was: silently exported KoPub)
//   B. custom export succeeds once the font is the page's asset           (sync path)
//   C. custom export != KoPub export                                      (the font really is in use)
//   D. custom export after an engine KILL == custom export before it      (rebuild + restore)
//   E. a normal KoPub export still works                                  (the guard is not a wall)

const PROBE_FONT = '__probe.ttf';

async function splitEngineProbe() {
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const sha = async (ab) => {
    const b = new Uint8Array(ab), m = b.slice();
    for (let i = 296; i < 304; i++) m[i] = 0;   // createTime differs between runs; nothing else may
    const d = await crypto.subtle.digest('SHA-256', m);
    return [...new Uint8Array(d)].map((x) => x.toString(16).padStart(2, '0')).join('').slice(0, 24);
  };
  const exportWith = (font, ms = 25000) =>
    window.__export.call('exportBook', { spec: { font }, mode: 1, xtcz: false }, null, ms);

  const out = { A_guard: null, B_custom: null, C_differs: null, D_afterKill: null, E_kopub: null };
  if (!window.__export) return { error: 'window.__export missing — this build predates the debug hook' };

  // A. the export engine must refuse a font it was never given (bug 1's guard)
  try {
    await exportWith('custom');
    out.A_guard = 'FAIL — export ran with an unavailable custom font';
  } catch (e) {
    out.A_guard = /not available in this engine/.test(e.message)
      ? 'PASS — refused: ' + e.message : 'FAIL — unexpected error: ' + e.message;
  }

  // E. KoPub must still export (the guard must not be a wall)
  try {
    const k = await window.__export.call('exportBook', { spec: { font: 'kopub' }, mode: 1, xtcz: false }, null, 25000);
    out.E_kopub = { pages: k.pages, sha: await sha(k.file) };
  } catch (e) { out.E_kopub = 'FAIL — ' + e.message; }

  // set the custom font through the app's own UI path, as a user would
  const buf = await (await fetch(PROBE_FONT)).arrayBuffer();
  const dt = new DataTransfer();
  dt.items.add(new File([buf], PROBE_FONT, { type: 'font/ttf' }));
  const radio = [...document.querySelectorAll('input[type=radio]')]
    .find((r) => /사용자|custom/i.test(r.closest('label')?.textContent || ''));
  if (radio) { radio.checked = true; radio.dispatchEvent(new Event('change', { bubbles: true })); }
  const input = document.getElementById('fontFile');
  input.files = dt.files;
  input.dispatchEvent(new Event('change', { bubbles: true }));
  for (let i = 0; i < 25; i++) {
    await sleep(1000);
    if (/활성/.test(document.getElementById('fontConvStatus')?.textContent || '')) break;
  }
  out.fontActive = /활성/.test(document.getElementById('fontConvStatus')?.textContent || '');

  // B + C
  let customSha = null;
  try {
    const c = await exportWith('custom');
    customSha = await sha(c.file);
    out.B_custom = { pages: c.pages, sha: customSha };
    out.C_differs = (out.E_kopub && out.E_kopub.sha && customSha !== out.E_kopub.sha)
      ? 'PASS — the custom font reaches the export engine'
      : 'FAIL — identical to the KoPub export (the font did not reach this engine)';
  } catch (e) { out.B_custom = 'FAIL — ' + e.message; out.C_differs = 'n/a'; }

  // D. destroy the engine, let the app rebuild + restore, and require the same bytes
  window.__cap = null;
  if (!window.__hooked) {
    const orig = URL.createObjectURL.bind(URL);
    URL.createObjectURL = (b) => { window.__cap = b; return orig(b); };
    window.__hooked = true;
  }
  window.__export.kill();
  document.getElementById('downloadBtn').click();
  for (let i = 0; i < 30 && !window.__cap; i++) await sleep(1000);
  if (!window.__cap) {
    out.D_afterKill = 'FAIL — no file after the kill: ' +
      (document.body.innerText.match(/[✓✗⚠][^\n]*/) || ['(no status)'])[0];
  } else {
    const after = await sha(await window.__cap.arrayBuffer());
    out.D_afterKill = (customSha && after === customSha)
      ? 'PASS — rebuilt engine reproduced ' + after
      : `FAIL — before ${customSha} vs after ${after}`;
  }

  const verdict = ['A_guard', 'C_differs', 'D_afterKill'].every((k) => /^PASS/.test(out[k] || ''));
  out.VERDICT = verdict ? 'PASS — split-engine behaviour verified' : 'FAIL — see the fields above';
  return out;
}
splitEngineProbe();

// ---------------------------------------------------------------------------------------------
// Finding 1: a reused pool must not keep exporting a previous custom face.
//
// The pool's engines hold the book AND the font bytes; everything else arrives per call. So the
// identity of a pool has to include the font generation, and a new canonical face must destroy the
// pool. Measured before the fix: after regenerating the font, ensurePool() matched on
// (bookEpoch, engineCount) alone and returned the OLD engines, which still had the old .epdfont
// loaded — a successful export with the previous face.
//
// Page counts are NOT the discriminator here: on an image-heavy book both faces paginate identically
// (that is how the split-engine bug survived its first test). The check is the byte hash.
async function poolFontProbe(fontAPath, fontBPath) {
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const sha = async (ab) => {
    const b = new Uint8Array(ab), m = b.slice();
    for (let i = 296; i < 304; i++) m[i] = 0;
    const d = await crypto.subtle.digest('SHA-256', m);
    return [...new Uint8Array(d)].map((x) => x.toString(16).padStart(2, '0')).join('').slice(0, 24);
  };
  const setFont = async (path) => {
    const buf = await (await fetch(path)).arrayBuffer();
    const dt = new DataTransfer();
    dt.items.add(new File([buf], path, { type: 'font/ttf' }));
    const radio = [...document.querySelectorAll('input[type=radio]')]
      .find((r) => /사용자|custom/i.test(r.closest('label')?.textContent || ''));
    if (radio) { radio.checked = true; radio.dispatchEvent(new Event('change', { bubbles: true })); }
    const input = document.getElementById('fontFile');
    input.files = dt.files;
    input.dispatchEvent(new Event('change', { bubbles: true }));
    for (let i = 0; i < 30; i++) {
      await sleep(1000);
      if (/활성/.test(document.getElementById('fontConvStatus')?.textContent || '')) break;
    }
    // one more tick so the conversion's applyCustomFont has run and the pool is gone
    await sleep(500);
    return window.__pool.fontGeneration();
  };

  const out = {};
  out.generationStart = window.__pool.fontGeneration();

  // A
  out.genA = await setFont(fontAPath);
  const a = await window.__pool.run(1, false, 4);
  out.A = { sha: await sha(a.file), pages: a.pages };

  // KoPub, for the "custom != built-in" limb
  const k = await window.__export.call('exportBook', { spec: { font: 'kopub' }, mode: 1, xtcz: false }, null, 120000);
  out.kopub = { sha: await sha(k.file), pages: k.pages };
  out.A_differs_from_kopub = out.A.sha !== out.kopub.sha ? 'PASS' : 'FAIL — custom A exported as KoPub';

  // B, regenerated through the app's own path
  const genBeforeB = window.__pool.fontGeneration();
  out.genB = await setFont(fontBPath);
  out.generation_advanced = out.genB > genBeforeB ? 'PASS (' + genBeforeB + ' -> ' + out.genB + ')'
                                                  : 'FAIL — generation did not advance';
  out.pool_destroyed_on_new_font = window.__pool.engines() === 0
    ? 'PASS' : 'FAIL — ' + window.__pool.engines() + ' engines survived the new face';

  const b = await window.__pool.run(1, false, 4);
  out.B = { sha: await sha(b.file), pages: b.pages };
  out.B_differs_from_A = out.B.sha !== out.A.sha
    ? 'PASS' : 'FAIL — B exported the bytes of A (stale pool: ' + out.B.sha + ')';
  out.B_differs_from_kopub = out.B.sha !== out.kopub.sha ? 'PASS' : 'FAIL';

  // destroy/rebuild: B before the rebuild must equal B after it
  window.__pool.kill();
  await sleep(300);
  const b2 = await window.__pool.run(1, false, 4);
  const shaB2 = await sha(b2.file);
  out.B_stable_across_rebuild = shaB2 === out.B.sha
    ? 'PASS — ' + shaB2 : 'FAIL — before ' + out.B.sha + ' vs after ' + shaB2;

  out.VERDICT = [out.generation_advanced, out.pool_destroyed_on_new_font, out.A_differs_from_kopub,
                 out.B_differs_from_A, out.B_differs_from_kopub, out.B_stable_across_rebuild]
    .every((v) => /^PASS/.test(v || '')) ? 'PASS — pool font identity verified' : 'FAIL — see the fields';
  return out;
}
// poolFontProbe('__probeA.ttf', '__probeB.ttf');

