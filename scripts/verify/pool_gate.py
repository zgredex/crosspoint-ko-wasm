#!/usr/bin/env python3
"""Pool gate: the pooled (per-spine encode + central assembly) export must be byte-identical to the
serial export.

WHY THIS IS THE RIGHT GATE: the pool changes WHERE pages are encoded, never what a page is. So the
only acceptable difference between the two paths is the container's createTime. If a page record, a
chapter, an index entry or the metadata block differs at all, the pool is wrong — and the way to find
out is to run both and compare, not to reason about the assembly code.

WHAT IT COVERS: every fixture, in both output modes (1-bit XTG and 2-bit XTH), plus the large
60-spine book when it is present. The small fixtures catch ordering and chapter-table bugs; the large
one catches anything that only appears with many spines.

It runs the HOST pool path (`--pool`), which is the reference implementation of the coordinator the
browser will use: spine-local writers, records appended in spine order by one assembler, chapters from
the shared buildChapters(). If this passes and the browser later disagrees, the difference is in the
JS wiring, not in the assembly algorithm — which is exactly the kind of thing worth being able to
separate.

usage: python3 scripts/verify/pool_gate.py [--big]
"""
import hashlib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOST = os.path.join(ROOT, 'build', 'ko_xtch_host')
TMP = '/tmp/ab'

FIXTURES = [
    'oracle/fixtures/ko-text.epub',
    'oracle/fixtures/ko-ruby.epub',
    'oracle/fixtures/ko-mixed.epub',
    'oracle/fixtures/ko-symbols.epub',
    'oracle/fixtures/ko-chapters.epub',
    'web/demo-png.epub',
    'web/demo-images.epub',
]
BIG = 'web/demo.epub'          # 1,690 pages / 60 spines


def masked_sha(path):
    """SHA-256 with createTime zeroed. createTime is the ONLY field two runs may differ in: it is a
    wall clock. Everything else is a decision the export made."""
    with open(path, 'rb') as fh:
        d = bytearray(fh.read())
    for i in range(296, 304):
        d[i] = 0
    return hashlib.sha256(bytes(d)).hexdigest(), len(d)


def run(book, out, flags):
    r = subprocess.run([HOST, os.path.join(ROOT, book), out] + flags,
                       capture_output=True, text=True, timeout=900)
    if r.returncode != 0:
        return None, r.stderr.strip()[:200]
    return out, None


def main():
    if not os.path.exists(HOST):
        print(f'FAIL — {HOST} is not built')
        return 1

    books = list(FIXTURES)
    if '--big' in sys.argv:
        books.append(BIG)
    else:
        print('(add --big to include the 1,690-page book)\n')

    fails = 0
    print(f'{"book":34s} {"mode":7s} {"serial":>11s} {"pooled":>11s}  verdict')
    for book in books:
        if not os.path.exists(os.path.join(ROOT, book)):
            print(f'{os.path.basename(book):34s} {"-":7s} {"(not present, skipped)":>24s}')
            continue
        for flags, mode in (([], '2-bit'), (['--1bit'], '1-bit')):
            ser, err = run(book, f'{TMP}/gate_ser.xtch', flags)
            if err:
                print(f'{os.path.basename(book):34s} {mode:7s} serial failed: {err}')
                fails += 1
                continue
            poo, err = run(book, f'{TMP}/gate_pool.xtch', flags + ['--pool'])
            if err:
                print(f'{os.path.basename(book):34s} {mode:7s} pooled failed: {err}')
                fails += 1
                continue
            hs, ns = masked_sha(ser)
            hp, np_ = masked_sha(poo)
            same = hs == hp
            if not same:
                fails += 1
            print(f'{os.path.basename(book):34s} {mode:7s} {ns:>11,} {np_:>11,}  '
                  f'{"IDENTICAL" if same else "DIFFER " + hs[:10] + " vs " + hp[:10]}')

    print()
    if fails:
        print(f'FAIL — {fails} comparison(s) differ; the pooled path is not equivalent')
        return 1
    print('PASS — pooled export is byte-identical to serial on every fixture and mode')
    print('       (the browser pool must reproduce this; if it does not, the difference is in the JS wiring)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
