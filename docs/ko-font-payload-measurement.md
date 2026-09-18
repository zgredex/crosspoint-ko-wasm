# Engine payload: what the built-in fonts cost

Measured 2026-09-18 on `8d06852` + the measurement toggles in this commit. The question was how many
bytes of the engine wasm are RIDIBatang, KoPub Batang and Pretendard — the generated source headers are
13.4 / 18.9 / 1.8 MB of C text, which says nothing about the compiled artifact.

## Method

Three temporary compile switches on the `koengine` target (not production architecture):

```
-DKO_EMBED_RIDI=OFF | -DKO_EMBED_KOPUB=OFF | -DKO_EMBED_PRETENDARD=OFF
```

Guarding both the enormous includes in `src/ko_engine_driver.h` and the font construction in
`src/wasm_api.cpp`. `#ifndef ... #define ... 1` defaults make an undefined macro mean "embedded", so
the native host build and any translation unit that misses the definition cannot silently lose a font.

Each variant is configured fresh in its own build directory against the emscripten 6.0.9 toolchain
(`build-perf-<name>`), built with `-j8`, then measured raw / `gzip -9` / `brotli -q 11`.

**The default build was re-verified first**: with all three switches on, the artifact is byte-identical
(`sha256 8e84c5d87dcf97e9`), so the switches are inert unless a variant is explicitly requested.

## Result

| variant | raw | gzip | brotli | brotli Δ vs baseline | brotli % of baseline |
|---|---:|---:|---:|---:|---:|
| baseline | 7,020,061 | 2,477,662 | 1,732,465 | — | 100% |
| no KoPub | 3,915,696 | 1,284,149 | **901,995** | **−830,470** | **52.1%** |
| no RIDIBatang | 4,794,796 | 1,951,555 | 1,464,466 | −267,999 | 84.5% |
| no Pretendard | 6,700,027 | 2,352,743 | 1,631,488 | −100,977 | 94.2% |
| core (all off) | 1,370,691 | 633,758 | 530,906 | −1,201,559 | 30.6% |

The individual savings are close to additive (830,470 + 267,999 + 100,977 = 1,199,446 vs 1,201,559
measured with all three off), so there is no meaningful cross-linkage between them.

## Reading it

**Raw size is dominated by font data** — with all three fonts removed the module is 1.37 MB, i.e. 80.5%
of the 7.02 MB was three font blobs. But raw bytes are not what a cold visit pays: the wasm is served
brotli-compressed, and there the picture changes completely.

Font glyph data compresses very well, so the three fonts cost **1,201,559 of 1,732,465 brotli bytes
(69.4%)** while the entire rest of the engine — renderer, layout, EPUB, dithering, the export writer —
is 530,906 bytes.

## The default face changed, and it inverts the verdict

The table above was read with RIDIBatang assumed to be the default face. It is not:
`CrossPointSettings::getReaderFontId()` returns `KOPUB_14_FONT_ID`, so **KoPub Batang is the face every
reader sees** and RIDIBatang is the optional alternative. That flips both rows:

| removal | brotli in-wasm | corrected verdict |
|---|---:|---|
| KoPub Batang | 830,470 | **keep embedded** — it is the default face; externalizing it adds a fetch to every first book |
| RIDIBatang | 267,999 | **the candidate to externalize** — optional, and most readers never select it |
| Pretendard | 100,977 | **deleted** — it is never drawn when reading a page (see below) |

### Pretendard is not drawn when reading — deleted

This file previously said "keep Pretendard embedded — the UI fallback". That was wrong twice: it is
not needed to render a page, and it is now removed from the build entirely.

The mechanism, from the pinned reference:

- `src/main.cpp:350` calls `renderer.setFallbackFont(UI_FONT_ID)`. That is a **font-ID-level**
  fallback: `GfxRenderer::getEffectiveFontId` consults it only when the *requested font id* is not
  registered. KoPub is always registered, so it never fires for a page render.
- The only **glyph-level** fallback anywhere is `src/main.cpp:162`,
  `setGlyphFallback(SYSTEM_FONT_ID, UI_FONT_ID)` — it backs an SD-card **system font** so codepoints
  that font lacks still draw. That is a device UI path, not a reading path.
- Therefore a codepoint KoPub lacks is drawn as *nothing* on the device, and the same here.

Counted over the rendered corpus (the manifests for `demo.epub`'s 1,690 pages plus the fixtures —
1,296 distinct codepoints against KoPub's 3,328 intervals) there are indeed 12 codepoints KoPub does
not cover:

```
★  ＝  │  ＋  ＿  ‐  Π  ψ  ⓒ  ä  ・  ï
```

They are **not** drawn by a fallback — that was an inference from coverage, and coverage is not
mechanism. The experiment settles it: `oracle/fixtures/ko-symbols.epub` is generated to contain
exactly those 12 codepoints, and rendering the 1,690-page book with the Pretendard binary and with
the Pretendard-free binary gives

```
layout manifests : IDENTICAL
container        : 1 byte differs out of 162,310,292 — byte 297, createTime (wall clock)
container_diff   : IDENTICAL: 1690 pages, every plane byte-equal
```

One wall-clock second, and no pixel anywhere. Deleting it removes ~101 KB brotli (331,752 bytes of
generated header) from every cold start and makes a request for a nonexistent font id fail instead
of resolving to a face that has no business rendering a page.

`ko-symbols.epub` stays in the gate's fixture list as the regression test: if any fallback is ever
consulted while reading, this is the fixture that will show it.

The arithmetic that kills the old plan, with the EPD2 blobs measured rather than assumed:

```
engine without KoPub                                    901,995
+ KoPub EPD2, brotli                                    770,874
= the bytes a default visit actually needs            1,672,869
  versus embedded                                     1,732,465
  saving                                                 59,596  ≈ 3.4%
```

and that 3.4% buys a second request, a parse, and a copy — for a *larger* combined payload than the
single embedded module. Externalizing KoPub was justified by a 48% figure that only existed while the
wrong face was the default.

The same arithmetic, for the face that *is* optional. `scripts/verify/font_payload_matrix.sh`
rebuilds all four combinations with one compiler and one flag set:

| config | raw | brotli | what it removes |
|---|---:|---:|---|
| `kopub+ridi` (**shipped**) | 7,025,145 | 1,732,438 | — |
| `kopub-only` (RIDI out) | 4,799,789 | 1,463,811 | **268,627** |
| `ridi-only` (KoPub out) | 3,920,690 | 901,818 | **830,620** |
| `neither` | 1,694,091 | 634,538 | 1,097,900 |

(The per-font savings drift a few hundred bytes from the numbers above, which were measured at an
earlier commit; the shape is unchanged.)

| face | in-wasm cost (brotli) | its own EPD2 asset (brotli) |
|---|---:|---:|
| KoPub Batang 14 | 830,620 | 770,874 |
| RIDIBatang 14 | 268,627 | 236,244 |

Read those two rows against each other and the decision makes itself:

- **Externalizing KoPub** — the *default* face — leaves a default visit fetching 901,818 + 770,874 =
  **1,672,692**, a 3.4% saving, paid for with a second request on the critical path.
- **Externalizing RIDIBatang** — the *optional* face — leaves a default visit at **1,463,811**, a
  **15.5%** saving, with **no extra request at all**: the face simply is not there, and a reader who
  selects it pays for it then. The same bytes are re-attributed in the KoPub plan, and saved here.

## Decision

**Keep both faces embedded for now. Do not ship external KoPub.** The published reason for the split
(48% off the default load) does not survive the corrected default, and the replacement figure (3.4%)
is inside the noise of a decision that adds a request to the critical path.

**RIDIBatang is the face worth externalizing**, and its EPD2 path is now proven rather than planned: the
blob reproduces the embedded face bit-for-bit (verified), externalizing it leaves the layout
byte-identical, and the raster layer finds zero differing pixels — all enforced in
`scripts/verify/oracle_conformance.sh`. It was not even exportable until the exporter's
KoPub-only constants (`U+AC00 == 437`, "kerning must be present") were replaced with per-face
expectations; RIDIBatang's U+AC00 is 444 and it ships no kerning at all (the blob is written with
`kExternalFontFlagHasKern` clear, and the verifier now checks that the wiring *matches* the embedded
face rather than that a kern matrix exists).

What still has to be measured before any of this ships is **navigation start → first readable page**,
not module size. A smaller module that must wait for a second asset can lose; a split that lets the
browser fetch the font while compiling the module can win. That benchmark has not been run, and the
splitting architecture does not exist in the shipped worker yet (it never calls
`ko_load_external_builtin_font`), so nothing is deployed that would need to be reverted.

The loading machinery already exists: runtime custom fonts go through `SdFontFamily`, and the browser
font converter already emits `.epdfont`, so the work is packaging plus a lazy-load path, not new format
support.

## Not measured

- **The variants were built and sized but never executed.** A linking build is not a working build: with
  a font compiled out its `FONT_ID` is never registered, so selecting that face must fail gracefully.
  That path is untested, and it is the first thing to check before externalizing.
- Browser cold-load was not timed per variant (instantiate ms / first-preview ms). The sizes above decide
  the KoPub question on their own; the timing would only size the win.
- `INITIAL_MEMORY` (still 64 MB) was not touched. The `36 MB → 32 MB` question only makes sense once the
  payload shrinks, and it must be measured with the memory-growth counter, not assumed.
