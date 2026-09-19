# Defensive correctness pass

State/lifetime/error-path fixes only — no performance work, no new optimization paths. Nine stages plus the
final hardening step, each with the control that proves the gate can fail. The audit that ordered them treated "all existing gates
green" as insufficient, because the mono-only regression and the earlier equivalence-gate failures had
already shown that an equivalence gate can be green while the product is wrong. So every stage below
names the evidence that discriminates, not just the evidence that passes.

Order is the audit's: the two critical stages first, then the drift and transactional ones, then the
harness and storage hardening, then the test-only isolation.

| # | Stage | Evidence |
|---|---|---|
| 1 | Serialize book replacement; last selection wins | worker half: two `openPreview` posted un-awaited, the frame the engine ends up holding is **B**'s (`05cf2368…`) and **A**'s differs (`098e48e6…`). page half: `__koOpenSuperseded` **1**, final title B, 10 chapters |
| 2 | Fail closed on replacement failure | after a corrupt replacement: title cleared, download/prev disabled, 0 chapters, and `render` **refused** (`render failed`) — A cannot be re-rendered |
| 3 | Narrow the `openPreview` fallback | the same corrupt-EPUB failure left `__koOpenFallback` **null** — a real error is no longer rewritten as "old worker" |
| 4 | One reset for `load` and `openPreview` | `resetWorkerStateAfterBookLoad()` used by both; the omission that mattered was `appliedFont`, see the drift note in the worker |
| 5 | Transactional custom-font replacement (C++ / worker / page) | a garbage payload is refused (`epdfont load failed (bad format?)`) and the next render is **byte-identical** to the pre-attempt frame (`05cf2368…`) |
| 6 | Host owned-buffer double free | truncated EPUB through `--owned`: clean parse failure, no allocator abort, and a valid `--owned` load still succeeds afterwards |
| 7 | Pool teardown on every replacement | `killPool()` at replacement start, so a book change cannot leave N engines resident — see the gap note below |
| 8 | External storage: generic reads + short reads | clamped 4 KiB reader → **94** window fills instead of 3, container byte-identical; the clamped run FAILS with the fill loop reverted (control, see below) |
| 9 | Progressive continuation failures surface | `sectionError` message + `__koSectionError`; quiet (null) on a normal open |
| — | Hardening: gray-plane control is test-only | product `renderPage()` has no boolean; `KO_TEST_NEGATIVE_CONTROLS` is host-only; the control still changes a 1-bit container (15,227 bytes on `ko-text`) |

## What is gated, and by what

* `scripts/verify/defensive_gate.py` — host stages 6, 8 (both halves), and the hardening step (no browser).
* `scripts/verify/defensive_probe.js` — page stages 1 (both halves), 2, 3, run in a real browser.
* Stage 8a's control: reverting `Blob::fillWindow()` to "believe the first positive read" makes the clamped
  arm fail the mount and the gate reports FAIL — verified by reverting and re-running, not asserted.
* The hardening step's control is the negative control itself: if dropping the gray planes ever stopped changing a
  1-bit container, `mono_planes_gate.py` would fail, because that is the only thing keeping the rejected
  optimization visible.

## Honest gaps

* **Stage 5's "valid face A survives rejected face B"** is not exercised end to end. The test above shows a
  refused payload cannot damage live engine state, but proving A survives needs an installed valid custom
  face, which needs the in-browser FreeType conversion path. The C++ is candidate-first by construction
  (`g_customFontPath` is only retired after the candidate parses), and the gate that would cover it is the
  existing custom-font/`poolIdentity` regression in `split_engine_probe.js`.
* **Stage 9 has no fixture** where page 1 is valid and later content in the same spine fails, so the error
  path is asserted to exist and to stay quiet on success, not to fire. Building that fixture means an EPUB
  whose first XHTML is well-formed and whose later content breaks mid-spine.
* **Stage 7's gate is an invariant, not a measurement**: `killPool()` is called at replacement start; the
  browser probe does not assert `__pool.engines() === 0` immediately afterwards because the pool is only
  built by a foreground export. The static half lives in `split_engine_gate.py`.
* **`fontStamp` is not exposed** by the worker's `stats` reply, so "stamp unchanged after a refused font"
  could not be asserted from the page; it is guaranteed by the commit-after-success ordering instead.

## Why the shapes are what they are

* **A queue, not a lock.** Only book replacement is queued. Page turns, renders and spec changes still run
  immediately: they have their own guards, and queuing them behind a slow book load would stall the reader.
  `bookLoadTail.then(fn, fn)` means a failed load neither blocks nor poisons the queue.
* **Two tokens, two halves.** The worker queue makes replacements sequential; the page's `bookLoadToken`
  decides whose result may be committed. Neither alone is enough: without the queue two handlers interleave
  inside the engine, and without the token an older reply still overwrites the newer book's UI.
* **`beginBookReplacement()` before the parse, `finishBookLoad()` after.** The old shape cleared the storage
  and then reset the rest only on success, so a failure left the writer, `currentSpine`, `totalPages` and the
  driver's section describing the previous book — whose files had already been dropped.
* **The page promotes candidates last.** `currentBookBlob` is assigned on success only; assigning it at the
  start meant a book that failed to parse became "the book we can rebuild from", so the recovery path
  re-loaded the file that had just failed.
