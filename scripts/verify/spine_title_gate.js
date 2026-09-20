#!/usr/bin/env node
// Chapter labels must come from the visible book, not internal filenames or
// generic EPUB navigation placeholders. The adversarial fixture says
// "Section N" in nav.xhtml while its rendered XHTML has Korean names; one real
// heading is absent from navigation entirely.

const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..', '..');
const createKoEngine = require(path.join(ROOT, 'build-wasm', 'ko_xtch_wasm.js'));
const KOPUB_ID = -1446433084;

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
  check(title(0) === 'PNG Image Tests', 'spine 1 uses its visible XHTML heading');
  check(title(1) === 'PNG Format Test', 'spine 2 uses its visible XHTML heading');
  check(title(9) === 'Image Centering Bleed Test', 'last spine uses its visible XHTML heading');
  check(title(-1) === '', 'negative spine index is rejected');
  check(title(spines) === '', 'past-end spine index is rejected');

  const chapterBytes = fs.readFileSync(path.join(ROOT, 'oracle', 'fixtures', 'ko-chapters.epub'));
  const chapterPtr = api._malloc(chapterBytes.length);
  api.HEAPU8.set(chapterBytes, chapterPtr);
  const chapterSpines = api._ko_load_epub(chapterPtr, chapterBytes.length, '/chapters.epub');
  api._free(chapterPtr);
  check(chapterSpines === 2, 'adversarial chapter fixture loads two spines');
  check(title(0) === '제1부 실제 첫 장', 'picker skips hidden/generic headings for the real Korean title');
  check(title(1) === 'XTCKO chapter parsing', 'picker uses the visible outer title on the omnibus spine');

  // Production registers this same externalized face in the worker. Load it
  // here so pagination—and therefore the omitted-heading page boundary—is a
  // real Korean-fork layout rather than a missing-font blank-page artifact.
  const face = fs.readFileSync(path.join(ROOT, 'web', 'kopub_14.epd2'));
  const facePtr = api._malloc(face.length);
  api.HEAPU8.set(face, facePtr);
  check(api._ko_load_external_builtin_font(KOPUB_ID, facePtr, face.length) === 0,
        'production KoPub face is registered');
  api._free(facePtr);
  check(api._ko_set_font(KOPUB_ID) === 0, 'production KoPub face is selected');

  check(api._ko_export_set_mode(1) === 0, '2-bit chapter export mode is accepted');
  check(api._ko_export_begin() === chapterSpines, 'chapter export begins');
  for (let spine = 0; spine < chapterSpines; spine++) {
    check(api._ko_export_spine(spine) > 0, `chapter spine ${spine + 1} renders`);
  }
  check(api._ko_export_finish() > 0, 'chapter export finalizes');
  const out = new Uint8Array(api.HEAPU8.buffer, api._ko_xtch_ptr(), api._ko_xtch_size());
  const view = new DataView(out.buffer, out.byteOffset, out.byteLength);
  const chapterCount = view.getUint16(56 + 0xf6, true);
  const decode = new TextDecoder('utf-8', { fatal: true });
  const names = [];
  for (let i = 0; i < chapterCount; i++) {
    const field = out.subarray(312 + i * 96, 312 + i * 96 + 80);
    const nul = field.indexOf(0);
    names.push(decode.decode(nul >= 0 ? field.subarray(0, nul) : field));
  }
  const expected = ['제1부 실제 첫 장', '제2장 깊은 제목', '제3장 목차에 없는 이름', '제4장 마지막 실제 이름'];
  check(JSON.stringify(names) === JSON.stringify(expected),
        `exported chapters are parsed from visible XHTML (${names.join(' / ')})`);
  check(!names.some((name) => /^Section\s+\d+$/i.test(name)), 'no generic navigation label reaches the XTC');

  api._ko_close();
  console.log('chapter-title gate OK');
})().catch((error) => {
  console.error('chapter-title gate FAILED:', error && error.stack || error);
  process.exit(1);
});
