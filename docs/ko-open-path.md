# The open path: EPUB → first readable page

Measured, not reasoned about. Every number here came from the browser (`window.__koBookOpen`, phases
reported by the worker) or from the host CLI, and the fixtures that did not exist were built for it.

## Where the time went

`window.__koBookOpen` carries the phases. Four shapes, all measured in the same browser session:

| book | bytes | blobRead | wasmCopy | engineLoad | buildMs (layout) | renderMs | firstPageMs |
|---|---|---|---|---|---|---|---|
| `demo-giant.epub` (1 spine, 326 pages) | 3 KB | 0.6 | 0.0 | 0.2 | **48.8** | 1.7 | 68.2 |
| `demo.epub` (60 spines, novel) | 12 MB | 8.5 | 1.9 | 4.8 | 0.5 | **61.5** | 83.5 |
| `demo-png.epub` (10 spines, images) | 0.1 MB | 0.5 | 0.0 | 0.3 | 0.2 | ~1 | 3.7 |
| `demo-large.epub` (80 MB) | 80 MB | **45.0** | **7.0** | 9.1 | 0.3 | 0.4 | 67.2 |

Two fixtures were built for this because the corpus could not show the costs:

* `demo-giant.epub` — 3,200 paragraphs in **one** XHTML: one spine, 326 pages. This is the case where
  "open this EPUB" became "paginate the whole chapter before showing anything".
* `demo-large.epub` — an 80 MB stored member. The transfer and copy costs of the load path scale with
  file size, and the largest real book here is 12 MB. (Both are gitignored; neither belongs in a repo.)

## Progressive first-spine layout (the big one)

`Section` already had `startBuild` / `buildSomeMore` / `isBuildComplete` / `estimatedTotalPages`; the port
called the build-to-completion wrapper, so a 326-page first spine was laid out in full before page 1 could
exist — 48.8 ms of a 68.2 ms first page (72 %).

Now the worker starts a section with `FIRST_PAGES = 1`, answers the render, and extends it
`BUILD_CHUNK = 4` pages at a time on a macrotask yield, so navigation and settings keep running.

| | one-shot | progressive |
|---|---|---|
| `firstPageMs` | 68.2 | **18.3** |
| `buildMs` (layout before page 1 exists) | 48.8 | **6.0** |
| pages | 326 | 326 (reads `1 / 321` while building, converges) |

Navigation stays clamped to pages that EXIST (`pages`); the display uses the estimate (`total`). A
continuation stops on: a changed `sectionGen`, a changed spine, a running export, or a running warm —
otherwise it would keep laying out a section nobody is looking at, or interleave with an export's own
`buildSection`.

Gate: `scripts/verify/incremental_gate.py` — chunked vs one-shot containers are byte-identical on 8
fixtures × 2 modes × chunk sizes 1/2/4/8, including the 1,690-page book. Container equality covers page
records, page index, chapter table and metadata, so it is the strongest available form of "same layout".

NOTE: this is an *equivalence* gate (two runs of one build), not a correctness gate. It passes even when
both paths are equally wrong — which is how a stale-`size` bug in the storage rewrite got past it. The
oracle gate is the one that compares against the reference.

## Spine labels behind the first frame

The load reply built one string per spine and the page made one `<option>` per spine before showing
anything. Now `spineHrefs` serves them in batches of 100 after the first frame (rAF-yielded, token
cancelled on a new book). Verified 60/60 labels and working navigation; the cost removed grows with spine
count, which is the omnibus case.

## Adopting the EPUB instead of copying it

The browser used to read the Blob into a JS `ArrayBuffer`, copy that into the wasm heap, and then let
`HalStorage::mountBlob` copy the whole book *again* into a `std::vector`. Now the buffer the page writes
is the buffer storage adopts (`mountOwnedBlob` + `ko_load_epub_owned`), and a failed open releases the
mounted book instead of retaining it.

Measured on the 80 MB book, host, mount isolated (`--mount-only`, 4 reps):

```
copy(alloc+memcpy)  6.40 / 1.77 / 2.07 / 1.80 ms      (first rep cold)
fill+adopt          1.78 / 2.55 / 1.84 / 1.94 ms      (includes the fill)
```

**An 80 MB copy costs ~1.8–2.0 ms**, not the tens of milliseconds it is tempting to assume. So this
change is a *memory* win (80 MB of transient gone, and the storage no longer holds a second copy), and a
small time win. The claim in an earlier report that the C++ copy was a large cost was wrong, and this
measurement is what corrected it.

Both paths are byte-identical to the copying mount (`--owned` vs default on every fixture × mode, plus
the oracle gate).

### Blob streaming: benchmarked, not default

`streamEpub: true` writes the Blob into the heap in chunks and adopts it, never materialising the book in
JS. Alternating repeats, same page and blob, 80 MB:

| round | read+adopt (default) | stream+own |
|---|---|---|
| 0 (cold) | 45.0 | 27.8 |
| 1 | 18.3 | 13.3 |
| 2 | 13.6 | 25.7 |
| median | 18.3 | 25.7 |

Wall time is a **wash** once the page cache is warm (mins 13.6 vs 13.3). Streaming's advantage is memory
(no 80 MB `ArrayBuffer`), and it stays available and measurable behind `streamEpub` rather than being the
default on the strength of one sample — an earlier single-sample run suggested 93.8 ms, which repeats did
not reproduce.

## The cover image is the biggest remaining cost, and it is decoder-bound

`demo.epub`'s first spine is `cover.jpg` (549 KB). In the browser the cover render is **44.3 ms**
(uncached, min of 3, spec alternated to defeat the frame cache) — about half of that book's 83.5 ms first
page, and 72× the layout work of a text spine (0.6 ms). With the image hidden the same page renders in
**0.1 ms**, so the entire cost is the image; the dither is a small share of it.

The split was measured on the host, which is ~4× slower on this page than wasm for reasons not
investigated (both are `-O3`; only the proportions are used here):

```
cover, blue-noise dither   185.10 ms
cover, no dither           171.50 ms      -> dither share        13.6 ms   (7 %)
cover, image hidden         18.40 ms      -> decode+scale+write 153.1 ms   (83 %)
```

Host absolute numbers and browser absolute numbers are NOT interchangeable; the browser's 44.3 ms is the
one that describes the product.

The per-pixel callback is already lean (precomputed orientation transform, branchless interior, bilinear
in fixed point), so the 153 ms is libjpeg-turbo's own decode.

**A build flag cannot fix this.** The vendored libjpeg-turbo has `simd/arm i386 mips mips64 nasm powerpc
x86_64` and **no wasm path**, so `WITH_SIMD=ON` would speed the host only — while inviting exactly the
host↔wasm divergence the build deliberately avoids (`CMakeLists.txt`: "SIMD is disabled on purpose … the
host build is the byte-exact verification harness"). Verified rather than assumed: no
`wasm`/`__wasm_simd128__`/`EMSCRIPTEN` references exist anywhere under `simd/`.

So improving it means one of:

1. a JPEG decoder with a wasm SIMD path (new dependency; must be gated, and would re-baseline image
   bytes);
2. a coarser DCT-domain downscale — `chooseScaleDenom` thresholds are deliberately frozen because they
   change the sampling geometry and therefore the output;
3. caching a decoded/downscaled cover — helps repeat renders, not the first one.

All three change image bytes or add a dependency, so this is a decision, not a patch.

## Image-once: the biggest remaining first-page win

The renderer already had the text-once optimization, but images had not received it. `renderPass()` ran
three times — BW, GRAYSCALE_LSB, GRAYSCALE_MSB — and the gray passes *decode the image again*, because
`DirectPixelWriter` re-runs the whole converter. Instrumented rather than assumed, one cover render:

```
decodes: 3   images: 3                      (the page rendered three times, one decode each)
readMs 0.3 | headerMs 0.1 | decodeMs 28.4 | drawMs 20.1      renderMs 49.1
```

`DirectPixelWriter` only ever SETS gray plane bits (in GRAYSCALE_LSB/MSB it either sets the bit or
returns, never clears), and the bits are a pure function of the dithered value it already computed:
level 1 → both plane bits, level 2 → MSB only. So those bits are now captured during the BW pass
(`captureLevelPhysical`, accumulate semantics, matching `orCapturedGrayInto`), and the two gray passes are
composed from the capture instead of re-rendering the image.

```
                     before    after
decodes                   3        1
renderMs (cover)       49.1     16.5      (-66%)
decodeMs               28.4      9.2
drawMs                 20.1      7.3
text spine renderMs     0.9      0.9      (unaffected: 0 decodes either way)
first page (novel)     83.5     60.7
```

That is the audit's predicted 15–20 ms, and JPEG and PNG both go through `DirectPixelWriter`, so both are
covered by one change.

**Gate**: `scripts/verify/image_once_gate.py` — image-once vs three-pass, byte-identical containers on 6
books × XTC/XTCH × 5 dither models (including the error-diffusion ones, whose state is per-image) × AA
on/off = 120 cases. `--three-pass` (host) and `ko_set_three_pass()` (wasm) exist so one binary can render
both ways, which is what makes the gate runnable without keeping a baseline binary. Proven to
discriminate: with the capture removed but the gray passes still skipped, the gate reports the image books.

AA-off is deliberately untouched: an AA-off page must carry no text greys, and its gray passes are the
only path for images there. The reference build (`KO_ORACLE_BUILD`) never captures — the reference renderer
has no capture API, and its plain three-pass behaviour is the control.

## The earlier "decode is the bottleneck" claim was measured wrong

Two errors, both now fixed:

1. The timer labelled decode in `JpegToFramebufferConverter` began AFTER the `jpeg_read_scanlines` loop —
   it measured `jpegDrawCallback`, i.e. DRAW. Replaced with `ImagePerf` (`vendor-lib/.../converters/
   ImagePerf.h`): read / header / decode / draw, plus a decode counter, reset per render and reported to
   the browser as `timing.image`.
2. The hidden-image comparison (`--image-rendering 2`) subtracts the entire image cost — extraction, file
   read, decode, scale, dither, writes — not the codec. It says "images cost 153 ms host-side", not "the
   decoder costs 153 ms".

With the parts separated, the cover is 58 % decode and 41 % draw per decode, and the conclusion that
mattered was neither: the real defect was doing either of them three times.

Note for future comparisons: the port's container differs from the `KO_ORACLE_BUILD` container on **JPEG**
image pages and did so BEFORE image-once (checked with the pre-change driver restored), while PNG pages
match. That is consistent with the gate's own design — layout manifests are compared byte-exactly against
the reference, raster content perceptually — so the correct control for image work is the port's own
pre-change build, not the oracle build.

## Knobs added for measuring

* host `--mount-only` — isolate mount from parse (this is how the copy cost above was measured);
* host `--image-rendering N` — 0 show / 1 placeholder / 2 hidden (isolates decode from the rest of the page);
* host `--chunk N` and `--owned` — the incremental and adopting paths, for `incremental_gate.py` and the
  owned-vs-copied comparison;
* worker `streamEpub: true` and the `timing` command.
