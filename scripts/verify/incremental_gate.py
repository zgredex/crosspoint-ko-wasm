#!/usr/bin/env python3
"""Progressive-layout gate: a spine laid out in chunks must produce exactly what one-shot layout
produces.

WHY: the open path now starts a section with `buildSomeMore(1)` so page 1 can exist immediately, then
extends it in chunks while the reader looks at it. That is only safe if the finished section — pages,
layout manifest, chapter anchors — is identical to the one-shot build it replaced. "It looks right" is
not an acceptable answer for a pagination change, so this compares the assembled CONTAINER byte for
byte, which subsumes all of those: page records, the page index, the chapter table and the metadata.

WHAT IT COVERS: every fixture, both output modes, and chunk sizes 1/2/4/8 pages. Chunk 1 is the most
adversarial (a layout boundary after every single page).

It runs the host path (`--chunk N`), which drives the same `Section::startBuild` / `buildSomeMore`
pair the browser worker drives.

usage: python3 scripts/verify/incremental_gate.py [--big]
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
    'web/demo-png.epub',
    'web/demo-images.epub',
    'web/demo-giant.epub',      # one spine, 326 pages: the case progressive layout exists for
]
BIG = 'web/demo.epub'
CHUNKS = (1, 2, 4, 8)


def masked_sha(path):
    with open(path, 'rb') as fh:
        d = bytearray(fh.read())
    for i in range(296, 304):          # createTime is a wall clock; nothing else may differ
        d[i] = 0
    return hashlib.sha256(bytes(d)).hexdigest()


def run(book, out, flags):
    r = subprocess.run([HOST, os.path.join(ROOT, book), out] + flags,
                       capture_output=True, text=True, timeout=1800)
    return (out, None) if r.returncode == 0 else (None, r.stderr.strip()[:200])


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
    print(f'{"book":26s} {"mode":6s} {"one-shot":>11s}  chunks  ' + '  '.join(f'{c}p' for c in CHUNKS))
    for book in books:
        if not os.path.exists(os.path.join(ROOT, book)):
            print(f'{os.path.basename(book):26s} (not present, skipped)')
            continue
        for flags, mode in (([], '2-bit'), (['--1bit'], '1-bit')):
            base, err = run(book, f'{TMP}/inc_base.xtch', flags)
            if err:
                print(f'{os.path.basename(book):26s} {mode:6s} one-shot failed: {err}')
                fails += 1
                continue
            hb = masked_sha(base)
            size = os.path.getsize(base)
            cells = []
            for c in CHUNKS:
                out, err = run(book, f'{TMP}/inc_{c}.xtch', flags + ['--chunk', str(c)])
                if err:
                    cells.append(f'{c}p ERR')
                    fails += 1
                    continue
                same = masked_sha(out) == hb
                if not same:
                    fails += 1
                cells.append(f'{c}p {"=" if same else "DIFF"}')
            print(f'{os.path.basename(book):26s} {mode:6s} {size:>11,}  ' + '  '.join(cells))

    print()
    if fails:
        print(f'FAIL — {fails} comparison(s) differ: chunked layout is not equivalent to one-shot')
        return 1
    print('PASS — chunked layout is byte-identical to one-shot on every fixture, mode and chunk size')
    print('       (the browser drives the same Section API; a difference there is in the JS wiring)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
