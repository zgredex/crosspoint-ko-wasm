#!/usr/bin/env node
// mono_preview_vs_file.js - verify the invariant the site promises: the 1-bit
// preview is byte-for-byte the page the exported XTG file carries.
//
// The 1-bit preview used to be the raw BW plane. Once the text-AA switch began
// driving the blue-noise dither, the file started carrying halftoned greys, so the
// preview had to apply the same mask decision (engine-side, ko_compose_rgba(1)) or
// the promise "plays back on the device exactly like the preview" breaks. This
// checks it in both switch positions.
//
// Usage: node scripts/verify/mono_preview_vs_file.js [book.epub] [spine] [page]
const fs = require('fs');
const path = require('path');

const WASM_DIR = path.join(__dirname, '..', '..', 'build-wasm');
const EPUB = process.argv[2] || '/Users/patryk/krxtc/ko-wasm/dist/demo-images.epub';
const SPINE = Number(process.argv[3] || 1);
const PAGE = Number(process.argv[4] || 0);
const LW = 480, LH = 800, PLANE = 48000, REC = 22 + PLANE;

const createKoEngine = require(path.join(WASM_DIR, 'ko_xtch_wasm.js'));

// XTG plane bit for logical (x, y): byte y*60 + (x>>3), bit 7-(x&7); bit 0 = ink.
function xtgInk(plane, x, y) {
  return ((plane[y * 60 + (x >> 3)] >> (7 - (x & 7))) & 1) === 0;
}

function xtgPages(buf) {
  const out = [];
  let p = 0;
  for (;;) {
    const q = buf.indexOf('XTG\0', p, 'latin1');
    if (q < 0) return out;
    out.push(buf.subarray(q + 22, q + REC));
    p = q + 22;
  }
}

(async () => {
  const api = await createKoEngine();
  api._ko_init(464, 764); // advisory: geometry is derived from the spec margins (14/8/22/8)
  const buf = fs.readFileSync(EPUB);
  const p = api._malloc(buf.length);
  api.HEAPU8.set(buf, p);
  if (api._ko_load_epub(p, buf.length, '/book.epub') < 0) {
    throw new Error('load failed: ' + api.UTF8ToString(api._ko_error()));
  }

  let failures = 0;
  for (const aa of [1, 0]) {
    api._ko_set_text_aa(aa);

    // Export FIRST: the XTG page order is whatever the exporter produced, so take the
    // per-spine page counts from it rather than assuming ko_build_spine agrees (a spine
    // whose pages fail to render is skipped and shifts every later page).
    api._ko_export_set_mode(0);
    const spines = api._ko_export_begin();
    const added = [];
    for (let s = 0; s < spines; s++) added.push(api._ko_export_spine(s));
    // ko_export_finish() returns the total page count, or -1 on failure.
    if (api._ko_export_finish() < 0) throw new Error('export finish failed');
    const file = Buffer.from(api.HEAPU8.subarray(api._ko_xtch_ptr(), api._ko_xtch_ptr() + api._ko_xtch_size()));
    const pages = xtgPages(file);

    const exportPageCount = added[SPINE] !== undefined ? added[SPINE] : 0;
    const page = Math.max(0, Math.min(PAGE, exportPageCount - 1));
    const target = added.slice(0, SPINE).reduce((a, b) => a + b, 0) + page;
    if (target >= pages.length) throw new Error(`page ${target} not in file (${pages.length} pages)`);

    // Now the preview for that same spine/page.
    const nPages = api._ko_build_spine(SPINE);
    if (nPages < 0) throw new Error('build failed: ' + api.UTF8ToString(api._ko_error()));
    if (api._ko_render_page(page) !== 0) {
      throw new Error('render failed: ' + api.UTF8ToString(api._ko_error()));
    }
    // preview: the RGBA the worker blits (black == ink). Copy it out NOW: a later
    // engine call can grow wasm memory, which detaches any view into the old heap
    // (the worker copies the frame for the same reason).
    if (api._ko_compose_rgba(1) !== 0) throw new Error('compose failed');
    const previewInk = new Uint8Array(LW * LH);
    {
      const rgba = new Uint32Array(api.HEAPU8.buffer, api._ko_rgba_ptr(), LW * LH);
      for (let i = 0; i < LW * LH; i++) previewInk[i] = rgba[i] === 0xff000000 ? 1 : 0;
    }

    const plane = pages[target];
    let mismatch = 0, inkFile = 0, inkPreview = 0;
    for (let y = 0; y < LH; y++) {
      for (let x = 0; x < LW; x++) {
        const previewIsInk = previewInk[y * LW + x] === 1;
        const fileInk = xtgInk(plane, x, y);
        if (previewIsInk) inkPreview++;
        if (fileInk) inkFile++;
        if (previewIsInk !== fileInk && mismatch < 3) {
          console.log(`  mismatch at x=${x} y=${y}: previewInk=${previewIsInk} fileInk=${fileInk}`);
        }
        if (previewIsInk !== fileInk) mismatch++;
      }
    }
    const ok = mismatch === 0;
    if (!ok) failures++;
    console.log(`AA=${aa} (1-bit): preview ink ${inkPreview} px, file ink ${inkFile} px, `
      + `mismatches ${mismatch}/${LW * LH} -> ${ok ? 'MATCH' : 'MISMATCH'}`);
  }
  console.log(failures === 0 ? 'MONO PREVIEW == FILE: OK' : 'MONO PREVIEW == FILE: FAILED');
  process.exit(failures === 0 ? 0 : 1);
})();
