# Stage A — single-text-blit path: COMPLETE and gated byte-identical

2026-09-18. Supersedes `ko-capture-session-handoff.md` (written 2026-09-17 when A was 3 pages
short). Code: `vendor-lib/GfxRenderer/GfxRenderer.{h,cpp}` (capture), `src/ko_engine_driver.h`
(`textOnce = true`).

## 1. What A is

`renderPage` used to render every page **three times** — once per plane (BW, GRAYSCALE_LSB,
GRAYSCALE_MSB) — re-walking the whole page layout and re-blitting every glyph each time. Measured
split on the 2,034-page text book: `pass1(ink) 38% | pass2(lsb) 28% | pass3(msb) 34%`.

With the path enabled the page is rendered **once** in BW mode (text + images), each pixel's text
coverage level is recorded into a panel-wide scratch buffer, and both gray planes are composed
from that recording (`orCapturedGrayInto`) instead of re-walking the layout twice more. The gray
passes still run, but images-only (`page->renderImages`), because images must be re-rendered in
each mode.

## 2. Measured result

OFF = same tree with `textOnce = false` (the capture is then inert: `beginLevelCapture` is never
called, so every capture call is a no-op). That binary reproduces the pre-existing pristine
reference of the text book **byte-for-byte** (0/2034 differing), so the OFF column is a valid
baseline. All runs on the same machine, same session, M2 Pro.

| book | pages | renderPage OFF → ON | total OFF → ON | gate |
|---|---|---|---|---|
| `web/demo.epub` (Korean text) | 2034 | 1447.2 → 923.6 ms (**−36.2%**) | 1604.8 → 1074.1 ms | **0 / 2034** |
| `dist/demo-images.epub` (images) | 14 | 30.6 → 30.3 ms (−1%) | 31.6 → 31.3 ms | **0 / 14** |
| Holly — Stephen King | 1792 | 771.3 → 531.8 ms (**−31.0%**) | 893.9 → 650.2 ms | **0 / 1792** |
| Innowatorzy (PL, illustrated) | 2915 | 2121.7 → 1700.3 ms (**−19.9%**) | 2333.5 → 1908.5 ms | **0 / 2915** |
| Teren Mikami Vol. 1 (image-heavy) | 814 | 880.4 → 806.0 ms (−8.4%) | 931.4 → 856.9 ms | **0 / 814** |

**7,568 pages gated byte-identical across 5 books.** Image-heavy books gain less because their
cost is image decoding, which A does not touch — the gain tracks the text share of the page.

## 3. Why it is exact — the two invariants

The capture must reproduce, per pixel, what the two gray passes would have put into their plane
bits. A gray pass does exactly two kinds of thing:

**(a) It ORs set-bits.** Mode-dispatched blits (glyphs, and images through `DirectPixelWriter`)
choose which pixels to SET per mode. For text that classification is a pure function of the
coverage level `v`, so `captureLevel` records it: **bit 0 = "the LSB pass sets this pixel too"
(v == 1, dark gray), bit 1 = "only the MSB pass does" (v == 2, light gray)**; black (v == 0) and
white (v == 3) set no plane bit in either pass and contribute nothing. Levels are **accumulated**
(`|=`), never assigned, because two overlapping glyphs with different levels must keep *both*
plane contributions — the plane ORs, so the capture must too.

**(b) It CLEARS bits with mode-agnostic writes.** `drawPixel` is completely mode-agnostic
(`state=true` clears the bit, `state=false` sets it), so any primitive with no `renderMode`
dispatch performs the *identical* write in every pass — and a clear there wipes a bit that an
earlier glyph set. That is `captureAgnostic(x, y, state)`, which **overwrites** the recorded
level: `true` → level 0 (no contribution), `false` → level 1 (both planes). Wired into the four
mode-agnostic primitives reachable from page rendering — `drawLine` (rules, link underlines), the
`drawPixelDither` templates behind `fillRect`/`fillRectDither` (shaded panels, frames), and
`renderCharScaled` (sup/sub glyphs) — with the dither templates passing their own per-pixel
parity expression, so a checkerboard fill is modelled pixel-exactly.

Everything else in the gray passes is re-drawn faithfully (images never *clear* plane bits in a
gray pass — they either set or do nothing), so OR-ing the captured text bits into the real plane
afterwards reproduces the three-pass output.

**Why (b) was the missing half.** With only (a) implemented the gate was 1 page / 6 bits short:
the last page of the text book has a full-width horizontal rule (`Page.cpp:88`,
`drawLine(..., thickness=2, state=true)`) drawn over the descenders of the line above it. In the
real gray passes the rule clears the plane bits the glyphs had just set; the OR-only capture kept
them, flipping 6 pixels from black (v=3) to dark/light gray. The old note calling the residual
"thin 1px differences in a regular grid → dither templates" was a guess about shape, not an
attributed cause; the actual cause was found by instrumenting `drawPixel` for the 12-pixel window
and reading the op sequence: `CAP x=303 y=665 level=2` immediately followed by
`DP x=303 y=665 state=1`.

## 4. Gate protocol (re-verify with these)

```bash
cd ko-wasm
cmake --build build -j8                       # host build
./build/ko_xtch_host <abs-path>.epub out.xtch # NOTE: absolute path required (mountBlob key)
python3 /tmp/ab/gate_xtch.py ref.xtch out.xtch [--detail N]
```

* Compare **page records**, never whole files: the container carries a millis-derived field.
  XTH page payload = 96,000 B = two 1-bit planes (`p1 = ink & ~lsb`, `p2 = ink & (lsb | ~msb)`,
  see `xtch_writer.h::addGrayPage`), column-major over the logical 480×800 grid:
  byte `i` → logical `x = 479 - i/100`, `y = 8*(i%100) + (7 - bit)`.
* The gate is only meaningful with the path **enabled**; with `textOnce = false` the capture is
  inert and cannot fail.
* For a baseline pair, build ON and OFF binaries once (`cp build/ko_xtch_host /tmp/ab/host_on`,
  flip the flag, rebuild, `cp … /tmp/ab/host_off`) and run both over each book.
* `--detail` decodes differing plane bits into logical pixel coordinates.

## 5. History worth keeping (from the 2026-09-17 session)

* **`captureLevel()` had zero call sites.** `beginLevelCapture()` filled its buffer and nothing
  ever wrote a level: divergence 2,026/2,034 pages. The capture machinery had never been wired up.
* **Pre-fill must be level 0, not white.** The gray passes only ever OR set-bits, so an untouched
  pixel must contribute nothing.
* **The engine model.** `GfxRenderer::drawPixel` is mode-agnostic; the gray "planes" are the
  framebuffer copied out after a mode-specific render (`copyGrayscaleLsbBuffers/MsbBuffers`). Modes
  select *which blits draw*, not what a bit means. So `plane_split.py`'s "derived v" reading is a
  reconstruction, and the old claim "the ink plane diverges too" was a tooling artefact — the BW
  plane is a straight copy of pass 1 and cannot be affected by the capture.
* **Tooling bug that corrupted three measurements:** `/tmp/ab/apply_capture2.py` accumulated
  `INS.append(...)` entries across runs, so three "independent" experiments were the same
  contamination. Lesson: patch scripts must be rebuilt from source each time, and a gate that
  returns bit-identical failures for different edits is reporting on the script, not the edit.
  This session used the `patch` tool against the real files plus `git` for revert instead, and
  re-verified the baseline (0/2034) before trusting any measurement.
* **Do not add a capture at the `drawPixel` choke point.** Attempted on 09-17 and it produced 618
  diverging pages: it recorded a *set* for every unrecorded pixel with no regard for `state`, so
  every ink-clearing primitive (rules, sup/sub, frames) gained a phantom gray bit. The `state`-aware
  `captureAgnostic` in §3(b) is the correct form of that idea.

## 6. Not done here (future work)

* `maskRoundedRectOutsideCorners`, `drawIcon`, `fillPolygon`, `drawBitmap1Bit` are also
  mode-agnostic writers. They are **not** reachable from EPUB page rendering
  (`vendor-lib/Epub/**` calls only `drawText`, `drawLine`, `fillRect`), so no capture call was
  added. If any of them ever becomes page-reachable, it needs `captureAgnostic` too, or pages
  using it will diverge exactly the way the rule did.
* Stage B/C (multithreading, see `ko-stageC-multithreading-plan.md`) untouched.
