#!/usr/bin/env node
/*
 * epdfont_parity.js - does the browser-side converter produce the SAME .epdfont as the
 * local Python tool?
 *
 * Converts one font both ways with identical parameters (the ones server.py would use)
 * and compares: file hash, header, intervals, every glyph record, every bitmap byte.
 * Any difference is reported with its offset and the first glyph that diverges, so a
 * failure is diagnosable rather than just red.
 *
 * usage: epdfont_parity.js <font> [size] [weight] [--1bit] [--noHangul]
 */
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execFileSync } = require('child_process');

const ROOT = path.join(__dirname, '..', '..');
const PY = '/usr/bin/python3';
const CONVERTER = path.join(ROOT, 'tools', 'ttf_to_epdfont_fast.py');
const EPUBFONT = require(path.join(ROOT, 'web', 'epdfont.js'));
const createFt = require(path.join(ROOT, 'web', 'ft_wasm.js'));

const fontPath = process.argv[2];
const size = parseInt(process.argv[3] || '14', 10);
const weight = parseInt(process.argv[4] || '500', 10);
const twoBit = !process.argv.includes('--1bit');
const noHangul = process.argv.includes('--noHangul');
const name = 'custom';

if (!fontPath) {
  console.error('usage: epdfont_parity.js <font> [size] [weight] [--1bit] [--noHangul]');
  process.exit(2);
}

const sha = (b) => crypto.createHash('sha256').update(b).digest('hex');

function parse(epd) {
  const dv = new DataView(epd.buffer, epd.byteOffset, epd.byteLength);
  const h = {
    magic: dv.getUint32(0, true), version: dv.getUint16(4, true),
    is2Bit: epd[6], advanceY: epd[8], ascender: epd[9], descender: epd[10],
    intervalCount: dv.getUint32(12, true), glyphCount: dv.getUint32(16, true),
    intervalsOffset: dv.getUint32(20, true), glyphsOffset: dv.getUint32(24, true),
    bitmapOffset: dv.getUint32(28, true),
  };
  const intervals = [];
  for (let i = 0; i < h.intervalCount; i++) {
    const o = h.intervalsOffset + i * 12;
    intervals.push([dv.getUint32(o, true), dv.getUint32(o + 4, true), dv.getUint32(o + 8, true)]);
  }
  const glyphs = [];
  for (let i = 0; i < h.glyphCount; i++) {
    const o = h.glyphsOffset + i * 16;
    glyphs.push({
      width: epd[o], height: epd[o + 1], advanceX: epd[o + 2],
      left: dv.getInt16(o + 4, true), top: dv.getInt16(o + 6, true),
      length: dv.getUint32(o + 8, true), offset: dv.getUint32(o + 12, true),
      bytes: epd.subarray(h.bitmapOffset + dv.getUint32(o + 12, true), h.bitmapOffset + dv.getUint32(o + 12, true) + dv.getUint32(o + 8, true)),
    });
  }
  return { h, intervals, glyphs };
}

(async () => {
  const fontBytes = fs.readFileSync(fontPath);
  const conv = await EPUBFONT.load({
    factory: createFt({ locateFile: (f) => path.join(ROOT, 'web', f) }),
  });

  // --- browser path (what the page will do) ---
  const t0 = Date.now();
  const mine = conv.convert({
    fontBytes, name, size, twoBit, noHangul,
    extraIntervals: noHangul ? [] : [],
    weight,
  });
  const mineMs = Date.now() - t0;

  // --- python path (the server's own invocation) ---
  const tmpDir = fs.mkdtempSync('/tmp/epdparity-');
  const outPy = path.join(tmpDir, 'py.epdfont');
  const args = [CONVERTER, name, String(size), fontPath];
  if (twoBit) args.push('--2bit');
  if (!noHangul) args.push('--additional-intervals', '0xAC00,0xD7AF');
  if (mine.emboldenPx64 > 0) args.push('--embolden', String(mine.emboldenPx64));
  args.push('-o', outPy);
  const t1 = Date.now();
  execFileSync(PY, args, { env: { PATH: '/opt/homebrew/bin:/usr/bin:/bin', HOME: process.env.HOME }, stdio: 'pipe' });
  const pyMs = Date.now() - t1;
  const py = fs.readFileSync(outPy);

  const a = parse(mine.bytes), b = parse(py);
  console.log(`font      : ${path.basename(fontPath)}  size ${size}  weight ${weight}  ${twoBit ? '2-bit' : '1-bit'}${noHangul ? '  (noHangul)' : ''}`);
  console.log(`weight use: ${mine.weightMode}${mine.nativeWeight !== null ? ' (native OS/2 ' + mine.nativeWeight + ')' : ''}` +
              `${mine.emboldenPx64 ? ', embolden ' + mine.emboldenPx64 + '/64px' : ''}${mine.effectiveWeight !== weight ? ', effective ' + mine.effectiveWeight : ''}`);
  console.log(`size      : wasm ${mine.bytes.length} B in ${mineMs} ms   |   python ${py.length} B in ${pyMs} ms`);
  console.log(`sha256    : wasm ${sha(mine.bytes).slice(0, 24)}`);
  console.log(`            py   ${sha(py).slice(0, 24)}`);

  const same = mine.bytes.length === py.length && Buffer.compare(Buffer.from(mine.bytes), py) === 0;
  console.log(`verdict   : ${same ? 'IDENTICAL' : 'DIFFERS'}`);
  if (same) return;

  // diagnose
  const hk = ['magic', 'version', 'is2Bit', 'advanceY', 'ascender', 'descender', 'intervalCount', 'glyphCount', 'intervalsOffset', 'glyphsOffset', 'bitmapOffset'];
  hk.forEach((k) => { if (a.h[k] !== b.h[k]) console.log(`  header.${k}: wasm ${a.h[k]} vs py ${b.h[k]}`); });
  if (a.intervals.length !== b.intervals.length) {
    console.log(`  intervals: ${a.intervals.length} vs ${b.intervals.length}`);
  } else {
    for (let i = 0; i < a.intervals.length; i++) {
      if (a.intervals[i].join() !== b.intervals[i].join()) {
        console.log(`  interval[${i}]: wasm ${a.intervals[i]} vs py ${b.intervals[i]}`);
      }
    }
  }
  if (a.glyphs.length !== b.glyphs.length) console.log(`  glyphs: ${a.glyphs.length} vs ${b.glyphs.length}`);
  const n = Math.min(a.glyphs.length, b.glyphs.length);
  let metricDiff = 0, bitmapDiff = 0, firstBad = -1;
  for (let i = 0; i < n; i++) {
    const ga = a.glyphs[i], gb = b.glyphs[i];
    if (ga.width !== gb.width || ga.height !== gb.height || ga.advanceX !== gb.advanceX ||
        ga.left !== gb.left || ga.top !== gb.top) {
      if (metricDiff++ < 5) console.log(`  glyph[${i}] metrics: wasm ${ga.width}x${ga.height} adv ${ga.advanceX} l ${ga.left} t ${ga.top}` +
                                        ` vs py ${gb.width}x${gb.height} adv ${gb.advanceX} l ${gb.left} t ${gb.top}`);
    }
    if (Buffer.compare(Buffer.from(ga.bytes), Buffer.from(gb.bytes)) !== 0) {
      bitmapDiff++;
      if (firstBad < 0) firstBad = i;
    }
  }
  console.log(`  glyphs differing: metrics ${metricDiff}, bitmaps ${bitmapDiff}${firstBad >= 0 ? ` (first bitmap diff: glyph ${firstBad})` : ''}`);
})().catch((e) => { console.error('ERROR:', e.message); process.exit(1); });
