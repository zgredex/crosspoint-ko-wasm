/*
 * epdfont.js - TTF/OTF -> .epdfont conversion that runs entirely in the browser.
 *
 * The hosted site has no backend, and the local tool (tools/ttf_to_epdfont_fast.py,
 * freetype-py + fontTools) cannot run there, so picking a custom reader font used to be a
 * dead end on the deployed build. This module replaces it: FreeType itself, compiled to
 * wasm (tools/ft_wasm.c via scripts/build_ft_wasm.sh), does the rasterizing, and the
 * container packing below is a line-by-line port of the Python - same intervals, same
 * glyph selection order, same metrics, same LUTs, same header.
 *
 * Verified byte-identical against the Python tool: scripts/verify/epdfont_parity.js
 * converts the same font both ways and compares the files (see
 * docs/font-conversion-in-browser.md for the numbers, and for the one path where the two
 * cannot be identical - variable fonts, where fontTools bakes a new static font while
 * FreeType interpolates at load time).
 *
 * Usage (worker or page):
 *   const conv = await EpdFontConverter.load({ locateFile: (f) => base + f });
 *   const bytes = await conv.convert({ fontBytes, name, size, twoBit, noHangul, ... });
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.EpdFontConverter = factory();
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var FT_JS = 'ft_wasm.js';
  var EPDFONT_MAGIC = 0x46445045;  // "EPDF"
  var EPDFONT_VERSION = 1;
  var MAX_UNICODE = 0x10FFFF;
  var MAX_EXTRA_INTERVALS = 64;
  var MAX_REQUESTED_CODEPOINTS = 131072;
  var MAX_FONT_SOURCE_BYTES = 64 * 1024 * 1024;
  var MAX_EPDFONT_BYTES = 64 * 1024 * 1024;
  var MAX_GLYPH_BITMAP_BYTES = 4 * 1024 * 1024;

  // ---- intervals (identical to the tool) ----------------------------------
  var DEFAULT_INTERVALS = [
    [0x0000, 0x007F], [0x0080, 0x00FF], [0x0100, 0x017F],
    [0x2000, 0x206F], [0x2010, 0x203A], [0x2040, 0x205F],
    [0x20A0, 0x20CF], [0x0300, 0x036F], [0x0400, 0x04FF],
    [0x2200, 0x22FF], [0x2190, 0x21FF],
  ];
  var KOREAN_INTERVALS = [
    // U+AC00-U+D7A3 is the 11,172 assigned modern syllables; D7A4-D7AF is unassigned
    // padding inside the block, so it is not requested.
    [0xAC00, 0xD7A3], [0x1100, 0x11FF], [0x3130, 0x318F], [0x3000, 0x303F],
  ];
  var HANGUL_FULL = [0xAC00, 0xD7A3];

  function mergeIntervals(list) {
    var sorted = list.slice().sort(function (a, b) { return a[0] - b[0]; });
    var out = [];
    for (var i = 0; i < sorted.length; i++) {
      var a = sorted[i][0], b = sorted[i][1];
      if (out.length && a <= out[out.length - 1][1] + 1) {
        out[out.length - 1][1] = Math.max(out[out.length - 1][1], b);
      } else {
        out.push([a, b]);
      }
    }
    return out;
  }

  function validateIntervals(list, label, maxIntervals) {
    if (!Array.isArray(list)) throw new Error(label + ' must be an array');
    if (list.length > maxIntervals) throw new Error(label + ' contains too many ranges');
    var out = [];
    list.forEach(function (iv) {
      if (!Array.isArray(iv) || iv.length !== 2 ||
          !Number.isSafeInteger(iv[0]) || !Number.isSafeInteger(iv[1]) ||
          iv[0] < 0 || iv[1] < iv[0] || iv[1] > MAX_UNICODE) {
        throw new Error('invalid Unicode interval in ' + label);
      }
      out.push([iv[0], iv[1]]);
    });
    return out;
  }

  function boundedMergedIntervals(list) {
    var merged = mergeIntervals(validateIntervals(list, 'font intervals',
                                                   DEFAULT_INTERVALS.length + KOREAN_INTERVALS.length +
                                                   MAX_EXTRA_INTERVALS + 1));
    var total = 0;
    merged.forEach(function (iv) {
      total += iv[1] - iv[0] + 1;
      if (!Number.isSafeInteger(total) || total > MAX_REQUESTED_CODEPOINTS) {
        throw new Error('font intervals exceed the 131072-codepoint limit');
      }
    });
    return merged;
  }

  // One or more ranges separated by semicolons. Each range accepts MIN-MAX,
  // MIN:MAX, or the historical server form MIN,MAX. Non-empty malformed input
  // is rejected rather than silently broadening/narrowing the requested font.
  function parseIntervalSpec(text) {
    var out = [];
    String(text || '').split(';').forEach(function (chunk) {
      chunk = chunk.trim();
      if (!chunk) return;
      var m = /^(0[xX][0-9a-fA-F]+)\s*[-:,]\s*(0[xX][0-9a-fA-F]+)$/.exec(chunk);
      if (!m) throw new Error('invalid Unicode interval: ' + chunk);
      out.push([parseInt(m[1], 16), parseInt(m[2], 16)]);
    });
    return validateIntervals(out, 'extra font intervals', MAX_EXTRA_INTERVALS);
  }

  function requireRange(value, min, max, label) {
    if (!Number.isSafeInteger(value) || value < min || value > max) {
      throw new Error(label + ' is not representable in EPDFont v1: ' + value);
    }
    return value;
  }

  // ---- packing (LUT + bit order identical to the tool) --------------------
  var LUT2 = new Uint8Array(256);
  var LUT1 = new Uint8Array(256);
  for (var v = 0; v < 256; v++) {
    var hi = v >> 4;
    LUT2[v] = hi >= 12 ? 3 : (hi >= 8 ? 2 : (hi >= 4 ? 1 : 0));
    LUT1[v] = v >= 32 ? 1 : 0;
  }

  function pack2bit(raw) {
    var n = raw.length;
    var out = new Uint8Array((n + 3) >> 2);
    var o = 0, i = 0;
    var n4 = n & ~3;
    for (; i < n4; i += 4) {
      out[o++] = (LUT2[raw[i]] << 6) | (LUT2[raw[i + 1]] << 4) | (LUT2[raw[i + 2]] << 2) | LUT2[raw[i + 3]];
    }
    if (i < n) {
      var acc = 0;
      while (i < n) { acc = (acc << 2) | LUT2[raw[i]]; i++; }
      out[o++] = acc << (2 * (4 - (n & 3)));
    }
    return out.subarray(0, o);
  }

  function pack1bit(raw) {
    var n = raw.length;
    var out = new Uint8Array((n + 7) >> 3);
    var o = 0, i = 0;
    var n8 = n & ~7;
    for (; i < n8; i += 8) {
      out[o++] = (LUT1[raw[i]] << 7) | (LUT1[raw[i + 1]] << 6) | (LUT1[raw[i + 2]] << 5) | (LUT1[raw[i + 3]] << 4) |
                 (LUT1[raw[i + 4]] << 3) | (LUT1[raw[i + 5]] << 2) | (LUT1[raw[i + 6]] << 1) | LUT1[raw[i + 7]];
    }
    if (i < n) {
      var acc = 0, nb = 0;
      while (i < n) { acc = (acc << 1) | LUT1[raw[i]]; nb++; i++; }
      out[o++] = acc << (8 - nb);
    }
    return out.subarray(0, o);
  }

  // ---- metrics helpers (same rounding as the tool) ------------------------
  function normFloor(val) { return Math.floor(val / 64); }
  function normCeil(val) { return Math.ceil(val / 64); }

  // server.py: static faces get synthetic bold relative to their OS/2 weight.
  function weightToEmboldenPx64(weight, nativeWeight, size) {
    var delta = weight - nativeWeight;
    if (delta <= 0) return 0;
    var ppem = size * 150.0 / 72.0;
    var px = 0.5 * (delta / 100.0) * (ppem / 29.17);
    px = Math.max(0.0, Math.min(px, 2.5));
    return Math.round(px * 64);
  }

  // OS/2 usWeightClass, read straight from the file (fontTools does this server-side).
  function nativeWeight(bytes) {
    var dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (bytes.length < 12) return 400;
    var numTables = dv.getUint16(4);
    for (var i = 0; i < numTables; i++) {
      var rec = 12 + i * 16;
      if (rec + 16 > bytes.length) break;
      var tag = String.fromCharCode(bytes[rec], bytes[rec + 1], bytes[rec + 2], bytes[rec + 3]);
      if (tag === 'OS/2') {
        var off = dv.getUint32(rec + 8);
        if (off + 6 <= bytes.length) return dv.getUint16(off + 4);
      }
    }
    return 400;
  }

  // ---- conversion ---------------------------------------------------------
  function createConverter(ft) {
    function convert(opts) {
      opts = opts || {};
      var fontFiles = opts.fontFiles || [opts.fontBytes];
      var name = (opts.name || 'custom').replace(/[^\w-]/g, '') || 'custom';
      var requestedSize = opts.size === undefined ? 14 : Number(opts.size);
      if (!Number.isFinite(requestedSize)) throw new Error('invalid font size');
      var size = Math.max(6, Math.min(Math.trunc(requestedSize), 72));
      var twoBit = opts.twoBit !== false;
      var requestedWeight = opts.weight === undefined ? 500 : Number(opts.weight);
      if (!Number.isFinite(requestedWeight)) throw new Error('invalid font weight');
      var weight = Math.max(100, Math.min(Math.trunc(requestedWeight), 900));
      var onProgress = opts.onProgress || function () {};
      var spacePx = (Number.isSafeInteger(opts.spacePx) && opts.spacePx >= 0 && opts.spacePx <= 255)
        ? opts.spacePx : null;

      if (!Array.isArray(fontFiles) || fontFiles.length < 1 || fontFiles.length > 4) {
        throw new Error('font stack must contain between 1 and 4 faces');
      }
      var sourceBytes = 0;
      fontFiles.forEach(function (b) {
        if (!(b instanceof Uint8Array) || b.length < 4 || b.length > MAX_FONT_SOURCE_BYTES) {
          throw new Error('invalid font source size');
        }
        sourceBytes += b.length;
        if (!Number.isSafeInteger(sourceBytes) || sourceBytes > MAX_FONT_SOURCE_BYTES) {
          throw new Error('font stack exceeds the supported 64 MiB source limit');
        }
      });

      ft._ftw_reset();
      try {
      var handles = [];
      for (var f = 0; f < fontFiles.length; f++) {
        var b = fontFiles[f];
        var ptr = ft._malloc(b.length);
        if (!ptr) throw new Error('out of memory while loading font face ' + f);
        var idx = -1;
        try {
          ft.HEAPU8.set(b, ptr);
          idx = ft._ftw_add_face(ptr, b.length);
        } finally {
          ft._free(ptr);
        }
        if (idx < 0) throw new Error('FreeType could not open the font (face ' + f + ')');
        handles.push(idx);
      }
      handles.forEach(function (h) {
        if (!ft._ftw_set_char_size(h, size << 6, 150)) {
          throw new Error('FreeType rejected the requested font size');
        }
      });

      // weight: variable fonts interpolate on their own axis; static faces embolden.
      var appliedEmbolden = 0, weightMode = 'native', native = null, effectiveWeight = weight;
      var minP = ft._malloc(4), maxP = ft._malloc(4);
      if (!minP || !maxP) {
        if (minP) ft._free(minP);
        if (maxP) ft._free(maxP);
        throw new Error('out of memory while reading font axes');
      }
      var hasAxis = ft._ftw_wght_range(handles[0], minP, maxP) === 1;
      var axisMin = ft.HEAP32[minP >> 2], axisMax = ft.HEAP32[maxP >> 2];
      ft._free(minP); ft._free(maxP);
      if (hasAxis) {
        effectiveWeight = Math.max(axisMin, Math.min(weight, axisMax));
        handles.forEach(function (h) {
          if (!ft._ftw_set_wght(h, effectiveWeight)) {
            throw new Error('FreeType rejected the requested weight');
          }
        });
        weightMode = 'wght-instance';
      } else {
        native = nativeWeight(fontFiles[0]);
        appliedEmbolden = weightToEmboldenPx64(weight, native, size);
        if (appliedEmbolden > 0) weightMode = 'embolden';
      }

      var intervals = DEFAULT_INTERVALS.slice();
      var lower = name.toLowerCase();
      if (lower.indexOf('hangul') >= 0 || lower.indexOf('korean') >= 0 || lower.indexOf('hangeuljaemin') >= 0) {
        intervals = intervals.concat(KOREAN_INTERVALS);
      }
      if (!opts.noHangul) intervals.push(HANGUL_FULL);  // server.py default
      var extras = [];
      (opts.extraIntervals || []).forEach(function (iv) {
        if (Array.isArray(iv)) extras.push(iv);
        else extras = extras.concat(parseIntervalSpec(iv));
      });
      extras = validateIntervals(extras, 'extra font intervals', MAX_EXTRA_INTERVALS);
      var merged = boundedMergedIntervals(intervals.concat(extras));

      // validation pass: existence only, no raster (the tool's speedup #1)
      var validated = [];
      merged.forEach(function (iv) {
        var start = iv[0];
        for (var cp = iv[0]; cp <= iv[1]; cp++) {
          var has = false;
          for (var h = 0; h < handles.length; h++) {
            if (ft._ftw_char_index(handles[h], cp) > 0) { has = true; break; }
          }
          if (!has) {
            if (start < cp) validated.push([start, cp - 1]);
            start = cp + 1;
          }
        }
        if (start <= iv[1]) validated.push([start, iv[1]]);
      });

      var total = 0;
      validated.forEach(function (iv) { total += iv[1] - iv[0] + 1; });
      if (total > MAX_REQUESTED_CODEPOINTS) throw new Error('validated font exceeds glyph limit');

      // Metadata size is known before the first bitmap is copied or retained.
      // This prevents a request that cannot fit the output ceiling from first
      // constructing a large blobs[] graph and only failing at final assembly.
      var fixedBytes = 32 + validated.length * 12 + total * 16;
      if (!Number.isSafeInteger(fixedBytes) || fixedBytes > MAX_EPDFONT_BYTES) {
        throw new Error('EPDFont metadata exceeds the supported 64 MiB limit');
      }

      var props = [];      // {width, height, advanceX, left, top, length, offset, cp}
      var blobs = [];
      var packedTotal = 0;
      var done = 0;
      var pack = twoBit ? pack2bit : pack1bit;

      validated.forEach(function (iv) {
        for (var cp = iv[0]; cp <= iv[1]; cp++) {
          done++;
          var face = -1, gi = 0;
          for (var h = 0; h < handles.length; h++) {
            var g = ft._ftw_char_index(handles[h], cp);
            if (g > 0) { face = h; gi = g; break; }
          }
          if (face < 0) throw new Error('validated glyph disappeared at U+' + cp.toString(16).toUpperCase());
          if (appliedEmbolden > 0) {
            if (!ft._ftw_load_outline(face, gi)) {
              throw new Error('FreeType outline load failed at U+' + cp.toString(16).toUpperCase());
            }
            if (!ft._ftw_embolden(face, appliedEmbolden)) {
              throw new Error('FreeType embolden failed at U+' + cp.toString(16).toUpperCase());
            }
          } else {
            if (!ft._ftw_load_render(face, gi)) {
              throw new Error('FreeType raster failed at U+' + cp.toString(16).toUpperCase());
            }
          }
          var w = ft._ftw_bm_width(face), rows = ft._ftw_bm_rows(face);
          requireRange(w, 0, 255, 'glyph width at U+' + cp.toString(16).toUpperCase());
          requireRange(rows, 0, 255, 'glyph height at U+' + cp.toString(16).toUpperCase());
          var bitmapBytes = w * rows;
          if (!Number.isSafeInteger(bitmapBytes) || bitmapBytes > MAX_GLYPH_BITMAP_BYTES) {
            throw new Error('glyph bitmap exceeds the 4 MiB limit at U+' + cp.toString(16).toUpperCase());
          }
          var packed = null;
          if (w > 0 && rows > 0) {
            if (!ft._ftw_bitmap_is_gray(face)) {
              throw new Error('unexpected non-grayscale bitmap (pixel mode ' + ft._ftw_bitmap_is_gray(face) + ')');
            }
            var bptr = ft._ftw_bitmap(face);
            if (!bptr) throw new Error('FreeType bitmap copy failed at U+' + cp.toString(16).toUpperCase());
            packed = pack(ft.HEAPU8.subarray(bptr, bptr + bitmapBytes));
          } else {
            packed = new Uint8Array(0);
          }
          var advX = ft._ftw_advance_x(face);
          if (appliedEmbolden > 0) advX += appliedEmbolden;  // FreeType does not update it
          var advanceX = requireRange(normFloor(advX), 0, 255,
                                      'glyph advance at U+' + cp.toString(16).toUpperCase());
          var left = requireRange(ft._ftw_bm_left(face), -32768, 32767,
                                  'glyph left bearing at U+' + cp.toString(16).toUpperCase());
          var top = requireRange(ft._ftw_bm_top(face), -32768, 32767,
                                 'glyph top bearing at U+' + cp.toString(16).toUpperCase());
          if (packedTotal > MAX_EPDFONT_BYTES - fixedBytes - packed.length) {
            throw new Error('converted epdfont exceeds the supported 64 MiB limit');
          }
          props.push({
            width: w, height: rows, advanceX: advanceX,
            left: left, top: top,
            length: packed.length, offset: packedTotal, codePoint: cp,
          });
          blobs.push(packed);
          packedTotal += packed.length;
          if (onProgress && (done & 511) === 0) onProgress(done, total);
        }
      });

      // vertical metrics from the first face that has '|' (the tool's choice)
      var metricFace = handles[0];
      for (var k = 0; k < handles.length; k++) {
        if (ft._ftw_char_index(handles[k], 0x7C) > 0) { metricFace = handles[k]; break; }
      }
      var advanceY = normCeil(ft._ftw_size_height(metricFace));
      var ascender = normCeil(ft._ftw_size_ascender(metricFace));
      var descender = normFloor(ft._ftw_size_descender(metricFace));
      requireRange(advanceY, 1, 255, 'font line height');
      requireRange(ascender, -128, 127, 'font ascender');
      requireRange(descender, -128, 127, 'font descender');

      var headerSize = 32;
      var intervalsSize = validated.length * 12;
      var glyphsSize = props.length * 16;
      var bitmapOffset = headerSize + intervalsSize + glyphsSize;
      if (bitmapOffset !== fixedBytes || bitmapOffset + packedTotal > MAX_EPDFONT_BYTES) {
        throw new Error('converted epdfont exceeds the supported 64 MiB limit');
      }
      var outBytes = new Uint8Array(bitmapOffset + packedTotal);
      var dv = new DataView(outBytes.buffer);

      dv.setUint32(0, EPDFONT_MAGIC, true);
      dv.setUint16(4, EPDFONT_VERSION, true);
      outBytes[6] = twoBit ? 1 : 0;
      outBytes[7] = 0;
      dv.setUint8(8, advanceY);
      dv.setInt8(9, ascender);
      dv.setInt8(10, descender);
      outBytes[11] = 0;
      dv.setUint32(12, validated.length, true);
      dv.setUint32(16, props.length, true);
      dv.setUint32(20, headerSize, true);
      dv.setUint32(24, headerSize + intervalsSize, true);
      dv.setUint32(28, bitmapOffset, true);

      var off = 32;
      var glyphIndex = 0;
      validated.forEach(function (iv) {
        dv.setUint32(off, iv[0], true);
        dv.setUint32(off + 4, iv[1], true);
        dv.setUint32(off + 8, glyphIndex, true);
        glyphIndex += iv[1] - iv[0] + 1;
        off += 12;
      });
      props.forEach(function (p) {
        outBytes[off] = p.width;
        outBytes[off + 1] = p.height;
        outBytes[off + 2] = p.advanceX;
        outBytes[off + 3] = 0;
        dv.setInt16(off + 4, p.left, true);
        dv.setInt16(off + 6, p.top, true);
        dv.setUint32(off + 8, p.length, true);
        dv.setUint32(off + 12, p.offset, true);
        off += 16;
      });
      var bpos = bitmapOffset;
      blobs.forEach(function (b) { outBytes.set(b, bpos); bpos += b.length; });

      // space advance patch (server.py does this after conversion, on the packed bytes)
      if (spacePx !== null) {
        for (var i = 0; i < validated.length; i++) {
          var a = validated[i][0], z = validated[i][1];
          if (a <= 0x20 && 0x20 <= z) {
            var gidx = dv.getUint32(32 + i * 12 + 8, true);
            var rec = 32 + intervalsSize + (gidx + (0x20 - a)) * 16;
            if (rec + 16 <= outBytes.length) outBytes[rec + 2] = spacePx;
            break;
          }
        }
      }

      return {
        bytes: outBytes,
        glyphCount: props.length,
        intervalCount: validated.length,
        bitmapBytes: packedTotal,
        advanceY: advanceY, ascender: ascender, descender: descender,
        weightMode: weightMode, effectiveWeight: effectiveWeight,
        nativeWeight: native, emboldenPx64: appliedEmbolden,
      };
      } finally {
        ft._ftw_reset();
      }
    }

    return { convert: convert };
  }

  function load(opts) {
    opts = opts || {};
    var factoryPromise = opts.factory;
    if (!factoryPromise) {
      if (typeof createFtConverter !== 'function') {
        return Promise.reject(new Error(
          'ft_wasm.js not loaded - include it before epdfont.js (or pass {factory})'));
      }
      factoryPromise = createFtConverter({ locateFile: opts.locateFile || function (f) { return f; } });
    }
    return Promise.resolve(factoryPromise).then(function (ft) {
      var conv = createConverter(ft);
      conv.ft = ft;                       // for diagnostics (ftw_version) and tests
      conv.ftVersion = ft.UTF8ToString(ft._ftw_version());
      return conv;
    });
  }

  return {
    load: load,
    createConverter: createConverter,
    // exported for tests
    _internals: {
      pack2bit: pack2bit, pack1bit: pack1bit, mergeIntervals: mergeIntervals,
      parseIntervalSpec: parseIntervalSpec, nativeWeight: nativeWeight,
      weightToEmboldenPx64: weightToEmboldenPx64, DEFAULT_INTERVALS: DEFAULT_INTERVALS,
      KOREAN_INTERVALS: KOREAN_INTERVALS,
      validateIntervals: validateIntervals, boundedMergedIntervals: boundedMergedIntervals,
      requireRange: requireRange,
      MAX_EXTRA_INTERVALS: MAX_EXTRA_INTERVALS,
      MAX_REQUESTED_CODEPOINTS: MAX_REQUESTED_CODEPOINTS,
      MAX_FONT_SOURCE_BYTES: MAX_FONT_SOURCE_BYTES,
      MAX_EPDFONT_BYTES: MAX_EPDFONT_BYTES,
      MAX_GLYPH_BITMAP_BYTES: MAX_GLYPH_BITMAP_BYTES,
    },
  };
}));
