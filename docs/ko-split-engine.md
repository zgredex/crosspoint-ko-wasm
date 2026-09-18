# The split engine: what it broke, and what now guards it

The preview and the export no longer share an engine (see the commit that introduced
`ko.export.worker.js`'s second instance in `web/app.js`). The split removed a pile of machinery —
interactive cooldown, preview-section invalidation after every export spine, the size preflight, the
64 MB cap — and it introduced a class of bug that has nothing to do with rendering:

> **state that one engine owns, or that the page believes about an engine, silently goes stale.**

Three of these were real. All three produced a *successful export of the wrong thing* or a failure
where success was promised, and none of them would have been caught by "does the export complete".

## Bug 1 — the custom font never reached the export engine

`applyCustomFont()` handed the converted `.epdfont` to the preview worker and kept nothing. The
export engine was then legitimately told `font: "custom"` by `readSpec()`, found no custom font in
itself, and rendered the whole book with the previously applied face — KoPub — while reporting
success.

The fix is that the **page owns the font**, exactly as it owns the EPUB Blob:

```js
let customFontAsset = null;   // { name, bytes: ArrayBuffer, generation }
```

`applyCustomFont()` keeps one canonical copy and hands each engine a `slice()` (a transferred
ArrayBuffer is detached, so the copy that goes out can never be the copy we keep), and
`loadExportEngine()` restores the font after loading the book. That last part is what makes killing
the export engine safe: no worker holds irreplaceable state.

## Bug 2 — a failed font apply still committed the spec

Making `applySpec()` throw when the requested font cannot be applied was not enough. Measured: the
`spec` call threw correctly, and **the very next `exportBook` still produced a KoPub file**, because
`applySpec()` assigned `currentSpec` at the top and the throw happened later. Every subsequent
`applySpecIfChanged()` then compared against a spec claiming `font:"custom"`, saw "no change", and
returned without applying anything.

So the commit is now transactional: the assignment stays (the steps below need it), but any failure
restores the previous spec, which makes an unmet requirement re-attempted rather than remembered as
satisfied.

```
before:  exportBook with an unavailable custom font -> 13 pages, KoPub glyphs
after:   exportBook with an unavailable custom font -> rejected:
         requested font "custom" is not available in this engine
```

## Bug 3 — killing the engine left the page believing a warm existed

`warmSpecKey` means "a converted container is sitting in the export engine, ready to download". After
a kill that is false, but the page kept the belief, so the download took the warm branch and handed
the user `✗ 내보내기에 실패했습니다` instead of a file. `killExportEngine()` now resets every page-side
belief about that engine.

The same test also exposed a fourth gap: the page retained the *picker File* but not the book itself,
so a book opened via `?epub=` had nothing to rebuild an engine from, and the recovery path silently did
nothing. `currentBookBlob` fixes that, and it is what `respawn()` prefers now too.

## Verified behaviour (browser, measured)

Book `web/demo-images.epub`, custom face Courier New (metrically very different from KoPub Batang):

| check | result |
|---|---|
| export engine told `font:"custom"`, never given one | rejected — `not available in this engine` |
| custom export | 13 pages, 1,249,766 bytes, sha `087fedac…` |
| KoPub export (same book, same spec) | 13 pages, 1,249,766 bytes, sha `8e3eb391…` |
| custom export after `killExportEngine()` + app rebuild | sha `087fedac…` — **identical to before the kill** |
| KoPub export still works (the guard is not a wall) | 13 pages |

Note what the page *count* did and did not tell us: it stayed 13 on this image-heavy book, so the
"custom preview pages == custom export pages" check the split originally suggested would have passed
**even with bug 1 present**. The discriminating check is the byte hash: custom ≠ KoPub, and
custom-after-kill == custom-before-kill.

## Guards

* `scripts/verify/split_engine_gate.py` — static, runs anywhere, exits non-zero on any violated
  invariant. It checks assertions **inside named functions**, not against the whole file: the first
  version used whole-file greps and happily passed with bug 3 re-typed into the source, because
  `warmSpecKey = null` also appears in two other places. That weakness is why the failure mode is
  tested: re-introducing bug 3 makes the gate report exactly one failure, by name.
* `scripts/verify/split_engine_probe.js` — the behavioural half, pasted into devtools (two real
  Workers and a real Blob cannot be faked without adding a browser dependency the project does not
  need). It asserts the five rows above and returns a VERDICT.

## Deferred engine preparation: measured, and not faster

Opening a book used to start both engines at once. It now renders the first preview page and prepares
the export engine at idle. This was measured, not assumed, and the measurement did **not** show the
expected win (localhost, warm cache, `web/demo-images.epub`):

| variant | first page | export engine ready |
|---|---:|---:|
| EAGER | 6.0–7.1 ms | 13.5–14.1 ms |
| DEFERRED | 7.3 ms | 25.7 ms |

Within noise on the first page (≈1 ms, in the wrong direction if anything), and deferring makes the
export engine ready ~12 ms later — irrelevant, since the warm is not scheduled until 3 s after load.
The honest statement is that the change is **not a measured win on this hardware**; it is kept because
the export engine's initialisation (a second WASM instantiate, a face fetch, a ZIP/OPF/TOC parse) no
longer competes with the work the user is waiting for, which is a claim about slower devices and real
networks that this measurement cannot demonstrate. I could not construct a cold-cache comparison
locally: the browser reused the compiled module even across ports.

## Also removed as a consequence

`refresh(true)` after an export, in both the warm-download and explicit-export paths — the export
engine never touched the preview's `section_` or frame cache, so the comment justifying it was simply
false. And the explicit export no longer calls `invalidateWarm()` + `scheduleWarm(2000)`:
`exportBook()` drops any warm on entry, so no buffer exists to keep, and re-warming meant rendering
the entire book a second time two seconds after finishing it, for settings that had not changed.
