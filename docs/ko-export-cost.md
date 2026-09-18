# Where export time actually goes

Measured with the host CLI's own phase timers (`PROFILE` / `PROFILE2` lines in `src/host_main.cpp`),
on this machine, with the same flags the web app uses.

## The 1,690-page book (`web/demo.epub`, 162,310,292 bytes out)

```
buildSection (parse + paginate)    140.7 ms   12.8%
renderPage   (glyphs + quantize)   939.4 ms   85.2%   <- the cost
writer       (encode + record)      22.0 ms    2.0%
                                   --------
total                             1102.2 ms   0.65 ms/page
finish (container assembly)         18.7 ms
disk write                          22.0 ms
load (EPUB open)                     2.1 ms
```

## What this means for prioritisation

**The writer is 2% of export time.** Anything done inside the page encoder can therefore only ever
be a 2% story, and only for a 13-page image book does the balance shift (3.43 ms/page, where the
image decode and dither dominate `renderPage`).

So: optimisation effort belongs in `renderPage` and `buildSection` — 98% between them — and,
because both are serialised through one engine today, the largest available lever is not making any
single phase faster but running spines in parallel. The serial floor after that work is roughly
`load + finish + write ≈ 45 ms`, which is where a spine pool would land.

## A measured-instead-of-assumed rejection: direct-encode into the page record

The plan was: stop allocating two 48 KB plane temporaries per page and an extra 96 KB insert, and
encode straight into the final record; and reuse one `RenderedPage` across a spine instead of
building a fresh one per page. For 2,000 pages that removes ~6,000 vector allocations and ~192 MB of
copying — which sounds obviously worth doing.

It is not. Six interleaved runs per binary, same book, drift shared by construction:

| binary | min | median | vs baseline |
|---|---:|---:|---:|
| baseline (neither change) | 1.186 s | 1.208 s | — |
| `RenderedPage` reuse only | 1.184 s | 1.203 s | −0.4% |
| direct-encode only | 1.193 s | 1.223 s | **+1.3%** |
| both | 1.227 s | 1.274 s | **+5.5%** |

The hypothesis was that the removed 96 KB `memcpy` per page dominates. It doesn't: the copy is a
bulk `memcpy` into a buffer that is already the destination, while the replacement writes the two
planes *per row* into a buffer whose plane regions start 22 bytes and 48,022 bytes in — unaligned
against the 100-byte row stride they already have. Trading a bulk copy for strided unaligned writes
is a loss on this workload, and the phase profile says why it could never have been a win: the
writer is 20 ms of 1,102 ms.

`RenderedPage` reuse alone measures neutral (−0.4%, i.e. inside noise): the three `assign()` calls
were already cheap, and glibc's allocator reuses the freed blocks. It may still be worth revisiting
in the *browser*, where the allocator is emscripten's and allocation cost is relatively higher — but
that is a hypothesis with a measurement attached, not a change to ship on the strength of a theory.

Both changes were reverted: correctness held (page records byte-identical, whole file differing by
one byte — `createTime`), but the deliverable was "less work", and the measurement says more.
