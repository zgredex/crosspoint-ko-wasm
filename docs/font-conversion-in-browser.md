# Font conversion in the browser (no local server)

The site used to dead-end its own font feature: custom TTF/OTF → `.epdfont` ran in
`server.py` + `tools/ttf_to_epdfont_fast.py` (freetype-py + fontTools), and the hosted build
had no backend, so the panel showed "conversion needs the local converter … not deployed
here". Now the conversion runs in the page's worker — nothing to install, and the font bytes
never leave the tab.

## Pieces

| piece | what it is |
|---|---|
| `tools/ft_wasm.c` | FreeType shim for the browser: `-sUSE_FREETYPE=1` (emscripten port, FreeType 2.14.3) + face stack, 26.6 metrics, pitch-stripped bitmap copy, `FT_Outline_EmboldenXY`, MM axis set |
| `scripts/build_ft_wasm.sh` | builds `web/ft_wasm.{js,wasm}` (≈ 586 KB wasm) and copies them into `dist/` |
| `web/epdfont.js` | the container packer: a line-by-line port of the Python tool — intervals, glyph selection order, metrics rounding, LUTs, header, space patch. Drives the FT shim |
| `web/ko.worker.js` | `cmd:'convertFont'` — lazily `importScripts`es the two modules (version-pinned) and returns the bytes transferable |
| `web/app.js` | `convertFontLocal()` first; the Python endpoint only as a fallback when it exists (local dev). The weight **ladder** is skipped for local conversion: it existed to pre-fill the server's memo cache, and here a full Hangul conversion is ~150-400 ms anyway |

The engine was already able to take the result: `ko_load_epdfont(data, size, name)`.

## Parity with the Python tool: byte-identical

`scripts/verify/epdfont_parity.js <font> [size] [weight] [--1bit] [--noHangul]` converts the
same font both ways and compares hash, header, every interval, every glyph record and every
bitmap byte.

| case | result |
|---|---|
| Arial 14 @400 2-bit | **IDENTICAL** (69,986 B) |
| Arial 14 @400 1-bit | **IDENTICAL** (41,679 B) |
| Arial 14 @400 `--noHangul` | **IDENTICAL** |
| Arial 14 @600 (embolden +64/64px) | **IDENTICAL** (77,398 B) |
| Arial 14 @800 (embolden +128/64px) | **IDENTICAL** (85,128 B) |
| Arial 18 @500 (embolden +41/64px) | **IDENTICAL** (115,111 B) |
| AppleSDGothicNeo 14 @400 (full Hangul, 1.93 MB) | **IDENTICAL** |
| AppleSDGothicNeo 14 @700 (embolden, 2.14 MB) | **IDENTICAL** |

Speed, same machine: Arial 17 ms vs 90 ms; Korean TTC 132 ms vs 635 ms (wasm is ~5x faster
than the Python loop; a page-level measurement in the browser worker gave 25-58 ms for Arial).

Verified in a real browser too, not only in node: the deployed page's worker converted Arial
to 75,063 B, sha256 `e8bb7dae…` — equal to the Python tool's output for the same parameters —
then `loadFont` accepted it and a re-render produced a different page than the default face
(ink 16,505 vs 10,988), i.e. the custom face really took effect.

## The one path that is NOT byte-identical: variable fonts

The server instances a variable font with fontTools (`instantiateVariableFont`, which bakes a
*new static font* with re-quantised outlines) and then rasterises; the browser cannot ship
fontTools, so it asks FreeType to interpolate the `wght` axis itself
(`FT_Set_Var_Design_Coordinates`). Both honour the requested weight, by different means:

NewYork.ttf 14 @700 (`scripts/verify/epdfont_variable_parity.js`):

```
glyph match : 546/645 (84.65%) identical
              metrics differ on 51 (7.91%), bitmaps on 99 (15.35%)
bitmap bytes: 1885 differing of 54593 (3.453%)
residual    : max advance delta 0 px, max bearing delta 1 px
              advance deltas: 0px×645
```

**Every advance is identical** — so line breaking, pagination and the page count are the same
as the server path — and the differences are ≤1 px of bearing on 15% of glyph masks, i.e.
outline rounding between two implementations of "at this weight". At the axis default (@400)
the two agree on 96.7% of glyphs, as expected. No claim of byte-parity for variable fonts;
the numbers above are what the two paths actually do.

## Two bugs this work found and fixed

1. **The local tool could not do `weight > native` at all.** `FT_Outline_Embolden` rejects
   degenerate outlines (`FT_Err_Invalid_Argument`: U+0020 has `n_points == 0`, U+2000 has a
   single point and no contour) and the tool raised on the first one — and both are in the
   default intervals, so *any* static font failed as soon as `--embolden` was used. The server
   path therefore returned a 500 for every weight above the face's native weight. Fixed in
   `tools/ttf_to_epdfont_fast.py` (rc 6 → render plainly and continue, codepoint now in the
   error message) and mirrored in `tools/ft_wasm.c`.
2. **Axis order.** `FT_Set_Var_Design_Coordinates` takes one coordinate *per axis in font
   order*, so passing a single value sets axis 0 — on NewYork (axes `opsz`,`wght`) that set the
   optical size and left the weight untouched: at @700 only 2.33% of glyphs matched the
   reference and 96% had different advances, while @400 matched 97.5% (nothing needed setting).
   `ftw_set_wght` now reads the current coordinates, replaces the `wght` entry and writes all
   axes back. After the fix: @700 went from 2.33% to **84.65%** identical.

## Cache-busting chain (all four hops)

`web/index.html` (`app.js?v=N`) → `web/app.js` (`ko.worker.js?v=N`) → `web/ko.worker.js`
(engine wasm `?v=N` **and** `FT_MODULE_VERSION`, used for `ft_wasm.js`/`epdfont.js` and
`ft_wasm.wasm`) → the modules themselves. Missing any hop pins an old converter in place.

## What the user sees

The "Hosted build: font conversion needs the local converter" note is gone (element and its
`.noBackend` styling rule removed), the upload panel is enabled on a static deploy, and after
a conversion the status line reports what happened and where it ran, e.g.
`✓ custom 14pt active (787 glyphs, 73 KB, converted in your browser in 25 ms — FreeType 2.14.3)`.
The tuning knobs (size/weight/Hangul/intervals/space) become live as soon as a file is picked,
because there is no longer a case where they are inert.
