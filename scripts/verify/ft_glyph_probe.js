// Stage 1: one glyph, wasm vs the Python tool's own calls.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const createFt = require(path.join(__dirname, '..', '..', 'web', 'ft_wasm.js'));
const FT_DIR = path.join(__dirname, '..', '..', 'web');

(async () => {
  const ft = await createFt({ locateFile: (f) => path.join(FT_DIR, f) });
  console.log('module:', ft.UTF8ToString(ft._ftw_version()));

  const fontPath = process.argv[2] || '/System/Library/Fonts/Supplemental/Arial.ttf';
  const cp = process.argv[3] ? parseInt(process.argv[3], 16) : 0x41;  // 'A'
  const size = 14, dpi = 150;
  const bytes = fs.readFileSync(fontPath);
  const p = ft._malloc(bytes.length);
  ft.HEAPU8.set(bytes, p);
  const idx = ft._ftw_add_face(p, bytes.length);
  if (idx < 0) throw new Error('add_face failed');
  ft._ftw_set_char_size(idx, size << 6, dpi);

  const gi = ft._ftw_char_index(idx, cp);
  ft._ftw_load_render(idx, gi);
  const w = ft._ftw_bm_width(idx), h = ft._ftw_bm_rows(idx);
  const ptr = ft._ftw_bitmap(idx);
  const bm = ptr ? Buffer.from(ft.HEAPU8.subarray(ptr, ptr + w * h)) : Buffer.alloc(0);
  console.log(JSON.stringify({
    face: path.basename(fontPath), codepoint: 'U+' + cp.toString(16).toUpperCase(),
    gid: gi, width: w, rows: h,
    bitmap_left: ft._ftw_bm_left(idx), bitmap_top: ft._ftw_bm_top(idx),
    advance_x_26_6: ft._ftw_advance_x(idx),
    size_height: ft._ftw_size_height(idx),
    size_ascender: ft._ftw_size_ascender(idx),
    size_descender: ft._ftw_size_descender(idx),
    gray: ft._ftw_bitmap_is_gray(idx),
    bitmap_sha256: crypto.createHash('sha256').update(bm).digest('hex'),
  }, null, 1));
})();
