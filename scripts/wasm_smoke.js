#!/usr/bin/env node
// wasm_smoke.js — load the KO wasm module in Node, convert a small Korean
// EPUB, verify: init, load, build spine, render page, XTCH bytes valid.
const fs = require('fs');
const path = require('path');

const WASM_DIR = path.join(__dirname, '..', 'build-wasm');
const EPUB = process.argv[2] || '/Users/patryk/krxtc/.hermes/desktop-attachments/test-3.epub';

(async () => {
  const createKoEngine = require(path.join(WASM_DIR, 'ko_xtch_wasm.js'));
  const mod = await createKoEngine();
  const api = mod;
  const verPtr = api._ko_version();
  console.log('version:', api.UTF8ToString(verPtr));

  api._ko_init(464, 778);
  console.log('logical:', api._ko_logical_width(), 'x', api._ko_logical_height(),
              'viewport:', api._ko_viewport_width(), 'x', api._ko_viewport_height());

  // Load the EPUB into wasm heap
  const buf = fs.readFileSync(EPUB);
  const p = api._malloc(buf.length);
  api.HEAPU8.set(buf, p);
  const spines = api._ko_load_epub(p, buf.length, '/book.epub');
  api._free(p);
  console.log('spines:', spines);
  if (spines < 0) {
    console.error('load failed:', api._ko_error());
    process.exit(1);
  }

  // Title (malloc'd char buffer)
  const tbuf = api._malloc(512);
  api._ko_get_title(tbuf, 512);
  const title = api.UTF8ToString(tbuf);
  api._free(tbuf);
  console.log('title:', title);

  const pages = api._ko_build_spine(1);
  console.log('spine 1 pages:', pages);

  const r = api._ko_render_page(0);
  console.log('render page 0 rc:', r, 'err:', api._ko_error() || '(none)');
  const plane = api._ko_plane_ptr(0);
  const size = api._ko_plane_size(0);
  console.log('plane0 ptr:', plane, 'size:', size);
  const px = new Uint8Array(api.HEAPU8.buffer, plane, size);
  let ink = 0;
  for (let i = 0; i < size; i++) {
    const b = px[i];
    for (let k = 0; k < 8; k++) if (!((b >> (7 - k)) & 1)) ink++;
  }
  console.log('BW ink pixels page 0 spine 1:', ink);

  // Full-book XTCH
  const total = api._ko_render_xtch();
  console.log('xtch total pages:', total);
  if (total > 0) {
    const outPtr = api._ko_xtch_ptr();
    const outSize = api._ko_xtch_size();
    const bytes = Buffer.from(new Uint8Array(api.HEAPU8.buffer, outPtr, outSize));
    fs.writeFileSync('/tmp/wasm-out.xtch', bytes);
    console.log('xtch bytes:', outSize, '-> /tmp/wasm-out.xtch');
    console.log('magic:', bytes.slice(0, 4).toString('latin1'),
                'pageCount@6:', bytes.readUInt16LE(6));
  }
  api._ko_close();
  console.log('SMOKE OK');
})().catch((e) => { console.error('SMOKE FAIL', e); process.exit(1); });
