#!/usr/bin/env node
// settings_effect.js - which settings actually DO something?
//
// A setting that cannot change the output is a lie in the UI (and upstream hides
// exactly such entries - see the "entries omitted: neither picker would select
// anything renderable" note in its SettingsList.h). So: export the whole book once
// per variant, one field flipped at a time, and diff the page payloads.
//
// Whole-book (not one page) is the point: hyphenation only shows up on lines that
// need breaking, indent only on paragraphs that start a block, alignment only where
// lines are short. A one-page probe would report false negatives.
//
// usage: settings_effect.js <book.epub> [pages-limit]
const fs = require('fs');
const path = require('path');

const WASM_DIR = path.join(__dirname, '..', '..', 'build-wasm');
const EPUB = process.argv[2];
const LIMIT = process.argv[3] ? parseInt(process.argv[3], 10) : 0;

if (!EPUB) {
  console.error('usage: settings_effect.js <book.epub> [pages-limit]');
  process.exit(2);
}

(async () => {
  const createKoEngine = require(path.join(WASM_DIR, 'ko_xtch_wasm.js'));
  const api = await createKoEngine();

  api._ko_init(464, 778);
  const buf = fs.readFileSync(EPUB);
  const p = api._malloc(buf.length);
  api.HEAPU8.set(buf, p);
  const spines = api._ko_load_epub(p, buf.length, '/book.epub');
  api._free(p);
  if (spines < 0) throw new Error('load failed: ' + api.UTF8ToString(api._ko_error()));

  const apply = (s) => {
    api._ko_set_line_compression(s.lineCompression);
    api._ko_set_extra_paragraph_spacing(s.extraParagraphSpacing);
    api._ko_set_paragraph_indent(s.paragraphIndent);
    api._ko_set_character_wrap(s.characterWrap);
    api._ko_set_paragraph_alignment(s.paragraphAlignment);
    api._ko_set_hyphenation(s.hyphenationEnabled);
    api._ko_set_embedded_style(s.embeddedStyle);
    api._ko_set_image_rendering(s.imageRendering);
    api._ko_set_text_aa(s.textAntiAliasing);
    api._ko_set_focus_reading(s.focusReadingEnabled);
  };

  const BASE = {
    lineCompression: 1.20, extraParagraphSpacing: 1, paragraphIndent: 0, characterWrap: 1,
    paragraphAlignment: 0, hyphenationEnabled: 0, embeddedStyle: 1, imageRendering: 0,
    textAntiAliasing: 1, focusReadingEnabled: 0,
  };

  // One field flipped per row; hyphenation is also probed with word wrap, because the
  // engine masks it behind characterWrap (as upstream does).
  const VARIANTS = [
    ['paragraphIndent       0 -> 1', { paragraphIndent: 1 }],
    ['extraParagraphSpacing 1 -> 0', { extraParagraphSpacing: 0 }],
    ['characterWrap         1 -> 0', { characterWrap: 0 }],
    ['hyphenation 0 -> 1 (wrap on : MASKED)', { hyphenationEnabled: 1 }],
    ['hyphenation 0 -> 1 (wrap off)', { characterWrap: 0, hyphenationEnabled: 1 }],
    ['   ...same, with wrap off as base', { characterWrap: 0, hyphenationEnabled: 0 }],
    ['lineCompression     1.20 -> 1.00', { lineCompression: 1.00 }],
    ['lineCompression     1.20 -> 1.40', { lineCompression: 1.40 }],
    ['paragraphAlignment  JUST -> LEFT', { paragraphAlignment: 1 }],
    ['paragraphAlignment  JUST -> CENTER', { paragraphAlignment: 2 }],
    ['paragraphAlignment  JUST -> RIGHT', { paragraphAlignment: 3 }],
    ['paragraphAlignment  JUST -> BOOK_STYLE', { paragraphAlignment: 4 }],
    ['embeddedStyle         1 -> 0', { embeddedStyle: 0 }],
    ['imageRendering    DISPLAY -> PLACEHOLDER', { imageRendering: 1 }],
    ['imageRendering    DISPLAY -> SUPPRESS', { imageRendering: 2 }],
    ['textAntiAliasing      1 -> 0', { textAntiAliasing: 0 }],
  ];

  const xtchPages = (bytes) => {
    const out = [];
    let pos = 0;
    while (true) {
      const q = bytes.indexOf(Buffer.from('XTH\0'), pos);
      if (q < 0) break;
      out.push(bytes.subarray(q + 22, q + 22 + 96000));
      pos = q + 22;
    }
    return out;
  };

  const run = (spec) => {
    apply(spec);
    api._ko_export_set_mode(1);  // 2-bit XTH: the container that carries layout
    api._ko_export_begin();
    for (let s = 0; s < spines; s++) api._ko_export_spine(s);
    const total = api._ko_export_finish();
    if (total < 0) throw new Error('export failed');
    const bytes = Buffer.from(api.HEAPU8.subarray(api._ko_xtch_ptr(),
                                                 api._ko_xtch_ptr() + api._ko_xtch_size()));
    const pages = xtchPages(bytes);
    return LIMIT ? pages.slice(0, LIMIT) : pages;
  };

  console.log(`book: ${EPUB}  (${spines} spines)`);
  const base = run(BASE);
  console.log(`baseline: ${base.length} pages\n`);
  console.log('setting change                              pages changed  first  count');

  // The page COUNT changes for most layout settings, so diff the common prefix and
  // report the count delta alongside - a count change is itself proof of an effect.
  const diffPages = (a, b) => {
    let diff = 0;
    let first = -1;
    const n = Math.min(a.length, b.length);
    for (let i = 0; i < n; i++) {
      if (!a[i].equals(b[i])) { diff++; if (first < 0) first = i; }
    }
    return { diff, first, delta: b.length - a.length };
  };

  let dead = [];
  for (const [label, delta] of VARIANTS) {
    const spec = Object.assign({}, BASE, delta);
    const got = run(spec);
    const { diff, first, delta: dc } = diffPages(base, got);
    const inert = diff === 0 && dc === 0;
    if (inert) dead.push(label);
    console.log(`${label.padEnd(42)} ${String(diff).padStart(7)}  ${first < 0 ? '-' : String(first).padStart(5)}  ` +
                `${dc > 0 ? '+' : ''}${dc}${inert ? '   <-- NO EFFECT' : ''}`);
  }

  console.log('\nsettings with NO effect on this book:');
  if (!dead.length) console.log('  (none)');
  for (const d of dead) console.log('  ' + d);
})().catch((e) => { console.error('ERROR:', e.message); process.exit(1); });
