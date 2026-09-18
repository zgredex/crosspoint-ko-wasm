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

Weighted against the decision thresholds:

| removal | brotli saving | verdict |
|---|---:|---|
| **KoPub Batang** | **830,470** (>500 KB) | **externalize** — it is optional and not the default face |
| RIDIBatang | 267,999 (<1 MB) | keep embedded — it IS the default face; externalizing would add a round trip to every first book |
| Pretendard | 100,977 (<200 KB) | keep embedded — it is the UI fallback and cheap |

## Decision

Externalize **KoPub Batang only**, as a fingerprinted `.epdfont` fetched when the face is selected. The
expected effect is **brotli 1,732,465 → 901,995 (−48%)** and raw 7.02 → 3.92 MB, with the default face
and the UI fallback still embedded, so no first-book path gains a network round trip.

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
