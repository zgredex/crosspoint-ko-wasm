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
anything. Now `spineLabels` serves them in batches of 100 after the first frame (rAF-yielded, token
cancelled on a new book). Each label is the first EPUB TOC title assigned to that spine—the same parsed
navigation metadata used by the Korean reader and the XTC chapter table. A spine with no exact TOC item
is shown explicitly as having no TOC entry; XHTML and basenames are never promoted to chapter names.
Verified 60/60 labels and working navigation; the cost removed grows with spine count, which is the
omnibus case.

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

## Startup: what the first frame is actually made of

The profiler used to stop at the worker boundary, which left a large part of a 60 ms first page
unattributed. It now measures the whole chain from the file being chosen to the first presented frame, and
the accounting closes. Twelve MB novel, measured in the browser:

```
worker totalOpenWorkerMs   36.0   render 24.8 (+build 0.2 +compose 0.7) + engineLoad 1.8 + memcpy 1.7 + read 5.4
page side                   6.5   mainBeforeRenderMs 5.6 + canvasDrawMs 0.7 + frameWaitMs 0.2
loadCallMs                 36.3   (the round trip the page waits on)
firstFrameMs               43.2
                    36.3 + 5.6 + 0.7 + 0.2 = 42.8  ~= 43.2  (nothing unattributed)
```

So the "missing ~29 ms" was not engine initialisation. The `initWaitMs` that the read-ahead was meant to
overlap measures **0** in this flow (the module is initialised long before a file is picked), which rules
engine startup out directly rather than by subtraction. What is left is the cover **render** — the earlier ~32 ms
sum used a cached-cover figure of 16.5 ms for a page that costs ~34 ms on a cold open — plus ~10 ms of
page-thread work (title, state, canvas blit) that nothing had been measuring at all.

## One round trip instead of two

`openPreview` returns the book metadata AND the first composed frame together, so title and chapter-list
work cannot sit between the user and page 1. `load` is unchanged and still what export/pool workers use.
Verified `singleRoundTrip: true` with no fallback taken; the page keeps a two-step fallback for a worker
that predates the command.

## The read/init overlap: measured against a control, and it fires

The read starts before `await initPromise` (reading a Blob needs no engine), and `blobBootOverlapMs`
reports what that bought. Measured with a fresh worker whose module is re-fetched and re-compiled, so
init is genuinely still running when the load arrives — which is the only situation the overlap can pay in:

```
12 MB novel, one build, one flag (noEarlyRead)
  read-ahead ON    initWaitMs 5.2   blobReadMs 10.6   blobWaitAfterInitMs 5.3   blobBootOverlapMs 5.3
  read-ahead OFF   initWaitMs 4.1   blobReadMs  9.1   blobWaitAfterInitMs 9.0   blobBootOverlapMs 0.1
```

Overlap = `min(read, init)` exactly, as arithmetic predicts: 5.3 ms of a 10.6 ms read is hidden.

In the *picker* flow it is ~0, for a structural reason worth stating: this app initialises the module when
the page loads and the user picks a file seconds later, so there is nothing outstanding to overlap. The
win exists on a cold first visit, and it is bounded by `min(read, cold-init)` — not by the book size.

One interaction that had to be handled or the overlap would have destroyed the range-backed mount below:
a read-ahead on a book that is about to be range-mounted would read all 80 MB into an ArrayBuffer nobody
awaits. The threshold check needs only `blob.size` and `FileReaderSync`, both known before init, so the
decision is made up front and `earlyReadSkipReason: 'external'` records why there is no overlap rather
than leaving a missing number to be misread.

## Not ingesting the file at all: the range-backed mount {#range-mount}

The bulk path makes the whole archive resident before parsing starts: on the 80 MB fixture, **72.5 ms of
read + 17.7 ms of copy** to make 80 MB available, when page 1 touches a fraction of it. The storage can
instead mount a file whose bytes are *outside* the address space and fetch aligned 256 KiB windows on
demand. Two hosts implement one callback — `FileReaderSync` over the page's Blob in the browser, `pread`
on the host — so the same code path is gateable without a browser.

Browser, engineSpanMs (read+copy+parse), and the demand counters:

| book | bulk | range-backed | demand | crossings |
|---|---|---|---|---|
| 80 MB | 86.3 ms | **5.0 ms** | 796,920 B — **0.9 %** | 7 |
| 12 MB | 8.1 / 10.0 ms | 3.6 / 5.5 ms | 1,032,332 B — 8.2 % | 4 |

Whole open on the 80 MB fixture: `totalOpenWorkerMs` **93.3 → 17.3 ms**. The demand is ~1 MB and roughly
*constant* (0.9 % of 80 MB, 8.2 % of 12 MB), which is why the saving grows with the book while the cost
does not — and why `EXTERNAL_MIN_BYTES` is 6 MiB: measured bulk ≈ 1.15 ms/MB against a ~4-5 ms constant
mount, so the crossover is ~4 MB. Below the threshold the bulk path is already a millisecond or two, and an
aligned window can over-fetch past a small file (322 KB fixture: 118.8 %).

Caveat, measured not assumed: the **first** external mount in a page pays a one-time cost (~3.5 ms per
crossing on the earliest 12 MB run, ~0.7-1 ms warm), so a book just above the threshold can be a wash on
the first open of a session. The 76 ms win at 80 MB is far outside that noise.

Equivalence is gated, because a mount that changes the bytes would be worthless however fast it is:
`external_gate.py` requires **external == owned == copy, byte-identical, on 7 fixtures × 2 modes**, plus a
fired-instrumentation check, a window-cache-hit check, and a truncated file that must fail the mount
rather than parse short. A `pread` out of range is refused by `Blob::externalSize`, not served.

## Mono-only rendering for 1-bit output: rejected, with the evidence kept

The proposal was that a 1-bit consumer reads only the BW plane, so a 1-bit preview or export could skip
both gray passes and save a full extra image decode per page. **The premise is false in this codebase**,
in two places:

* `xtch_writer.h::addMonoPage()` requires and reads `lsb`/`msb`, turning grey pixels into ink dots and
  thinning anti-aliased ink with them;
* `wasm_api.cpp::ko_compose_rgba()` reads them too: *"A 1-bit page is never a pure function of the BW
  plane: grey pixels become ink dots."*

It was implemented anyway (an earlier batch took the premise on trust and `image_once_gate` cannot see it:
that gate compares one build against itself, and the flag is not one of its variables). Measured with the
control that does see it — 1-bit export with the gray planes dropped vs not:

```
ko-text        15,227 differing bytes
ko-textref     26,950 differing bytes
demo-png       26,950 differing bytes
2-bit                0 differing bytes (the flag is 1-bit-specific)
```

So it was not free, it was a silent corruption of every 1-bit export and preview. `ko_render_page_mode`
and the worker's `renderPageEngine(page, mono)` are **gone** rather than left as a switch, every product
path passes `false`, and the capability survives only as `ko_xtch_host --drop-gray-planes` — the negative
control that `mono_planes_gate.py` asserts is off by default and still able to demonstrate the corruption.

## The spine-href accessor: real UB, and the gate that can actually see it

`spineHref()` returned `const std::string&` bound to `epub_->getSpineItem(i).href`, but `getSpineItem()`
returns `SpineEntry` **by value** — so the reference outlived the temporary that owned the string, and
short hrefs were read out of a dead stack frame (the `䏆` / U+070F U+0006 garbage). It returns
`std::string` by value now, and `ko_get_spine_href()` assigns directly instead of through a
`const auto& item` binding that would silently reintroduce the UB if the accessor ever reverted.

Two findings about the *evidence*, because the fix is the easy part:

* **`--spine-hrefs` cannot fail.** With the reference form reinstated it printed
  `SPINE_HREFS ok — 3 and 10 spines, 0 differing/ill-formed` on every fixture. It reads the href
  immediately after the call, so it copies out of the poisoned slot while it is still intact.
  `tools/spine_href_repro.cpp` row A is that case; row B is the same read with one intervening call, and
  it corrupts. The sweep's docstring used to claim it "turns that UB into a plain failure — and under ASAN
  it is a use-after-return report". Both halves were false and are corrected in place.
* **ASAN does not work on this host.** A trivial `clang -fsanitize=address` program hangs (killed at 20 s).
  So there is no stack-use-after-return report to point at, and no ASAN evidence is claimed anywhere.

The gate is `spine_href_gate.py`: static assertions on the accessor and every caller, plus the repro
compiled at `-O0` and `-O3` where the buggy arm must corrupt (or abort inside libc++ with a corrupted
string length) and the fixed arm must hold. `spine_href_control.py` reinstates the bug and the gate fails
by name — the check that the gate is not decorative.

## Knobs added for measuring

* host `--mount-only` — isolate mount from parse (this is how the copy cost above was measured);
* host `--image-rendering N` — 0 show / 1 placeholder / 2 hidden (isolates decode from the rest of the page);
* host `--chunk N`, `--owned` and `--external` — the incremental, adopting and range-backed paths, for
  `incremental_gate.py`, the owned-vs-copied comparison and `external_gate.py`;
* host `--drop-gray-planes` — the P5 negative control; never a product path.
* worker `streamEpub`, `timing`, `openPreview`, `noEarlyRead` (the read-ahead control) and `bulkRead`
  (the whole-file-ingest control) — each exists so a claim about the open path has a baseline in the same
  build. `?noEarlyRead=1` / `?bulkRead=1` are the page-level spellings.
