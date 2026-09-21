#!/usr/bin/env node
'use strict';

const path = require('path');
const EpdFontConverter = require(path.join(__dirname, '..', '..', 'web', 'epdfont.js'));
const i = EpdFontConverter._internals;

function check(value, message) {
  if (!value) throw new Error(message);
}

function rejects(fn, message) {
  let rejected = false;
  try { fn(); } catch (_) { rejected = true; }
  check(rejected, message);
}

check(JSON.stringify(i.parseIntervalSpec('0x3130,0x318F')) === JSON.stringify([[0x3130, 0x318F]]),
      'historical comma interval was not parsed');
check(i.parseIntervalSpec('0x20-0x7e; 0x1100:0x11ff').length === 2,
      'semicolon-separated interval list was not parsed');
rejects(() => i.parseIntervalSpec('garbage'), 'malformed interval was silently accepted');
rejects(() => i.parseIntervalSpec('0x100,0x20'), 'reversed interval was accepted');
rejects(() => i.parseIntervalSpec('0x0,0x110000'), 'out-of-Unicode interval was accepted');
rejects(() => i.parseIntervalSpec(Array(65).fill('0x20,0x20').join(';')),
        'extra interval-count limit was not enforced');
rejects(() => i.boundedMergedIntervals([[0, 0x10ffff]]),
        'codepoint-scan budget was not enforced');
rejects(() => i.requireRange(256, 0, 255, 'glyph width'),
        'EPDFont uint8 metric overflow was accepted');
rejects(() => i.requireRange(-129, -128, 127, 'font ascender'),
        'EPDFont int8 metric overflow was accepted');

function fakeFt(overrides) {
  const buffer = new ArrayBuffer(4096);
  let next = 64;
  let resets = 0;
  const ft = {
    HEAPU8: new Uint8Array(buffer),
    HEAP32: new Int32Array(buffer),
    _malloc(n) { const p = next; next += Math.max(8, n); return p; },
    _free() {},
    _ftw_reset() { resets++; },
    _ftw_add_face() { return 0; },
    _ftw_set_char_size() { return 1; },
    _ftw_wght_range() { return 0; },
    _ftw_char_index(_face, cp) { return cp === 0 ? 1 : 0; },
    _ftw_load_render() { return 1; },
    _ftw_bm_width() { return 0; },
    _ftw_bm_rows() { return 0; },
    _ftw_advance_x() { return 0; },
    _ftw_bm_left() { return 0; },
    _ftw_bm_top() { return 0; },
    _ftw_size_height() { return 64; },
    _ftw_size_ascender() { return 64; },
    _ftw_size_descender() { return 0; },
  };
  Object.assign(ft, overrides || {});
  ft.resetCount = () => resets;
  return ft;
}

{
  const ft = fakeFt({ _ftw_load_render() { return 0; } });
  const converter = EpdFontConverter.createConverter(ft);
  rejects(() => converter.convert({ fontBytes: new Uint8Array(4), noHangul: true, weight: 400 }),
          'glyph raster failure was silently skipped');
  check(ft.resetCount() === 2, 'failed conversion did not release FreeType state');
}
{
  const ft = fakeFt({ _ftw_size_height() { return 256 * 64; } });
  const converter = EpdFontConverter.createConverter(ft);
  rejects(() => converter.convert({ fontBytes: new Uint8Array(4), noHangul: true, weight: 400 }),
          'font metric overflow was silently truncated');
  check(ft.resetCount() === 2, 'metric rejection did not release FreeType state');
}

console.log('font-limits: intervals, raster failures and metric overflow fail closed');
