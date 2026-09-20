# Korean-fork device profiles and landscape modes

The converter supports Xteink X4 and X3, portrait plus both landscape holding
directions, using the rendering contract at the pinned Korean fork commit
`84a39194dfce1ebd772ac9163df0a59daa0d72dc`.

## Audited source contract

The claims here come from these pinned sources:

- [`HalDisplay.cpp`](https://github.com/crosspoint-reader-ko/crosspoint-reader-ko/blob/84a39194dfce1ebd772ac9163df0a59daa0d72dc/lib/hal/HalDisplay.cpp) selects X3 with `setDisplayX3()` before display initialization.
- [`GfxRenderer.cpp`](https://github.com/crosspoint-reader-ko/crosspoint-reader-ko/blob/84a39194dfce1ebd772ac9163df0a59daa0d72dc/lib/GfxRenderer/GfxRenderer.cpp) reads physical width, height, row stride, and buffer size from the selected display at runtime and contains the four coordinate transforms.
- [`GfxRenderer.h`](https://github.com/crosspoint-reader-ko/crosspoint-reader-ko/blob/84a39194dfce1ebd772ac9163df0a59daa0d72dc/lib/GfxRenderer/GfxRenderer.h) defines one shared set of safe margins: top/right/bottom/left `9/3/3/3`.
- [`EpubReaderActivity.cpp`](https://github.com/crosspoint-reader-ko/crosspoint-reader-ko/blob/84a39194dfce1ebd772ac9163df0a59daa0d72dc/src/activities/reader/EpubReaderActivity.cpp) applies orientation before pagination and derives the viewport from the oriented margins.
- [`XtcReaderActivity.cpp`](https://github.com/crosspoint-reader-ko/crosspoint-reader-ko/blob/84a39194dfce1ebd772ac9163df0a59daa0d72dc/src/activities/reader/XtcReaderActivity.cpp) reads each page's width and height, derives plane/row/column sizes from them, and draws record coordinates directly.

The underlying SDK independently records X4 as physical `800×480` and X3 as
physical `792×528`, with runtime selection through `setDisplayX3()`:
[`FreeInk SDK supported devices`](https://github.com/Free-Ink/freeink-sdk#supported-devices).

## Geometry

| Profile | Physical panel | Portrait logical/record | Physical row bytes | Plane bytes |
|---|---:|---:|---:|---:|
| X4 | 800×480 | 480×800 | 100 | 48,000 |
| X3 | 792×528 | 528×792 | 99 | 52,272 |

The renderer does not have a separate X3 typography preset. The selected display
changes the runtime geometry and stride; the same Korean font, parser, line layout,
safe margins, and settings are then evaluated against the X3 viewport. X3-specific
checks in the reader concern panel refresh/storage paths, not page text metrics.

For physical panel width `PW` and height `PH`, the pinned renderer transforms are:

| Value | Mode | Logical screen | Physical panel coordinate |
|---:|---|---:|---|
| 0 | portrait | `PH×PW` | `(y, PH-1-x)` |
| 1 | landscape clockwise | `PW×PH` | `(PW-1-x, PH-1-y)` |
| 2 | portrait inverted | `PH×PW` | `(PW-1-y, x)` |
| 3 | landscape counter-clockwise | `PW×PH` | `(x, y)` |

The web UI exposes portrait plus values 1 and 3. Value 2 remains available in
the WASM API and host CLI to preserve the full firmware contract.

The fork rotates the hardware-safe margins with orientation, adds `screenMargin`
to logical top/right/left, and adds `max(screenMargin, 19)` to logical bottom.
At the default screen margin of 5:

| Profile/mode | Final T/R/B/L | Text viewport |
|---|---:|---:|
| X4 portrait | 14/8/22/8 | 464×764 |
| X4 landscape CW | 8/14/22/8 | 778×450 |
| X4 landscape CCW | 8/8/22/14 | 778×450 |
| X3 portrait | 14/8/22/8 | 512×756 |
| X3 landscape CW | 8/14/22/8 | 770×498 |
| X3 landscape CCW | 8/8/22/14 | 770×498 |

## Why landscape records use portrait profile dimensions

The Korean XTC parser is dimension-driven; it does not require a universal
480×800 page. The reader does, however, draw page-record `(x,y)` coordinates
without applying the EPUB reader's orientation setting. Consequently each export
uses its target profile's portrait record geometry:

1. paginate in the selected profile's logical landscape screen;
2. render through the fork's CW or CCW transform into that profile's physical panel planes;
3. encode the physical capture into X4 `480×800` or X3 `528×792` XTG/XTH records.

The record is therefore pre-rotated for the selected holding direction. The browser
preview applies the inverse selected transform and presents an upright landscape page.

## Interfaces and verification

- Web: **기기** selects X4/X3; **읽기 방향** selects portrait/landscape.
- Host: `--device x4|x3` and `--orientation portrait|landscape-cw|inverted|landscape-ccw`.
- WASM: `ko_set_device_profile(4|3)`, `ko_set_orientation(0..3)`, and dynamic geometry getters.

`scripts/verify/oracle_conformance.sh --quick --no-font-layers` compiles and runs
both the port and the pinned Korean fork. The fixture matrix includes an X3 render;
layout manifests, text planes, and decoded rasters are compared.

`node scripts/verify/orientation_preview_vs_file.js` covers both profiles, all four
orientations, and both XTG/XTH formats: 16 full-frame comparisons. It also asserts
the page-index and page-header dimensions before comparing every preview pixel with
the corresponding decoded file pixel.
