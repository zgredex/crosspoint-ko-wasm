#!/usr/bin/env node
// Prove the device-profile + orientation contract at the WASM boundary:
//
//   * the renderer lays text out in the Korean fork's selected logical screen
//     (X4: 480x800/800x480; X3: 528x792/792x528),
//   * the preview is upright in that logical orientation, and
//   * the XTC/XTCH record stays in the selected device's portrait geometry,
//     which the fork's XtcReaderActivity reads from each page header.
//
// Every preview pixel is compared with the corresponding pixel decoded from
// the exported record, for all four firmware orientations, in both 1-bit XTG
// and 2-bit XTH modes. The UI-facing requirement is the two landscape holding
// directions; inverted portrait is included because the API preserves all four.
//
// Usage: node scripts/verify/orientation_preview_vs_file.js [book.epub] [spine] [page]
const fs = require('fs');
const path = require('path');

const WASM_DIR = path.join(__dirname, '..', '..', 'build-wasm');
const EPUB = process.argv[2] || path.join(__dirname, '..', '..', 'dist', 'demo-images.epub');
const SPINE = Number(process.argv[3] || 1);
const PAGE = Number(process.argv[4] || 0);
const PALETTE = [0xffffffff, 0xff4e4e4e, 0xff9c9c9c, 0xff000000];
const KOPUB_ID = -1446433084;
const DEVICES = [
  {name: 'x4', code: 4, physicalW: 800, physicalH: 480, fileW: 480, fileH: 800},
  {name: 'x3', code: 3, physicalW: 792, physicalH: 528, fileW: 528, fileH: 792},
];

const createKoEngine = require(path.join(WASM_DIR, 'ko_xtch_wasm.js'));

function u16(buf, off) { return buf[off] | (buf[off + 1] << 8); }
function u32(buf, off) {
  return (buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16)
          | (buf[off + 3] << 24)) >>> 0;
}
function u64Safe(buf, off) {
  const low = u32(buf, off);
  const high = u32(buf, off + 4);
  const value = high * 0x100000000 + low;
  if (!Number.isSafeInteger(value)) throw new Error('container offset exceeds JS safe integer');
  return value;
}

function recordsFromContainer(buf, device) {
  if (buf.length < 56) throw new Error('container is shorter than its header');
  const count = u16(buf, 6);
  const index = u64Safe(buf, 0x18);
  const records = [];
  for (let i = 0; i < count; i++) {
    const entry = index + i * 16;
    const off = u64Safe(buf, entry);
    const size = u32(buf, entry + 8);
    const width = u16(buf, entry + 12);
    const height = u16(buf, entry + 14);
    if (width !== device.fileW || height !== device.fileH) {
      throw new Error(`page ${i}: file geometry ${width}x${height}, expected `
                      + `${device.fileW}x${device.fileH}`);
    }
    if (size < 22 || off < 0 || off + size > buf.length) {
      throw new Error(`page ${i}: invalid record offset=${off} size=${size}`);
    }
    records.push(buf.subarray(off, off + size));
  }
  return records;
}

// The writer converts physical panel coordinates to the selected profile's
// portrait record as fileX=physicalH-1-phyY, fileY=phyX. These are the Korean
// fork's orientation transforms composed with that writer transform.
function logicalToFile(device, orientation, x, y) {
  let phyX;
  let phyY;
  switch (orientation) {
    case 1: phyX = device.physicalW - 1 - x; phyY = device.physicalH - 1 - y; break;
    case 2: phyX = device.physicalW - 1 - y; phyY = x; break;
    case 3: phyX = x; phyY = y; break;
    default: phyX = y; phyY = device.physicalH - 1 - x; break;
  }
  return [device.physicalH - 1 - phyY, phyX];
}

function xtgPixel(record, device, x, y) {
  if (record.subarray(0, 4).toString('latin1') !== 'XTG\0') {
    throw new Error(`expected XTG record, got ${record.subarray(0, 4).toString('hex')}`);
  }
  const byte = record[22 + y * (device.fileW >> 3) + (x >> 3)];
  const white = (byte >> (7 - (x & 7))) & 1;
  return white ? PALETTE[0] : PALETTE[3];
}

function xthPixel(record, device, x, y) {
  if (record.subarray(0, 4).toString('latin1') !== 'XTH\0') {
    throw new Error(`expected XTH record, got ${record.subarray(0, 4).toString('hex')}`);
  }
  // XTH columns are stored right-to-left, with fileH/8 vertical bytes per column.
  const columnBytes = device.fileH >> 3;
  const planeBytes = device.fileW * columnBytes;
  const index = (device.fileW - 1 - x) * columnBytes + (y >> 3);
  const shift = 7 - (y & 7);
  const p1 = (record[22 + index] >> shift) & 1;
  const p2 = (record[22 + planeBytes + index] >> shift) & 1;
  return PALETTE[(p1 << 1) | p2];
}

function engineError(api) {
  const p = api._ko_error();
  return p ? api.UTF8ToString(p) : 'unknown engine error';
}

(async () => {
  const api = await createKoEngine();
  api._ko_init(464, 764);
  const epub = fs.readFileSync(EPUB);
  const p = api._malloc(epub.length);
  api.HEAPU8.set(epub, p);
  const spineCount = api._ko_load_epub(p, epub.length, '/book.epub');
  api._free(p);
  if (spineCount < 0) throw new Error('load failed: ' + engineError(api));
  // Production fetches this face in the worker. Verification loads the same
  // shipped EPD2 explicitly so the page contains real Korean-fork text rather
  // than exercising rotation over a missing-font blank.
  if (!api._ko_has_font(KOPUB_ID)) {
    const face = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'kopub_14.epd2'));
    const facePtr = api._malloc(face.length);
    api.HEAPU8.set(face, facePtr);
    const rc = api._ko_load_external_builtin_font(KOPUB_ID, facePtr, face.length);
    api._free(facePtr);
    if (rc !== 0) throw new Error('KoPub load failed: ' + engineError(api));
  }
  if (api._ko_set_font(KOPUB_ID) !== 0) throw new Error('KoPub select failed: ' + engineError(api));
  if (SPINE < 0 || SPINE >= spineCount) {
    throw new Error(`spine ${SPINE} is outside 0..${spineCount - 1}`);
  }

  let failures = 0;
  for (const device of DEVICES) {
   if (api._ko_set_device_profile(device.code) !== 0) throw new Error(engineError(api));
   for (const orientation of [0, 1, 2, 3]) {
    if (api._ko_set_orientation(orientation) !== 0) throw new Error(engineError(api));
    if (api._ko_set_screen_margin(5) !== 0) throw new Error(engineError(api));
    const width = api._ko_logical_width();
    const height = api._ko_logical_height();
    const expected = orientation === 1 || orientation === 3
      ? [device.physicalW, device.physicalH] : [device.fileW, device.fileH];
    if (width !== expected[0] || height !== expected[1]) {
      throw new Error(`orientation ${orientation}: logical screen ${width}x${height}, `
                      + `expected ${expected[0]}x${expected[1]}`);
    }

    for (const mode of [0, 1]) {
      api._ko_set_image_tone_depth(mode === 0 ? 2 : 4);
      api._ko_export_set_mode(mode);
      const nSpines = api._ko_export_begin();
      if (nSpines !== spineCount) throw new Error(`export begin failed: ${engineError(api)}`);
      const added = [];
      for (let s = 0; s < nSpines; s++) {
        const n = api._ko_export_spine(s);
        if (n < 0) throw new Error(`export spine ${s} failed: ${engineError(api)}`);
        added.push(n);
      }
      if (api._ko_export_finish() < 0) throw new Error('export finish failed: ' + engineError(api));
      const outPtr = api._ko_xtch_ptr();
      const outSize = api._ko_xtch_size();
      const file = Buffer.from(api.HEAPU8.slice(outPtr, outPtr + outSize));
      const records = recordsFromContainer(file, device);

      const localPage = Math.max(0, Math.min(PAGE, added[SPINE] - 1));
      const globalPage = added.slice(0, SPINE).reduce((a, b) => a + b, 0) + localPage;
      if (added[SPINE] <= 0 || globalPage >= records.length) {
        throw new Error(`orientation ${orientation}, mode ${mode}: target page was not exported`);
      }

      const pages = api._ko_build_spine(SPINE);
      if (pages <= localPage) throw new Error(`preview build failed: ${engineError(api)}`);
      if (api._ko_render_page(localPage) !== 0) throw new Error('render failed: ' + engineError(api));
      if (api._ko_compose_rgba(mode === 0 ? 1 : 0) !== 0) {
        throw new Error('compose failed: ' + engineError(api));
      }
      const pixels = device.fileW * device.fileH;
      const rgba = new Uint32Array(api.HEAPU8.buffer, api._ko_rgba_ptr(), pixels);
      const record = records[globalPage];
      let mismatch = 0;
      let first = '';
      for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
          const [fileX, fileY] = logicalToFile(device, orientation, x, y);
          const fromFile = mode === 0
            ? xtgPixel(record, device, fileX, fileY)
            : xthPixel(record, device, fileX, fileY);
          const fromPreview = rgba[y * width + x] >>> 0;
          if (fromFile !== fromPreview) {
            mismatch++;
            if (!first) {
              first = ` first=(${x},${y}) preview=0x${fromPreview.toString(16)}`
                    + ` file=0x${fromFile.toString(16)}`;
            }
          }
        }
      }
      const label = `${device.name} ${orientation === 0 ? 'portrait' : orientation === 1 ? 'landscape-cw'
        : orientation === 2 ? 'portrait-inverted' : 'landscape-ccw'}`
                  + ` ${mode === 0 ? 'XTG' : 'XTH'}`;
      console.log(`${label}: ${width}x${height}, file ${device.fileW}x${device.fileH}, `
                  + `mismatches ${mismatch}/${pixels}`
                  + first);
      if (mismatch !== 0) failures++;
    }
   }
  }

  api._ko_close();
  if (failures) throw new Error(`${failures} orientation preview/file comparison(s) failed`);
  console.log('ORIENTATION PREVIEW == FILE: OK');
})().catch((e) => {
  console.error('ORIENTATION PREVIEW == FILE: FAILED:', e && e.stack ? e.stack : e);
  process.exit(1);
});
