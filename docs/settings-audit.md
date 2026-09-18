# Settings audit: does every visible control do something?

2026-09-18, against crosspoint-reader-ko **v1.5.0-ko.3**. Method:
`scripts/verify/settings_effect.js <book.epub>` flips one field at a time, exports the WHOLE
book and diffs the page payloads (a one-page probe would report false negatives — hyphenation
only matters on lines that need breaking, indent only on paragraphs that start a block).

Two books, deliberately different: `web/demo.epub` (Korean, 2034 pages) and
`dist/demo-images.epub` (English, image-heavy, 14 pages).

## Typography code is upstream's, verbatim

Our vendored engine differs from upstream in **10 files**, all in the image/decoder path
(`converters/*`, `blocks/ImageBlock.*`, `css/CssParser.cpp`). The typography path is
byte-identical:

| file | vs upstream |
|---|---|
| `Epub/ParsedText.cpp` | IDENTICAL |
| `Epub/parsers/ChapterHtmlSlimParser.cpp` | IDENTICAL |
| `Epub/blocks/BlockStyle.h` | IDENTICAL |
| `Epub/hyphenation/Hyphenator.cpp` | IDENTICAL |
| `Epub/hyphenation/HyphenationCommon.cpp` | IDENTICAL |

Defaults also match the device (`CrossPointSettings.h`): `paragraphIndent = 0`,
`characterWrap = 1`, `hyphenationEnabled = 0`, `extraParagraphSpacing = 1`,
`paragraphAlignment = JUSTIFIED`, `embeddedStyle = 1`, `textAntiAliasing = 1`,
`imageRendering = IMAGES_DISPLAY`, `lineSpacing = NORMAL` → the same 1.00/1.20/1.40 triple
`getReaderLineCompression()` returns. So the web converter's defaults produce the device's
default rendering.

## Measured effects

| setting change | Korean book | English book | verdict |
|---|---|---|---|
| `paragraphIndent` 0→1 | differs | 12/14 pages | live |
| `extraParagraphSpacing` 1→0 | differs | 12/14 (−2 pages) | live |
| `characterWrap` 1→0 | differs | 12/14 | live |
| **`hyphenation` 0→1, wrap ON** | **0/2034** | **0/14** | **INERT — hidden** |
| `hyphenation` 0→1, wrap OFF | 1125/2084 pages | 5/14 pages | live |
| `lineCompression` 1.20→1.00 / 1.40 | differs | 13/14 (−1) / 12/14 | live |
| `paragraphAlignment` → LEFT/CENTER/RIGHT | differs | 12/14 | live |
| `paragraphAlignment` → BOOK_STYLE | differs (page count +1) | **0/14** | live, content-dependent |
| `embeddedStyle` 1→0 | 2003 pages | **0/14** | live, content-dependent |
| `imageRendering` → PLACEHOLDER / SUPPRESS | 1945 pages | 9–10 pages (−3) | live |
| `textAntiAliasing` 1→0 | 2026 pages | 12/14 | live |
| `screenMargin` 5→35 | viewport 464×778 → 404×718, canvas hash changes | | live |
| `LZ4 compress` off→on | XTCH 1,345,804 B → XTZ4 322,552 B (24%) | | live |
| `Focus reading` | not in the UI; `ko_set_focus_reading()` hardcodes 0 (KO build) | | already hidden |

The two "content-dependent" rows are not dead controls: `BOOK_STYLE` means "use the book's
own alignment", which equals Justified for a book with no alignment CSS, and `embeddedStyle`
only matters if the book *has* embedded CSS. They do work on books that carry it.

## The one structurally inert setting: hyphenation

`toReaderSpec()` computes `hyphenationEnabled && characterWrap == 0`, i.e. character wrap
*masks* hyphenation — upstream's own comment says why: "Hyphenation only applies in word-wrap
mode: character wrap can break anywhere already." So with character wrap on (the KO default)
flipping hyphenation changes exactly 0 page payloads on either book. It is hidden while wrap is
on (`#hyphenationLab` + `syncDependentControls()` in `app.js`) and appears when wrap goes off,
where it does real work: 1125/2084 pages on the Korean book.

Note hyphenation is NOT dead for Korean: upstream ships patterns for
de/en/es/fi/fr/it/pl/ru/sv/uk only, but the fallback path (`includeFallback`) adds plain word
breaks for languages without a hyphenator, and CJK break points need no visible hyphen. That is
why the Korean book still moves 1125 pages — worth knowing before "hiding hyphenation for ko".

## Also fixed: custom-font knobs were inert while visible

`fontSize`, `fontWeight`, `fontHangul`, `fontIntervals`, `fontSpacePx` and the font `Name` all
funnel through `runFontConvert()`, which returns early unless a font FILE is loaded
(`if (!f || els.fontPreset.value !== 'custom') return;`). They now live in `#fontTuning`, hidden
until a file is chosen.

## Verified in a real browser, not just by reading the code

`python3 -m http.server` over `dist/`, `/?epub=demo-images.epub` (the app auto-loads via the
`epub` query param), then: hyphenation row hidden at load (wrap on) → visible after unchecking
character wrap → hidden again after `Reset defaults` (which sets `.checked` programmatically and
therefore fires no `change` event — `applyDefaults()` now re-applies the rule by hand). No JS
errors in the console.
