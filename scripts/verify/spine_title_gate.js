#!/usr/bin/env node
// The chapter picker must use EPUB navigation titles, not internal filenames.
// demo-png.epub deliberately makes those differ: chapter1.xhtml is titled
// "Introduction", chapter2.xhtml is titled "1. PNG Format", and so on.

const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..', '..');
const createKoEngine = require(path.join(ROOT, 'build-wasm', 'ko_xtch_wasm.js'));

function check(ok, message) {
  if (!ok) throw new Error(message);
  console.log('PASS ' + message);
}

(async () => {
  const api = await createKoEngine();
  api._ko_init(464, 764);

  const bytes = fs.readFileSync(path.join(ROOT, 'web', 'demo-png.epub'));
  const ptr = api._malloc(bytes.length);
  api.HEAPU8.set(bytes, ptr);
  const spines = api._ko_load_epub(ptr, bytes.length, '/book.epub');
  api._free(ptr);

  check(spines === 10, 'fixture loads ten spines');
  check(typeof api._ko_get_spine_title === 'function', 'spine title API is exported');

  const title = (i) => api.UTF8ToString(api._ko_get_spine_title(i));
  check(title(0) === 'Introduction', 'spine 1 uses the EPUB navigation title');
  check(title(1) === '1. PNG Format', 'spine 2 preserves the book title verbatim');
  check(title(9) === '9. Alignment Bleed', 'last spine maps to its navigation title');
  check(title(-1) === '', 'negative spine index is rejected');
  check(title(spines) === '', 'past-end spine index is rejected');

  api._ko_close();
  console.log('chapter-title gate OK');
})().catch((error) => {
  console.error('chapter-title gate FAILED:', error && error.stack || error);
  process.exit(1);
});
