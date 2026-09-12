# Third-party components and attribution

This repository is a **WASM port + web front-end** for the CrossPoint-KO e-reader
engine. Almost all of `vendor-lib/`, `third_party/`, and `stubs/` is third-party
work. Licenses of the upstream projects are reproduced/preserved in-tree where
provided; this file records the provenance.

## The engine (this port is derived from it)

| Project | Author / org | License | Notes |
|---|---|---|---|
| [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) | CrossPoint Reader project | **MIT** | Upstream e-reader firmware for the Xteink X4 (ESP32-C3). Root of the lineage. |
| [crosspoint-reader-ko/crosspoint-reader-ko](https://github.com/crosspoint-reader-ko/crosspoint-reader-ko) | **Eunchurn Park** | **MIT** | Korean fork (v1.5.0-ko.3 lineage): KO typography build, RIDIBatang reader face, Korean UI. `vendor-lib/` is taken from this fork. |

`vendor-lib/` is a vendored copy of the KO fork's libraries and is their
copyright; the MIT terms above apply to it. The WASM port itself
(`src/wasm_api.cpp`, `src/ko_engine_driver.h`, `src/xtch_writer.h`), the
`stubs/` shims, the `server.py` conversion API, and everything under `web/`
are additions of this repository.

## Bundled libraries

| Component | Location | Author | License |
|---|---|---|---|
| JPEGDEC | `third_party/JPEGDEC/` | Larry Bank (bitbank2) | MIT |
| PNGdec | `third_party/PNGdec/` | Larry Bank (bitbank2) | MIT |
| LZ4 | `third_party/lz4/` | Yann Collet | BSD 2-Clause |
| Expat | `vendor-lib/expat/` | Expat maintainers | MIT |
| miniz | `vendor-lib/miniz/` | Rich Geldreich et al. | MIT |
| uzlib | `vendor-lib/uzlib/` | Paul Sokolovsky et al. | Zlib |
| MiniBidi | `vendor-lib/MiniBidi/` | (see file headers) | see file headers |

## Fonts

Fonts are redistributed as **generated 2-bit bitmap data** (C headers under
`vendor-lib/EpdFont/builtinFonts/`). Font licensing is independent of this
repository's MIT license — the fonts' own terms apply to the glyph data.

| Font | Form in repo | License |
|---|---|---|
| Noto Sans / Noto Serif / Noto Sans Arabic / Noto Sans Hebrew | `.ttf` sources + generated headers | SIL OFL 1.1 (`OFL.txt` in-tree) |
| OpenDyslexic | `.otf` sources | SIL OFL 1.1 (`OFL.txt` in-tree) |
| Ubuntu | `.ttf` sources | Ubuntu Font License 1.0 (`UFL.txt` in-tree) |
| Pretendard | generated header only | SIL OFL 1.1 (upstream: Kil Hyung-jin) |
| RIDIBatang | generated header only | RIDI Corp. font terms — see the upstream KO fork |
| KoPub Batang | generated header only | KoPub font terms (Korea Publishers Association) — see the upstream KO fork |

The Korean reader faces (RIDIBatang, KoPub Batang) and Pretendard are carried
over from the upstream KO fork; consult that project and each font's own
license before redistributing the generated glyph data.

## Not included

`web/demo.epub` (a commercial Korean edition) is intentionally **not** tracked
by this repository (see `.gitignore`).
