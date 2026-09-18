# Single-text-blit path (stage "A") — session handoff

**SUPERSEDED 2026-09-18: A is complete, enabled, and gated byte-identical over 7,568 pages.**
See `docs/stage-a-text-once.md` for the final implementation, the measured numbers, the exactness
argument, the gate protocol, and the history of what this document got wrong.

The sections below are kept only as the historical record of the 2026-09-17 session, which ended
with A implemented but 3 pages short and, as it turned out, with only half of the capture model
written (it modelled the OR of gray set-bits, but not the mode-agnostic clears). Two of its
"eliminated by measurement" claims were invalid because its patch script accumulated edits across
runs (§5).

---

## 1. What A is

`src/ko_engine_driver.h::renderPage` renders every page **three times** — once per plane
(BW, GRAYSCALE_LSB, GRAYSCALE_MSB) — re-walking the whole page layout and re-blitting every glyph
each time. Measured split for the 2,034-page text book:

```
pass1(ink) 38%  |  pass2(lsb) 28%  |  pass3(msb) 34%  |  planes+copy 0.3%
```

The "single-text-blit" path (`const bool textOnce` in `ko_engine_driver.h`) renders the page **once**
in BW mode, capturing each pixel's coverage level, then composes both gray planes from that capture
via `orCapturedGrayInto()` — skipping the two extra layout walks.

## 2. Measured state (2026-09-17, historical)

```
renderPage : 1,412.6 -> 930.7 ms   (-34%)
total      : 1,559.3 -> 1,078.0 ms
GATE       : 2,031 / 2,034 pages byte-identical
residual   : 86 pixels on 3 pages
             page  415: 48 px  x 41-255   y runs 360-364 / 507-511 / 654-658
             page  416: 32 px  x 69-206   y runs 66-70 / 259-263
             page 2033:  6 px  x 303-407  y 665-666
```

Those pixel coordinates came from a decoder that treated the 96,000-byte XTH payload as 2-bit
levels over one 384,000-pixel image. It is actually **two 1-bit planes** of 48,000 bytes
(`xtch_writer.h::addGrayPage`), so the coordinates are unreliable. 2026-09-18 rewrote the gate to
decode the planes correctly; on the clean minimal patch set the residual was **1 page / 6 bits**
(page 2033, the last page), not 3 pages / 86 pixels.

## 3. Root causes found (all verified by reading source or by measurement)

1. **`captureLevel()` had ZERO call sites.** `beginLevelCapture()` filled a 192 KB buffer with
   white (3) and nothing ever wrote a level, so the composed planes contained none of the text's
   levels. Divergence was 2,026/2,034 pages.
2. **`captureLevelPhysical` ASSIGNED while the gray passes OR.** `*p = (old & ~m) | (level << sh)`
   vs `fb[idx] |= mask` — so a pixel drawn twice with different levels kept only the last. Fixing
   this to accumulate one bit per plane contribution (bit0 = LSB-trigger v==1, bit1 = MSB-trigger
   v==2), changing the pre-fill `0xFF` → `0x00`, and making `orCapturedGrayInto`'s equality tests
   bit tests took divergence **299 → 3 pages**.
3. **Proof the capture machinery was never finished:** `captureLevelPhysical` and
   `orCapturedGrayInto` existed but nothing called the former.
4. **Missing half of the model (found 2026-09-18):** mode-agnostic writers clear plane bits, so an
   opaque rule/panel drawn over text must *take back* the captured level. See
   `docs/stage-a-text-once.md` §3(b).

## 4. Engine model (important — this was a wrong assumption for most of the session)

`GfxRenderer::drawPixel` (GfxRenderer.cpp:603) is **completely mode-agnostic**:

```cpp
if (state) { target[byteIndex] &= ~(1 << bitPosition); }  // state=true  CLEARS (ink)
else       { target[byteIndex] |= 1 << bitPosition; }     // state=false SETS
```

The gray planes are **not** separate render targets — they are the framebuffer copied out after a
mode-specific render:

```cpp
GfxRenderer.cpp:2188  copyGrayscaleLsbBuffers() { display.copyGrayscaleLsbBuffers(frameBuffer); }
GfxRenderer.cpp:2190  copyGrayscaleMsbBuffers() { display.copyGrayscaleMsbBuffers(frameBuffer); }
```

The modes control **which blits draw** (BW: `v < 3` includes black; MSB: `v == 1 || v == 2`;
LSB: `v == 1`), not what a bit means. So a plane bit means *"a draw occurred at this pixel in this
mode"* — which is what the capture's bit encoding models. This is also why the notes' claim
"the ink plane diverges too" was a **tooling artefact**: `plane_split.py` reports *derived v*, so
missing gray levels change the ink count while the BW plane is untouched.

## 5. Tooling bug that corrupted the last three measurements — READ THIS

`/tmp/ab/apply_capture2.py` **accumulates `INS.append(...)` entries across runs.** Each
`finish_a*.py` appends to it and nothing is ever removed. Consequence: the three runs that all
reported **618 differing pages with byte-identical bits** (page 34, plane1=28, plane2=0) were the
**same contamination** — the `drawPixel` choke point (edit #13) was in the file for all of them.

Corrected attribution:

| experiment | result | valid? |
|---|---|---|
| `drawPixel` choke point (unrecorded-pixels-only capture) | 618 pages | **VALID** — caused it (3 → 618) |
| 1-bit glyph branch capture at level 1 | 618 pages | **INVALID** — choke point still in file |
| dither template captures | 618 pages | **INVALID** — choke point still in file |

**Do not trust any measurement from `apply_capture2.py` until it is rebuilt clean.**

## 6. The residual's best-supported cause (2026-09-17 hypothesis — WRONG)

The `drawPixelDither` templates (GfxRenderer.cpp:1000-1021) draw **checkerboards**:

```cpp
1006  drawPixelDither<Color::Black>     -> drawPixel(x, y, true);
1011  drawPixelDither<Color::White>     -> drawPixel(x, y, false);
1016  drawPixelDither<Color::LightGray> -> drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
1021  drawPixelDither<Color::DarkGray>  -> drawPixel(x, y, (x + y) % 2 == 0);
```

This was a guess from the *shape* of the residual, not an attributed cause. The real cause (found
2026-09-18 by instrumenting `drawPixel` in a 12-pixel window) was a horizontal rule drawn over
glyph descenders: mode-agnostic clears that the OR-only capture could not model. The dither
templates do need capture calls — but as **mode-agnostic writes** (`captureAgnostic`), not as
"levels": their per-pixel parity expression decides clear vs set.

## 7. Complete drawPixel call-site inventory (from `GfxRenderer.cpp`)

```
renderCharScaled : 335 (2-bit)   357 (1-bit)      -> mode-agnostic CLEAR/SET (captureAgnostic)
renderCharImpl   : 535/538/540 (BW, CAPTURED)  547/550/552 (MSB)  557/560/562 (LSB)
                   586/589/591 (1-bit branch, no renderMode dispatch)
drawLine         : 830/837/850                    -> mode-agnostic (captureAgnostic)
dither templates : 1006/1011/1016/1021            -> mode-agnostic (captureAgnostic)
maskRoundedRectOutsideCorners : 1228-1231         -> mode-agnostic, not page-reachable
drawIcon         : 1397                           -> mode-agnostic, not page-reachable
drawBitmap       : 1498/1500/1502                 -> dispatched; images re-render anyway
drawBitmap1Bit   : 1570                           -> mode-agnostic, not page-reachable
fillPolygon      : 1632                           -> mode-agnostic, not page-reachable
```

Line numbers are from 2026-09-17 and have drifted. The 1-bit glyph branch (586-591) has no
`renderMode` dispatch, so it is mode-agnostic like `drawLine`; it is unreachable for the fonts this
pipeline loads (2-bit), so it carries no capture call. If a 1-bit font is ever used, it needs
`captureAgnostic(screenX, screenY, pixelState)` — the earlier attempt to treat it as a *level*
(level 1) was one of the unproven edits and is not in the final patch set.

## 8. The patch set that actually shipped

1. `renderCharImpl` BW branch (+ fast-path BW branch) — `captureLevel(bmpVal)` / the physical twin,
   including the synthetic-bold twin pixel.
2. Semantics fixes: pre-fill `0x00`; `captureLevelPhysical` accumulates (`level==1 -> bit0`,
   `level==2 -> bit1`); `orCapturedGrayInto` bit tests (`lv & (lsbPlane ? 0x1u : 0x3u)`).
3. `captureAgnostic` + calls in `drawLine`, the four `drawPixelDither` templates, and
   `renderCharScaled`.
4. No capture in the gray branches (dead: the gray passes never render text under this path), no
   captures at the `drawPixel` choke point, no captures for `drawBitmap*`.

## 9. Gate protocol (non-negotiable)

Only meaningful with the path **ENABLED** (`textOnce = true`): with it disabled `_levelCapture` is
false, `captureLevel` returns immediately, and the gate passes regardless of how wrong the capture
is. There is no useful "capture inert" step — except as the *baseline* build for an A/B pair, which
is how the 2026-09-18 numbers were produced.

Page-level comparison, never whole-file `cmp` (the container carries a varying millis-derived field).
XTH page payload = 96,000 B (`p1` = "light grey or black" = `ink & ~lsb`; `p2` = "dark grey or
black" = `ink & (lsb | ~msb)`); XTH records are 96,022 B with a 22-byte header. The absolute path of
the EPUB is required (the host blob store keys on the exact string).

Apply → build → gate → **auto-revert on failure**. 2026-09-18 used the `patch` tool on the real
files plus `git checkout --` for revert, which removes the patch-script failure mode of §5.

## 10. Related banked work (committed, verified)

- Mono writer: **590.3 → 231.8 ms (2.54x)** via bit-parallel output bytes with a three-layer noise
  mask and an 8x8 bit transpose (self-tested exhaustively against a naive reference). Committed.
- `-msimd128` + `-flto` are in the wasm build but emit **zero** SIMD — measured, and wasm SIMD has
  no gather instruction, so the mono writer's strided gather is not addressable by SIMD.
- Branchless compose: 1.498 → 0.219 ms/page (6.8x), 17x cumulative from the original JS.
- `build_and_deploy.sh` needs a retry loop on its live-hash check (propagation latency causes false
  FAILs).

## 11. Environment gotchas

- `env -i PATH=/opt/homebrew/bin:/usr/bin:/bin` — plain `env -i` strips PATH.
- No `timeout`; use a script file rather than `python -c` for anything containing quotes/escapes
  (two failures this session came from escape mangling in generated patches).
- `grep -c` exits 1 on zero matches, breaking `&&` chains.
- Foreground terminal timeout max 600 s.
- The host converter needs an **absolute** EPUB path, otherwise `Epub::load` fails with
  "Could not find or size META-INF/container.xml".
