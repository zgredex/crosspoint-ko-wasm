#!/usr/bin/env python3
"""Controls for the raster layer: it must catch what a reader would see and nothing else.

A tolerance-based comparison is only worth having if the tolerance is in the right place. Three
mutants are manufactured from a real container, each a specific failure mode or a specific
harmless difference, and each is required to land on the right side:

  shift-1px      the whole page moved one column → content moved → FAIL (this is
                 "reference word x = 147, XTCKO word x = 150" seen at the pixel level)
  quantize-1lvl  a few hundred isolated pixels change exactly one level → PASS (grayscale
                 quantization / halftone phase, no typography change)
  tone-2lvl      a few hundred pixels change two levels → FAIL (a visible tone step)

Building mutants from real output keeps this honest: the input is what the engine actually
emits, not a synthetic image chosen to make the thresholds look good.

usage: raster_controls.py <container.xtch> [--work DIR]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import container_diff as cd  # noqa: E402
import raster_diff as rd  # noqa: E402

W, H = 480, 800
XTH = 22 + 2 * 48000


def mutate_shift(rec):
    """Move the page one column to the right: XTH columns are 100 bytes each."""
    out = bytearray(rec)
    for base in (22, 22 + 48000):
        plane = bytearray(rec[base:base + 48000])
        shifted = bytearray(48000)          # column 0 (rightmost) fills with 0, like a blank edge
        shifted[100:] = plane[:-100]
        out[base:base + 48000] = shifted
    return bytes(out)


def _flip(rec, count, plane, seed=12345):
    """Flip `count` pixels in the given XTH plane.

    Plane layout matters and getting it backwards silently changes the control: the record is
    header, then plane 1, then plane 2, and a logical level is (plane1 << 1) | plane2 — so
    flipping a bit in plane 1 moves the level by TWO and flipping plane 2 moves it by ONE.
    The caller asserts the measured histogram, so a mis-specified mutant fails as a control
    error instead of being compared as if it meant something.
    """
    out = bytearray(rec)
    base = 22 if plane == 1 else 22 + 48000
    x = seed
    touched = 0
    while touched < count:
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        off = x % 48000
        bit = (x >> 8) % 8
        out[base + off] ^= (1 << bit)
        touched += 1
    return bytes(out)


def level_histogram(a_levels, b_levels):
    hist = {}
    for a, b in zip(a_levels, b_levels):
        if a != b:
            hist[abs(a - b)] = hist.get(abs(a - b), 0) + 1
    return hist


def splice(src_path, dest_path, page_index, mutated_record):
    """Replace ONE page record in a copy of the container, in place.

    Copying the container and splicing keeps the header, index, metadata and every other page
    byte-identical, so the only variable in the comparison is the mutation. (Constructing a
    one-page container instead changes the page count, which raster_diff correctly refuses to
    compare at all — that was the first version of this control's bug.)
    """
    data = bytearray(open(src_path, 'rb').read())
    index_offset = int.from_bytes(data[0x18:0x20], 'little')
    e = index_offset + page_index * 16
    off = int.from_bytes(data[e:e + 8], 'little')
    size = int.from_bytes(data[e + 8:e + 12], 'little')
    if len(mutated_record) != size:
        raise SystemExit(f'mutation changed the record size ({len(mutated_record)} vs {size})')
    data[off:off + size] = mutated_record
    with open(dest_path, 'wb') as fh:
        fh.write(data)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    work = sys.argv[sys.argv.index('--work') + 1] if '--work' in sys.argv else '/tmp/ko-raster-controls'
    if len(args) != 1:
        print(__doc__)
        return 2
    pages, _hdr = cd.container(args[0])
    if not pages or len(pages[0]) != XTH:
        print(f'{args[0]}: expected at least one 2-bit (XTH) page')
        return 2
    os.makedirs(work, exist_ok=True)

    ref_levels = rd.decode_page(pages[0])
    cases = []

    # 1. the page moved one column → content moved
    moved = mutate_shift(pages[0])
    cases.append(('shift-1px', moved, 'FAIL', {}))

    # 2. one-level quantization on 300 scattered pixels (plane 2 = the low bit)
    q1 = _flip(pages[0], 300, plane=2)
    hist1 = level_histogram(ref_levels, rd.decode_page(q1))
    cases.append(('quantize-1lvl', q1, 'PASS', hist1))

    # 3. two-level tone change on 300 pixels (plane 1 = the high bit)
    t2 = _flip(pages[0], 300, plane=1, seed=999)
    hist2 = level_histogram(ref_levels, rd.decode_page(t2))
    cases.append(('tone-2lvl', t2, 'FAIL', hist2))

    # The mutants must be the failure modes they claim to be. A control whose input does not
    # actually contain the thing it is testing certifies nothing, so the measured level deltas
    # are asserted: {1: 300} for quantization, {2: 300} for the tone step.
    if hist1 != {1: 300}:
        print(f'CONTROL INVALID: quantize-1lvl changed levels {hist1}, expected {{1: 300}}')
        return 1
    if hist2 != {2: 300}:
        print(f'CONTROL INVALID: tone-2lvl changed levels {hist2}, expected {{2: 300}}')
        return 1

    failures = 0
    for name, rec, want, hist in cases:
        path = os.path.join(work, f'{name}.xtch')
        splice(args[0], path, 0, rec)
        rc = os.system(f'python3 {HERE}/raster_diff.py {args[0]} {path} > {work}/{name}.log 2>&1')
        verdict = 'PASS' if rc == 0 else 'FAIL'
        ok = verdict == want
        if not ok:
            failures += 1
        detail = f'level deltas {hist}' if hist else 'whole-page displacement'
        print(f'  {"OK " if ok else "x  "} {name:14s} expected {want}, got {verdict}  ({detail})')
        if not ok:
            with open(f'{work}/{name}.log', encoding='utf-8') as fh:
                for line in fh.read().splitlines()[-6:]:
                    print(f'        {line}')
    print()
    if failures:
        print(f'FAIL — {failures} control(s) landed on the wrong side; the raster layer is '
              f'either blind or over-strict')
        return 1
    print('PASS — the raster layer catches movement and visible tone steps, and tolerates '
          'one-level quantization')
    return 0


if __name__ == '__main__':
    sys.exit(main())
