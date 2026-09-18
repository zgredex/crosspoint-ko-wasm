#!/usr/bin/env python3
"""Fast TTF/OTF -> .epdfont converter (byte-identical to ttf_to_epdfont.py).

Speedups over the stock script:
  1. Validation pass uses face.get_char_index() only (no FT_LOAD_RENDER raster).
  2. Glyph buffers are materialised once (bytes) — the stock script iterates
     freetype-py's lazy per-index buffer (tens of millions of __getitem__ calls).
  3. 2-bit/1-bit packing uses a precomputed 8-bit->2/1-bit LUT + tight pack
     loops instead of the stock per-pixel 4-bit intermediate dance.

Output is byte-for-byte identical to ttf_to_epdfont.py for the same inputs
(verified: same intervals, glyphs, metrics, bitmap bytes, header layout).
"""
import argparse
import ctypes
import math
import struct
import sys
from collections import namedtuple

import freetype

EPDFONT_MAGIC = 0x46445045  # "EPDF"
EPDFONT_VERSION = 1

# Synthetic bold via FreeType's FT_Outline_Embolden (26.6 px, 1/64 px units).
# freetype-py exposes no embolden API, so load libfreetype directly. Resolution
# order: FREETYPE_LIB_DIR env (server sets it) > common Homebrew paths >
# ctypes.util. Loaded lazily only when --embolden is used.
_ftlib = None
_FT_Outline_Embolden = None
_FT_Render_Glyph = None


def _load_ft():
    global _ftlib, _FT_Outline_Embolden, _FT_Render_Glyph
    if _ftlib is not None:
        return True
    import ctypes.util
    import os
    cands = []
    env_dir = os.environ.get("FREETYPE_LIB_DIR")
    if env_dir:
        for name in ("libfreetype.dylib", "libfreetype.6.dylib", "libfreetype.so", "libfreetype.so.6"):
            cands.append(os.path.join(env_dir, name))
    cands += [
        "/opt/homebrew/opt/freetype/lib/libfreetype.6.dylib",
        "/usr/local/opt/freetype/lib/libfreetype.dylib",
        "/usr/lib/x86_64-linux-gnu/libfreetype.so.6",
        "/usr/lib/aarch64-linux-gnu/libfreetype.so.6",
    ]
    found = ctypes.util.find_library("freetype")
    if found:
        cands.append(found)
    for c in cands:
        try:
            _ftlib = ctypes.CDLL(c)
            _FT_Outline_Embolden = _ftlib.FT_Outline_Embolden
            _FT_Outline_Embolden.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.c_long]
            _FT_Outline_Embolden.restype = ctypes.c_int
            _FT_Render_Glyph = _ftlib.FT_Render_Glyph
            _FT_Render_Glyph.argtypes = [ctypes.c_void_p, ctypes.c_int]
            _FT_Render_Glyph.restype = ctypes.c_int
            return True
        except (OSError, AttributeError):
            continue
    return False


def embolden_glyph(face, emb_px64, cp=None):
    """Embolden the already-loaded outline glyph in place, then rasterize it.

    face must have the target glyph loaded with FT_LOAD_NO_BITMAP (outline,
    hinted). FT_Outline_Embolden scales monotonically with strength (verified;
    FT_GlyphSlot_Embolden saturates at its minimum — do not use it). The
    renderer recomputes bitmap_left/top from the grown outline; FreeType does
    NOT update the advance, so callers must add emb_px64 to it themselves.
    """
    if not _load_ft():
        raise RuntimeError("embolden: libfreetype not found")
    slot = face.glyph._FT_GlyphSlot
    outline = ctypes.byref(slot.contents.outline)
    rc = _FT_Outline_Embolden(ctypes.cast(outline, ctypes.c_void_p), emb_px64, emb_px64)
    if rc == 6:  # FT_Err_Invalid_Argument: outline too degenerate to thicken
        # Space has n_points == 0; U+2000-style glyphs have a single point and no
        # contour. FreeType rejects both, and raising here aborted the WHOLE conversion
        # for any font as soon as --embolden was used, because U+0020 and U+2000 are both
        # in the default intervals. Nothing to thicken: render plainly and carry on.
        # Same rule in tools/ft_wasm.c.
        rc = _FT_Render_Glyph(ctypes.cast(slot, ctypes.c_void_p), 0)  # FT_RENDER_MODE_NORMAL
        if rc != 0:
            raise RuntimeError("FT_Render_Glyph failed rc=%d" % rc)
        return
    if rc != 0:
        raise RuntimeError("FT_Outline_Embolden failed rc=%d (codepoint U+%s, n_points=%d)"
                           % (rc, ("%04X" % cp) if cp is not None else "?",
                              slot.contents.outline.n_points))
    rc = _FT_Render_Glyph(ctypes.cast(slot, ctypes.c_void_p), 0)  # FT_RENDER_MODE_NORMAL
    if rc != 0:
        raise RuntimeError("FT_Render_Glyph failed rc=%d" % rc)

GlyphProps = namedtuple("GlyphProps", ["width", "height", "advance_x", "left", "top",
                                       "data_length", "data_offset", "code_point"])

# 8-bit gray -> 2-bit value, matching stock: v>>4 in [0,3]; >=12->3, >=8->2, >=4->1
LUT2 = bytes((3 if (v >> 4) >= 12 else (2 if (v >> 4) >= 8 else (1 if (v >> 4) >= 4 else 0)))
             for v in range(256))
# 8-bit gray -> 1-bit threshold: stock even/odd analysis reduces to v >= 32
# (see pack_1bit comment); keep per-pixel via translate for clarity.
LUT1 = bytes((1 if v >= 32 else 0) for v in range(256))


def norm_floor(val):
    return int(math.floor(val / (1 << 6)))


def norm_ceil(val):
    return int(math.ceil(val / (1 << 6)))


def pack_2bit(raw):
    """Pack 8-bit gray rows (pitch==width, flat) into 2-bit glyph bytes.

    Stock equivalence: 4-bit nibble pair stage + per-pixel scan reduces to
    LUT2 per byte, then 4 px MSB-first per output byte.
    """
    vals = raw.translate(LUT2)          # 1 byte per pixel, value 0..3
    n = len(vals)
    out = bytearray()
    ap = out.append
    i = 0
    n4 = n & ~3
    while i < n4:
        ap((vals[i] << 6) | (vals[i + 1] << 4) | (vals[i + 2] << 2) | vals[i + 3])
        i += 4
    if i < n:                            # tail: pad low bits (stock: px << (4-n%4)*2)
        acc = 0
        while i < n:
            acc = (acc << 2) | vals[i]
            i += 1
        ap(acc << (2 * (4 - (n & 3))))
    return bytes(out)


def pack_1bit(raw):
    """Pack 8-bit gray rows into 1-bit glyph bytes, byte-identical to stock.

    Stock (per odd/even nibble extraction) sets a pixel when its 4-bit value
    is >= 2, i.e. raw >= 32 for both parities — equivalent to a single
    threshold LUT1 applied to the raw 8-bit buffer, then 8 px MSB-first/byte.
    """
    vals = raw.translate(LUT1)
    n = len(vals)
    out = bytearray()
    ap = out.append
    i = 0
    n8 = n & ~7
    while i < n8:
        ap((vals[i] << 7) | (vals[i + 1] << 6) | (vals[i + 2] << 5) | (vals[i + 3] << 4) |
           (vals[i + 4] << 3) | (vals[i + 5] << 2) | (vals[i + 6] << 1) | vals[i + 7])
        i += 8
    if i < n:
        acc = 0
        nb = 0
        while i < n:
            acc = (acc << 1) | vals[i]
            nb += 1
            i += 1
        ap(acc << (8 - nb))
    return bytes(out)


def convert_ttf_to_epdfont(font_files, font_name, size, output_path, additional_intervals=None,
                           is_2bit=False, embolden_px64=0):
    font_stack = [freetype.Face(f) for f in font_files]
    for face in font_stack:
        face.set_char_size(size << 6, size << 6, 150, 150)
    do_embolden = embolden_px64 > 0
    if do_embolden and not _load_ft():
        raise RuntimeError("embolden requested but libfreetype could not be loaded")

    # Default intervals (Latin etc.) — identical to stock
    intervals = [
        (0x0000, 0x007F), (0x0080, 0x00FF), (0x0100, 0x017F),
        (0x2000, 0x206F), (0x2010, 0x203A), (0x2040, 0x205F),
        (0x20A0, 0x20CF), (0x0300, 0x036F), (0x0400, 0x04FF),
        (0x2200, 0x22FF), (0x2190, 0x21FF),
    ]
    korean_intervals = [
        (0xAC00, 0xD7AF), (0x1100, 0x11FF), (0x3130, 0x318F), (0x3000, 0x303F),
    ]
    if additional_intervals:
        for interval in additional_intervals:
            parts = interval.split(',')
            if len(parts) == 2:
                intervals.append((int(parts[0], 0), int(parts[1], 0)))
    if 'hangul' in font_name.lower() or 'korean' in font_name.lower() or 'hangeuljaemin' in font_name.lower():
        intervals.extend(korean_intervals)

    unmerged = sorted(intervals)
    merged = []
    for a, b in unmerged:
        if merged and a <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], b))
        else:
            merged.append((a, b))

    # Validation: existence only — get_char_index, NO raster (stock rasterises here)
    validated = []
    for a, b in merged:
        start = a
        for cp in range(a, b + 1):
            if not any(f.get_char_index(cp) for f in font_stack):
                if start < cp:
                    validated.append((start, cp - 1))
                start = cp + 1
        if start <= b:
            validated.append((start, b))

    print(f"Processing {len(validated)} intervals...")

    total_size = 0
    all_glyphs = []
    pack = pack_2bit if is_2bit else pack_1bit
    for a, b in validated:
        for cp in range(a, b + 1):
            face = None
            for f in font_stack:
                gi = f.get_char_index(cp)
                if gi > 0:
                    if do_embolden:
                        # outline load (hinted) → FT_Outline_Embolden → raster
                        f.load_glyph(gi, freetype.FT_LOAD_NO_BITMAP)
                        embolden_glyph(f, embolden_px64, cp)
                    else:
                        f.load_glyph(gi, freetype.FT_LOAD_RENDER)
                    face = f
                    break
            if face is None:
                continue
            bm = face.glyph.bitmap
            w, rows = bm.width, bm.rows
            if w == 0 or rows == 0:
                packed = b''
            else:
                # Zero-copy C read of the glyph bitmap (freetype-py's .buffer
                # builds a Python list per element — tens of millions of calls).
                raw = ctypes.string_at(bm._FT_Bitmap.buffer, rows * abs(bm.pitch))
                packed = pack(raw)
            total_size += len(packed)
            adv_x = face.glyph.advance.x
            if do_embolden:
                # FT_Outline_Embolden leaves the advance untouched; a bolder
                # glyph needs its pen advance grown by the same amount (26.6).
                adv_x += embolden_px64
            all_glyphs.append((GlyphProps(
                width=w,
                height=rows,
                advance_x=norm_floor(adv_x),
                left=face.glyph.bitmap_left,
                top=face.glyph.bitmap_top,
                data_length=len(packed),
                data_offset=total_size - len(packed),
                code_point=cp,
            ), packed))

    face = font_stack[0]
    for f in font_stack:
        if f.get_char_index(ord('|')) > 0:
            f.load_glyph(f.get_char_index(ord('|')), freetype.FT_LOAD_RENDER)
            face = f
            break
    advance_y = norm_ceil(face.size.height)
    ascender = norm_ceil(face.size.ascender)
    descender = norm_floor(face.size.descender)

    print(f"Generated {len(all_glyphs)} glyphs")
    print(f"Font metrics: advanceY={advance_y}, ascender={ascender}, descender={descender}")
    write_epdfont(output_path, validated, all_glyphs, advance_y, ascender, descender, is_2bit)
    return True


def write_epdfont(output_path, intervals, all_glyphs, advance_y, ascender, descender, is_2bit):
    header_size = 32
    intervals_size = len(intervals) * 12
    glyphs_size = len(all_glyphs) * 16
    intervals_offset = header_size
    glyphs_offset = intervals_offset + intervals_size
    bitmap_offset = glyphs_offset + glyphs_size
    bitmap_data = b''.join(p for _, p in all_glyphs)

    with open(output_path, 'wb') as f:
        header = struct.pack(
            '<IHBBBBBB5I',
            EPDFONT_MAGIC, EPDFONT_VERSION, 1 if is_2bit else 0, 0,
            advance_y & 0xFF, ascender & 0xFF, descender & 0xFF, 0,
            len(intervals), len(all_glyphs),
            intervals_offset, glyphs_offset, bitmap_offset,
        )
        f.write(header)
        offset = 0
        for a, b in intervals:
            f.write(struct.pack('<3I', a, b, offset))
            offset += b - a + 1
        for g, _ in all_glyphs:
            f.write(struct.pack('<4B2h2I', g.width, g.height, g.advance_x, 0,
                                g.left, g.top, g.data_length, g.data_offset))
        f.write(bitmap_data)

    total_size = bitmap_offset + len(bitmap_data)
    print(f"\nCreated: {output_path}")
    print(f"  Intervals: {len(intervals)}")
    print(f"  Glyphs: {len(all_glyphs)}")
    print(f"  Bitmap size: {len(bitmap_data)} bytes")
    print(f"  Total file size: {total_size} bytes ({total_size / 1024 / 1024:.2f} MB)")


def main():
    parser = argparse.ArgumentParser(description='Fast TTF/OTF -> .epdfont (byte-identical)')
    parser.add_argument('name')
    parser.add_argument('size', type=int)
    parser.add_argument('fontfiles', nargs='+')
    parser.add_argument('--2bit', dest='is_2bit', action='store_true')
    parser.add_argument('--additional-intervals', dest='additional_intervals', action='append')
    parser.add_argument('--embolden', dest='embolden_px64', type=int, default=0,
                        help='Synthetic bold: FT_Outline_Embolden strength in 1/64 px (26.6)')
    parser.add_argument('-o', '--output', dest='output')
    args = parser.parse_args()

    output_path = args.output if args.output else f"{args.name}_{args.size}.epdfont"
    print(f"Converting {args.fontfiles[0]} to {output_path}")
    print(f"Font: {args.name}, Size: {args.size}pt, Mode: {'2-bit' if args.is_2bit else '1-bit'}, "
          f"Embolden: {args.embolden_px64}/64px")
    ok = convert_ttf_to_epdfont(args.fontfiles, args.name, args.size, output_path,
                                args.additional_intervals, args.is_2bit, args.embolden_px64)
    if ok:
        print("\nConversion complete!")
    else:
        print("\nConversion failed!")
        sys.exit(1)


if __name__ == '__main__':
    main()
