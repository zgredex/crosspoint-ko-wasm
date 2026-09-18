#!/usr/bin/env node
/*
 * dither_decision.js - does a 1-bit BLUE-NOISE halftone of greys earn its keep, measured
 * against non-dithered 1-bit candidates and scored against the 2-bit + AA page?
 *
 * SCORING SPACE IS PANEL REFLECTANCE, NOT PREVIEW RGB. The four panel states are modelled as
 * reflectance white 210 / dark grey 30 / light grey 80 / black 15 (the {15,30,80,210} anchors
 * in vendor-lib/Epub/Epub/converters/DitherUtils.h kProfileMaster, i.e. the same model the
 * writer's kMonoInkDensity derives its 92%/67% densities from). Scoring against the
 * preview's RGB greys (255/128/205/0) instead is a trap: it silently rewrites the target as
 * "dark grey = 50% ink", under which a correct halftone looks 92% too dark. Measured both
 * ways, which is how the trap was found.
 *
 * Candidates (all from the engine, nothing simulated):
 *   R  = ko_compose_rgba(0)   the 2-bit + AA page, 4 levels
 *   D  = ko_compose_rgba(1)   the SHIPPED 1-bit path: grey pixels become blue-noise dots
 *   N1 = 50% rule             no dither: dark grey + black inked, light grey dropped
 *   N2 = raw BW plane         no dither: every non-white pixel inked
 *
 * Metrics:
 *   pxErr     mean |reflectance difference| per pixel      - pixel accuracy (dither gives this up)
 *   toneErr   |local mean reflectance difference| / 195      - tone as the eye integrates it
 *   lvlErr    toneErr restricted to the reference's grey/black pixels - "is this level right?"
 *   inkLaid   % of that level's pixels inked, vs its ideal density
 *   drop      % of solid-black pixels turned white          - ink loss
 *   pepper    isolated pixels per 1000                      - speckle the eye reads as texture
 *
 * Usage: dither_decision.js <book.epub> <spine> <page> [<page>...] [--aa 0|1]
 */
const fs = require('fs');
const path = require('path');

const WASM_DIR = path.join(__dirname, '..', '..', 'build-wasm');
const createKoEngine = require(path.join(WASM_DIR, 'ko_xtch_wasm.js'));
const LW = 480, LH = 800, NPX = LW * LH, RAD = 2;

const argv = process.argv.slice(2);
const book = argv[0], spine = Number(argv[1]);
const aaIdx = argv.indexOf('--aa');
const AA = aaIdx >= 0 ? Number(argv[aaIdx + 1]) : 1;
const pages = [];
for (let i = 2; i < argv.length; i++) {
  if (argv[i] === '--aa') { i++; continue; }
  if (/^\d+$/.test(argv[i])) pages.push(Number(argv[i]));
}

// preview palette (what ko_compose_rgba emits) -> panel reflectance
const PREVIEW = { 255: 210, 128: 30, 205: 80, 0: 15 };
const R_WHITE = 210, R_BLACK = 15, RANGE = R_WHITE - R_BLACK;   // 195
const IDEAL_INK = { 210: 0, 30: 0.923, 80: 0.667, 15: 1.0 };    // = kMonoInkDensity/255

const reflect = (previewGrey) => PREVIEW[previewGrey];
// a 1-bit candidate: black pixel = ink = R_BLACK, white pixel = R_WHITE
const reflectMono = (g) => (g === 255 ? R_WHITE : R_BLACK);

function boxMean(arr, radius) {
  const out = new Float32Array(NPX);
  for (let y = 0; y < LH; y++) {
    const y0 = Math.max(0, y - radius), y1 = Math.min(LH - 1, y + radius);
    for (let x = 0; x < LW; x++) {
      const x0 = Math.max(0, x - radius), x1 = Math.min(LW - 1, x + radius);
      let acc = 0;
      for (let yy = y0; yy <= y1; yy++) {
        const row = yy * LW;
        for (let xx = x0; xx <= x1; xx++) acc += arr[row + xx];
      }
      out[y * LW + x] = acc / ((y1 - y0 + 1) * (x1 - x0 + 1));
    }
  }
  return out;
}

function score(refR, candR, candPreview, name) {
  let pxErr = 0;
  for (let i = 0; i < NPX; i++) pxErr += Math.abs(refR[i] - candR[i]);
  pxErr /= NPX;

  const rm = boxMean(refR, RAD), cm = boxMean(candR, RAD);
  let toneSum = 0;
  const lvl = new Map();
  for (let i = 0; i < NPX; i++) {
    const e = Math.abs(rm[i] - cm[i]);
    toneSum += e;
    const r = refR[i];
    const cur = lvl.get(r) || [0, 0];
    cur[0] += e; cur[1]++;
    lvl.set(r, cur);
  }

  let blackTotal = 0, dropped = 0;
  const laid = new Map();
  for (let i = 0; i < NPX; i++) {
    const r = refR[i];
    const cur = laid.get(r) || [0, 0];
    cur[1]++;
    if (candPreview[i] !== 255) cur[0]++;
    laid.set(r, cur);
    if (r === R_BLACK) { blackTotal++; if (candPreview[i] === 255) dropped++; }
  }

  let pepper = 0;
  for (let y = 1; y < LH - 1; y++) {
    for (let x = 1; x < LW - 1; x++) {
      const i = y * LW + x, dark = candPreview[i] !== 255;
      const same = (j) => (candPreview[j] !== 255) === dark;
      if (!same(i - 1) && !same(i + 1) && !same(i - LW) && !same(i + LW)) pepper++;
    }
  }

  const stat = (r) => {
    const L = lvl.get(r), I = laid.get(r);
    return {
      tone: L ? (100 * L[0] / L[1]) / RANGE : NaN,
      n: L ? L[1] : 0,
      ink: I ? (100 * I[0] / I[1]) : NaN,
      ideal: 100 * IDEAL_INK[r],
    };
  };
  return {
    name, pxErr, tone: (100 * toneSum / NPX) / RANGE,
    dark: stat(30), light: stat(80), black: stat(15),
    drop: blackTotal ? (100 * dropped) / blackTotal : 0,
    pepper: (1000 * pepper) / NPX,
  };
}

function show(m) {
  const f = (v, w = 5) => (Number.isNaN(v) ? '  n/a' : v.toFixed(2).padStart(w));
  console.log('    ' + m.name.padEnd(22) +
    ' pxErr ' + f(m.pxErr, 6) + '/195' +
    '  toneErr ' + f(m.tone) + '%' +
    '  [dark ' + f(m.dark.tone) + '% ink ' + f(m.dark.ink, 5) + '/' + m.dark.ideal.toFixed(1) +
    '  light ' + f(m.light.tone) + '% ink ' + f(m.light.ink, 5) + '/' + m.light.ideal.toFixed(1) +
    '  black ' + f(m.black.tone) + '% ink ' + f(m.black.ink, 5) + ']' +
    '  black-lost ' + f(m.drop) + '%  pepper/1k ' + f(m.pepper, 5));
}

(async () => {
  const api = await createKoEngine();
  api._ko_init(464, 778);
  const buf = fs.readFileSync(book);
  const hp = api._malloc(buf.length);
  api.HEAPU8.set(buf, hp);
  if (api._ko_load_epub(hp, buf.length, '/book.epub') < 0) throw new Error('load failed');
  api._ko_set_text_aa(AA);
  if (api._ko_build_spine(spine) < 0) throw new Error('build failed');

  const names = ['blue-noise (shipped)', 'no dither: 50% rule', 'no dither: raw BW'];
  const agg = names.map((nm) => ({ name: nm, pxErr: 0, tone: 0, drop: 0, pepper: 0, n: 0,
    darkW: 0, darkN: 0, lightW: 0, lightN: 0, blackW: 0, blackN: 0,
    darkInk: 0, lightInk: 0, blackInk: 0 }));

  for (const pg of pages) {
    if (api._ko_render_page(pg) !== 0) { console.log('  page ' + pg + ': render failed'); continue; }
    const readComp = (mono) => {
      if (api._ko_compose_rgba(mono) !== 0) throw new Error('compose failed');
      const rgba = new Uint32Array(api.HEAPU8.buffer, api._ko_rgba_ptr(), NPX);
      const out = new Uint8Array(NPX);
      for (let i = 0; i < NPX; i++) out[i] = (rgba[i] === 0xFFFFFFFF) ? 255 : (rgba[i] & 0xFF);
      return out;
    };
    const Dp = readComp(1), Rp = readComp(0);
    const N1p = new Uint8Array(NPX), N2p = new Uint8Array(NPX);
    for (let i = 0; i < NPX; i++) {
      N1p[i] = (Rp[i] === 128 || Rp[i] === 0) ? 0 : 255;   // ink the dark greys + black
      N2p[i] = Rp[i] !== 255 ? 0 : 255;
    }
    const refR = new Float32Array(NPX);
    for (let i = 0; i < NPX; i++) refR[i] = reflect(Rp[i]);
    const asR = (p) => { const a = new Float32Array(NPX); for (let i = 0; i < NPX; i++) a[i] = reflectMono(p[i]); return a; };

    let ink = 0, grey = 0;
    for (let i = 0; i < NPX; i++) { if (Rp[i] !== 255) ink++; if (Rp[i] === 128 || Rp[i] === 205) grey++; }
    console.log(`  page ${pg}:  reference ink ${(100 * ink / NPX).toFixed(2)}%  of which greys ` +
                `${(100 * grey / NPX).toFixed(2)}%   (reflectance: white 210, dark 30, light 80, black 15)`);
    [Dp, N1p, N2p].forEach((cand, k) => {
      const m = score(refR, asR(cand), cand, names[k]);
      show(m);
      const a = agg[k];
      a.pxErr += m.pxErr; a.tone += m.tone; a.drop += m.drop; a.pepper += m.pepper; a.n++;
      ['dark', 'light', 'black'].forEach((kk) => {
        if (Number.isNaN(m[kk].tone)) return;
        a[kk + 'W'] += m[kk].tone * m[kk].n; a[kk + 'N'] += m[kk].n;
        a[kk + 'Ink'] += m[kk].ink * m[kk].n;
      });
    });
  }

  if (agg[0].n > 1) {
    console.log(`  ---- mean over ${agg[0].n} pages ----`);
    agg.forEach((a) => {
      const mk = (kk) => ({ tone: a[kk + 'N'] ? a[kk + 'W'] / a[kk + 'N'] : NaN, n: a[kk + 'N'],
                            ink: a[kk + 'N'] ? a[kk + 'Ink'] / a[kk + 'N'] : NaN,
                            ideal: 100 * IDEAL_INK[{ dark: 30, light: 80, black: 15 }[kk]] });
      show({ name: a.name, pxErr: a.pxErr / a.n, tone: a.tone / a.n, dark: mk('dark'),
             light: mk('light'), black: mk('black'), drop: a.drop / a.n, pepper: a.pepper / a.n });
    });
  }
})();
