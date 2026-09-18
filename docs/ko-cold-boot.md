# Cold boot: what the module costs, measured three ways

The browser has to fetch, decompress, validate, compile and instantiate the engine before it can draw a
page. This file records what each part of that costs, on a variant ladder that differs only in which
reader faces are compiled into the module and which arrive as EPD2 data.

Everything here is produced by `scripts/measure/cold_boot_variants.sh` and
`scripts/measure/serve_brotli.py`; the numbers are measurements, not estimates. Where a number is a
hand estimate it says so.

## The variants

| | RIDIBatang | KoPub Batang | module carries |
|---|---|---|---|
| **A** | embedded | embedded | both faces |
| **B** | *external* (fetched on selection) | embedded | KoPub |
| **C** | *external* (fetched on selection) | *external* (fetched in parallel with the module) | no reader face |

Build them with `bash scripts/measure/cold_boot_variants.sh A B C` → `/tmp/coldboot/site-{A,B,C}`.
Each variant dir is `web/` plus that variant's module, its `ko_build_info.js`, and the blobs for the
faces it does not carry — so the page, worker and CSS are identical across variants and only the
engine differs.

## What each variant costs

| variant | module raw (compiled) | module brotli (transferred) | faces fetched at boot | boot-path brotli total |
|---|---:|---:|---|---:|
| A | 6,705,020 | 1,634,353 | — | **1,634,353** |
| B | 4,479,662 | 1,365,634 | none (RIDIBatang only if selected) | **1,365,634** |
| C | **1,375,478** | **533,839** | KoPub 770,874 | **1,304,713** |

Against the state this work started from — 7,025,145 raw / 1,732,438 brotli, both faces plus
Pretendard embedded — variant C is **−80.4% raw** (the bytes the browser compiles) and **−24.7%
brotli** (the bytes it transfers).

## Boot phases, same harness, same machine

`scripts/measure/serve_brotli.py` serves brotli with `Cache-Control: no-store`, so each navigation is
a genuine cold fetch. Phases are the worker's own timings (`bootstats`); `navToReady` is
navigationStart → engine ready on the page's timeline.

| variant | factory (fetch+compile+instantiate) | init (register faces) | worker total | navToReady |
|---|---:|---:|---:|---:|
| A | 40.5 ms | 0.3 ms | 41.0 ms | 213 ms |
| B | 26.9 ms | 0 ms | 27.0 ms | 102 ms |
| C | **11.3 ms** | 4.1 ms | 15.5 ms | **74 ms** |

The interpretation that matters: the compile/instantiate step scales with the module, and the module
at C is a quarter the size — 40.5 ms → 11.3 ms for that step. The font, meanwhile, costs 4.1 ms of
`init` (it is a memcpy into the heap plus a parse) and overlaps the module fetch, which is why C is
faster end to end than a variant that carries KoPub but transfers *more* bytes than C.

Caveats, because these numbers are easy to over-read:

- **Single samples on localhost.** No WAN latency: the transfer half is ~0 ms here, so for a real
  reader the dominant term is still the 1.30 MB (C) vs 1.63 MB (A) of brotli over their connection.
  What is measured reliably is the *compile* difference and the *bytes*.
- **The compile numbers are an M2 Pro's.** Compile cost scales with module size and a phone is
  several times slower, so a 29 ms difference here is the shape of a larger one there.
- The first measurements of this used a serial HTTP/1.0 server and reported `navToReady` 732 ms with
  worker phases of 27 ms. That gap was the harness, not the app — fixed by threading the server and
  speaking HTTP/1.1 before any variant was compared.

## Correctness is not traded for the bytes

A faster module that renders differently is worthless, so each variant's output is compared against
the host CLI at the page-record level (SHA-256 per page record, read through the container's own
index):

- **C, default face (KoPub fetched in parallel):** 13 pages, every page record byte-identical to
  `ko_xtch_host --font kopub`.
- **B, RIDIBatang selected (fetched on demand):** 14 pages, every page record byte-identical to
  `ko_xtch_host --font ridibatang`.

And the lazy behaviour is verified from the server's own request log, not from intent: on a B boot
there is **no** `.epd2` request at all until the RIDIBatang control is used, at which point
`GET /ridibatang_14.epd2` appears (236,244 bytes on the wire).

## Zero-copy EPD2: the alignment decides the shape

The plan was to have `EpdFontData` point straight into the retained blob instead of copying it into
`std::vector`s. That is mostly available, but not uniformly — from the blobs themselves:

```
kopub_14.epd2        intervals at      71   aligned to 4: NO
                     glyphs    at   40008   aligned to 4: yes
                     bitmaps   at  299528   aligned to 1: yes
```

The EPD2 v2 header is 71 bytes and the interval table follows it immediately, so an
`EpdUnicodeInterval*` cast at `blob + 71` would perform unaligned 32-bit reads — tolerated by
wasm32, formally undefined in C++, and a fault on a strict-alignment target. The glyph table and the
bitmap (16,220 × 16 B and 2,868,311 B — 3.1 MB of KoPub's 3.17 MB) *are* aligned.

So the sensible zero-copy shape is: copy the 40 KB interval table (cheap, and it fixes alignment) and
point `glyph`/`bitmap` directly into the blob. That removes ~98% of the copy rather than all of it,
without a format change. Padding the header to a 4-byte boundary would make full zero-copy possible,
but that is a format revision and this is worth ~3 ms of boot and ~3 MB of transient heap — real, and
much smaller than everything above it in this file.
