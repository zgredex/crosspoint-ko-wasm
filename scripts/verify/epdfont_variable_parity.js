#!/usr/bin/env node
/*
 * epdfont_variable_parity.js - variable-font (wght axis) conversion, browser vs server.
 *
 * This is the ONE path where the two cannot be identical by construction:
 *   server  : fontTools `instantiateVariableFont(tt, {"wght": W})` bakes a NEW static font
 *             (outlines re-quantised to integer coordinates), then FreeType rasterises that
 *   browser : FreeType interpolates the axis itself at load time
 *             (FT_Set_Var_Design_Coordinates) before rasterising
 * Same design coordinates, two different implementations of "at this weight" - so expect
 * close-but-not-equal glyphs. This script measures HOW close, per glyph, so the claim in
 * the docs is a number rather than a hope.
 *
 * usage: epdfont_variable_parity.js <variable font> [size] [weight]
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
const weight = parseInt(process.argv[4] || '700', 10);
const name = 'custom';
if (!fontPath) { console.error('usage: epdfont_variable_parity.js <variable font> [size] [weight]'); process.exit(2); }

const sha = (b) => crypto.createHash('sha256').update(b).digest('hex');

function parse(epd) {
  const dv = new DataView(epd.buffer, epd.byteOffset, epd.byteLength);
  const h = { glyphCount: dv.getUint32(16, true), intervalCount: dv.getUint32(12, true), bitmapOffset: dv.getUint32(28, true) };
  const glyphs = [];
  for (let i = 0; i < h.glyphCount; i++) {
    const o = 32 + h.intervalCount * 12 + i * 16;
    const len = dv.getUint32(o + 8, true), off = dv.getUint32(o + 12, true);
    glyphs.push({
      width: epd[o], height: epd[o + 1], advanceX: epd[o + 2],
      left: dv.getInt16(o + 4, true), top: dv.getInt16(o + 6, true),
      bytes: epd.subarray(h.bitmapOffset + off, h.bitmapOffset + off + len),
    });
  }
  return glyphs;
}

(async () => {
  const fontBytes = fs.readFileSync(fontPath);
  const conv = await EPUBFONT.load({ factory: createFt({ locateFile: (f) => path.join(ROOT, 'web', f) }) });

  const t0 = Date.now();
  const mine = conv.convert({ fontBytes, name, size, twoBit: true, weight });
  const mineMs = Date.now() - t0;
  if (mine.weightMode !== 'wght-instance') {
    console.log(`note: this font has no wght axis (mode ${mine.weightMode}) - it is a STATIC comparison,`);
    console.log('      see epdfont_parity.js for those (they are byte-identical).');
  }

  const tmp = fs.mkdtempSync('/tmp/epdvar-');
  const instanced = path.join(tmp, 'inst.ttf');
  const outPy = path.join(tmp, 'py.epdfont');
  const pyScript = `
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
tt = TTFont(${JSON.stringify(fontPath)})
instantiateVariableFont(tt, {"wght": ${weight}}, inplace=True)
tt.save(${JSON.stringify(instanced)})
`;
  const t1 = Date.now();
  execFileSync(PY, ['-c', pyScript], { env: { PATH: '/opt/homebrew/bin:/usr/bin:/bin', HOME: process.env.HOME } });
  const args = [CONVERTER, name, String(size), instanced, '--2bit', '--additional-intervals', '0xAC00,0xD7AF', '-o', outPy];
  execFileSync(PY, args, { env: { PATH: '/opt/homebrew/bin:/usr/bin:/bin', HOME: process.env.HOME }, stdio: 'pipe' });
  const pyMs = Date.now() - t1;
  const py = fs.readFileSync(outPy);

  console.log(`font        : ${path.basename(fontPath)}  size ${size}  requested weight ${weight}`);
  console.log(`browser     : ${mine.bytes.length} B in ${mineMs} ms (${mine.weightMode}${mine.effectiveWeight !== weight ? ', effective ' + mine.effectiveWeight : ''})`);
  console.log(`server+ft   : ${py.length} B in ${pyMs} ms (fontTools instance @${weight}, then the converter)`);
  console.log(`sha256      : browser ${sha(mine.bytes).slice(0, 24)}`);
  console.log(`              server  ${sha(py).slice(0, 24)}`);

  const a = parse(mine.bytes), b = parse(py);
  console.log(`glyphs      : browser ${a.length} | server ${b.length}`);
  const n = Math.min(a.length, b.length);
  let metricDiff = 0, bitmapDiff = 0, byteDiff = 0, totalBytes = 0;
  let maxAdvDelta = 0, maxLeftDelta = 0, maxLevelDelta = 0;
  const advDeltas = new Map();
  for (let i = 0; i < n; i++) {
    const ga = a[i], gb = b[i];
    if (ga.width !== gb.width || ga.height !== gb.height || ga.advanceX !== gb.advanceX ||
        ga.left !== gb.left || ga.top !== gb.top) metricDiff++;
    const da = Math.abs(ga.advanceX - gb.advanceX);
    maxAdvDelta = Math.max(maxAdvDelta, da);
    advDeltas.set(da, (advDeltas.get(da) || 0) + 1);
    maxLeftDelta = Math.max(maxLeftDelta, Math.abs(ga.left - gb.left), Math.abs(ga.top - gb.top));
    totalBytes += Math.max(ga.bytes.length, gb.bytes.length);
    if (Buffer.compare(Buffer.from(ga.bytes), Buffer.from(gb.bytes)) !== 0) {
      bitmapDiff++;
      const m = Math.min(ga.bytes.length, gb.bytes.length);
      for (let k = 0; k < m; k++) {
        if (ga.bytes[k] !== gb.bytes[k]) {
          byteDiff++;
          // how far apart are the 2-bit levels inside the differing byte? (0..3 per pixel)
          let x = ga.bytes[k] ^ gb.bytes[k];
          for (let sh = 0; sh < 8; sh += 2) {
            const d = Math.abs(((ga.bytes[k] >> sh) & 3) - ((gb.bytes[k] >> sh) & 3));
            if (d > maxLevelDelta) maxLevelDelta = d;
          }
        }
      }
    }
  }
  const pct = (x) => (100 * x / n).toFixed(2) + '%';
  console.log(`glyph match : ${n - Math.max(metricDiff, bitmapDiff)}/${n} (${pct(n - Math.max(metricDiff, bitmapDiff))}) identical`);
  console.log(`              metrics differ on ${metricDiff} (${pct(metricDiff)}), bitmaps on ${bitmapDiff} (${pct(bitmapDiff)})`);
  if (totalBytes) console.log(`bitmap bytes: ${byteDiff} differing of ${totalBytes} (${(100 * byteDiff / totalBytes).toFixed(3)}%)`);
  const advHist = Array.from(advDeltas.entries()).sort((x, y) => x[0] - y[0])
    .map(([d, c]) => `${d}px×${c}`).join('  ');
  console.log(`residual    : max advance delta ${maxAdvDelta} px, max bearing delta ${maxLeftDelta} px,`);
  console.log(`              max 2-bit level delta ${maxLevelDelta}/3  |  advance deltas: ${advHist}`);
})().catch((e) => { console.error('ERROR:', e.message); process.exit(1); });
