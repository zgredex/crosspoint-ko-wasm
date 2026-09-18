# The spine pool: parallel export, measured

Rendering is ~85% of export time and it is per-spine work, so a *spine* is the unit of parallel work.
The container is not parallelisable, so assembly stays centralised: workers produce page records, one
assembler appends them in spine order and writes the file once.

Two invariants are enforced in code rather than hoped for:

* **Dynamic dispatch.** A worker pulls the next spine index when it finishes. Spine cost is measurably
  uneven (one spine of 60 carries ~9% of the work), so a static block split leaves every other worker
  idle behind whoever drew the heavy one.
* **Assembly in SPINE order, never completion order.** Page numbering and the chapter table are
  functions of position in the book.

Pool members restore the custom font from `customFontAsset` on load, like the export engine does — a
pooled export cannot encode spines with the wrong face.

## Byte-identity, at every pool size, in the browser

`web/demo-images.epub`, KoPub, XTCH: serial and 1/2/4/8 engines all produce `8e3eb3910d2c7953`,
13 pages, 1,249,766 bytes.

`web/demo.epub` (60 spines, 1,690 pages, 162 MB): serial, 1, 4 and 8 engines all produce
`60aa7b67441725d8ce33`, 1,690 pages, 162,310,100 bytes — and that container is also byte-identical to
the host CLI's (`scripts/verify/pool_gate.py --big`).

## Scaling, and why it is not linear

`web/demo-images.epub` — 10 spines, 13 pages. Its measured ceiling is **3.66×**, because one spine
owns 27% of the work and no amount of parallelism gets below the heaviest spine.

| engines | total ms | encode ms | assemble ms |
|---:|---:|---:|---:|
| serial | 107 (spine sum) | — | — |
| 1 | 122 | 107 | 1 |
| 2 | 74 | 58 | 1 |
| 4 | **56** | 34 | 1 |
| 8 | 75 | 35 | 1 |

1→2 is 1.84×, 2→4 is 1.71×, and **4→8 is 0.97× — slower**. Encode time goes flat (35 vs 34) because the
ceiling is now the binding constraint, while `total - encode` keeps growing with engine count (15 → 16 →
22 → 40 ms): each engine pays a book load (~5 ms here).

`web/demo.epub` — 60 spines, 1,690 pages. Ceiling **8.00×**.

| engines | total ms | encode ms | assemble ms | encode speedup |
|---:|---:|---:|---:|---:|
| 1 | 1,746 | 1,653 | 93 | 1.00× |
| 4 | 651 | 507 | 99 | 3.26× |
| 8 | **561** | 373 | 114 | 4.43× |

Here 4→8 *does* help (1.16×), because the ceiling is 8× and the work is genuinely uneven — the opposite
of the small book at the same engine count. So "more engines" is neither free nor linear, and the
correct pool size is a property of the book, not of the machine.

Three costs bound the speedup, all measured rather than assumed:

1. **The ceiling.** `sum / max(heaviest spine, sum/N)` — the heaviest spine is a hard floor.
2. **Assembly is serial.** 93–153 ms to move and append 162 MB through one engine. That is Amdahl's
   serial fraction, and it is why total speedup (3.11×) sits below encode speedup (4.43×).
3. **Per-engine setup.** Every member loads the whole book and the font before it can encode anything.
   At 8 engines on the small book, setup exceeded the parallel gain. On the big book each member holds
   a ~161 MB book in its own wasm heap — 8 members is ~1.3 GB of heaps. It ran, but that is the real
   price of the design and it is why the pool size must be chosen from the book, not from
   `navigator.hardwareConcurrency`.

## Status

The pool is **not the default**; the download path still uses the serial export. The measurements above
say the sizing policy has to be settled first — an engine count derived from the book (its spine
distribution, which the export already records per spine, and its size) rather than `hw - 1`. Shipping
the pool as the default with `hw - 1` engines would make small books *slower* than today, which the
8-engine row on the small book demonstrates.

Process a pooled export in two phases, and if the record copy becomes the bottleneck, the fix is a
spine-range pipeline between workers and the assembler — not a faster writer, since the writer is ~2%
of the work.

## Finding 1: a reused pool could hold a previous custom face

The pool key was `bookEpoch:engineCount`. An engine keeps two things as state across calls — the book
(`load`) and the font bytes (`loadFont`); everything else arrives with each `encodeSpine` call. Leaving
the font out of the key meant this sequence reused a stale pool:

```
build a 4-engine pool while font generation 1 is canonical
regenerate the font (face / size / weight change) -> generation 2
preview and the export engine get generation 2
pool is untouched, its engines still hold generation 1
pooledExportBook() -> ensurePool() matches on (bookEpoch, 4) -> REUSES it
-> a successful export with the previous face
```

Fixed by making the pool's identity what it always should have been — everything an engine holds as
state — and by destroying rather than hot-syncing:

```js
function poolIdentity(engineCount) {
  return [bookEpoch, customFontAsset ? customFontAsset.generation : 0, engineCount].join(':');
}
```

`applyCustomFont()` now calls `killPool()`. Broadcasting a regenerated face into N speculative engines
is more code and more state to reason about than rebuilding them on the next export, which already
restores the current asset through the verified path. The single export engine keeps its hot-sync: it
serves the download path and is one engine, not N.

### Measured, with the fix and without it

`web/demo-images.epub`, pool of 4. A = Courier New, B = Times New Roman, both regenerated through the
app's own UI path:

| | fixed | original code |
|---|---|---|
| generation advances on the new face | 1 → 2 | 1 → 2 |
| engines still alive after the new face | **0** (pool destroyed) | **4** (stale pool) |
| export with B | `93729a2eb590afadc79c8efa` | `087fedac07e8cd896901e6a5` = **A's bytes** |
| export with B after kill + rebuild | `93729a2eb590afadc79c8efa` (same) | n/a |
| B differs from KoPub | yes (`8e3eb3910d2c7953`) | — |

**Every row had 13 pages.** A page-count assertion — the first thing the original split-engine test
tried — passes on the broken build. The byte hash is the only check here that discriminates.

The regression lives in `scripts/verify/split_engine_probe.js` (`poolFontProbe`) and was run against
both builds: it passes with the fix and reports `B == A (stale pool reproduced)` without it. The static
half (`scripts/verify/split_engine_gate.py`) fails by name on both removed lines.
