#!/usr/bin/env python3
"""Reference for ft_glyph_probe.js: the SAME one-glyph call chain the converter makes,
via freetype-py (the local tool's own rasterizer), so the wasm shim can be compared
metric-for-metric and byte-for-byte against it.

usage: ft_glyph_probe_ref.py <font> <hex codepoint> [size] [dpi]
"""
import ctypes
import hashlib
import json
import sys

import freetype

font = sys.argv[1]
cp = int(sys.argv[2], 16)
size = int(sys.argv[3]) if len(sys.argv) > 3 else 14
dpi = int(sys.argv[4]) if len(sys.argv) > 4 else 150

face = freetype.Face(font)
face.set_char_size(size << 6, size << 6, dpi, dpi)
gi = face.get_char_index(cp)
face.load_glyph(gi, freetype.FT_LOAD_RENDER)
bm = face.glyph.bitmap
raw = ctypes.string_at(bm._FT_Bitmap.buffer, bm.rows * abs(bm.pitch))
# the packer walks the buffer flat, so report the same tight view the wasm shim builds
tight = b''.join(raw[y * abs(bm.pitch):y * abs(bm.pitch) + bm.width] for y in range(bm.rows)) if bm.rows else b''

print(json.dumps({
    "face": font.split("/")[-1],
    "codepoint": "U+%X" % cp,
    "gid": gi,
    "width": bm.width,
    "rows": bm.rows,
    "bitmap_left": face.glyph.bitmap_left,
    "bitmap_top": face.glyph.bitmap_top,
    "advance_x_26_6": face.glyph.advance.x,
    "size_height": face.size.height,
    "size_ascender": face.size.ascender,
    "size_descender": face.size.descender,
    "pitch": bm.pitch,
    "bitmap_sha256": hashlib.sha256(tight).hexdigest(),
}, indent=1))
