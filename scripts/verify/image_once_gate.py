#!/usr/bin/env python3
"""Image-once gate: capturing image gray levels during the BW pass must produce exactly the planes the
three gray passes produced.

WHY: an image page used to be decoded and dithered THREE times per rendered page (BW, then LSB, then
MSB), because DirectPixelWriter re-ran the whole decode in each pass. The gray bits it sets are a pure
function of the dithered value it already computed in the BW pass (level 1 sets both plane bits, level 2
only the MSB one), so they are now recorded once and the two gray passes are composed from the recording.

That is only acceptable if the result is byte-identical, so this compares the assembled CONTAINER of the
optimized path against the same binary rendering three-pass (`--three-pass`). Container equality covers
page records, planes, index, chapter table and metadata.

What it covers: every book here x XTC/XTCH x five dither models (including the error-diffusion ones, whose
state is per-image) x AA on/off. AA-off and the reference build must not change at all, and this asserts
that too, since captureWholeGray is false for them by construction.

usage: python3 scripts/verify/image_once_gate.py [--verbose]
"""
import hashlib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOST = os.path.join(ROOT, 'build', 'ko_xtch_host')
TMP = '/tmp/ab'

BOOKS = [
    'web/demo-png.epub',          # PNG image pages
    'web/demo-images.epub',       # JPEG image pages
    'web/demo.epub',              # real novel: JPEG cover + text
    'web/demo-giant.epub',        # text only, one long spine
    'oracle/fixtures/ko-text.epub',
    'oracle/fixtures/ko-ruby.epub',
]
MODES = [([], '2-bit'), (['--1bit'], '1-bit')]
DITHERS = [([], 'blue-noise'), (['--image-dither-name', 'bayer4'], 'bayer'),
           (['--image-dither-name', 'floyd'], 'floyd'), (['--image-dither-name', 'atkinson'], 'atkinson'),
           (['--image-dither-name', 'none'], 'none')]
AA = [([], 'aa-on'), (['--no-text-aa'], 'aa-off')]


def masked_sha(path):
    with open(path, 'rb') as fh:
        d = bytearray(fh.read())
    for i in range(296, 304):        # createTime
        d[i] = 0
    return hashlib.sha256(bytes(d)).hexdigest()


def render(book, out, flags):
    r = subprocess.run([HOST, os.path.join(ROOT, book), out] + flags,
                       capture_output=True, text=True, timeout=1800)
    return r.returncode == 0, r.stderr.strip()[:160]


def main():
    verbose = '--verbose' in sys.argv
    if not os.path.exists(HOST):
        print(f'FAIL — {HOST} is not built')
        return 1

    diffs, run_fails, cases = [], [], 0
    for book in BOOKS:
        if not os.path.exists(os.path.join(ROOT, book)):
            print(f'  {os.path.basename(book)}: not present, skipped')
            continue
        for mflags, mode in MODES:
            for dflags, dn in DITHERS:
                for aflags, an in AA:
                    cases += 1
                    flags = mflags + dflags + aflags
                    ok_a, err_a = render(book, f'{TMP}/io_once.xtch', flags)
                    ok_b, err_b = render(book, f'{TMP}/io_three.xtch', flags + ['--three-pass'])
                    if not (ok_a and ok_b):
                        run_fails.append((book, mode, dn, an, err_a or err_b))
                        continue
                    same = masked_sha(f'{TMP}/io_once.xtch') == masked_sha(f'{TMP}/io_three.xtch')
                    if verbose or not same:
                        print(f'  {os.path.basename(book):22s} {mode:5s} {dn:10s} {an:6s} '
                              f'{"=" if same else "DIFF"}')
                    if not same:
                        diffs.append((book, mode, dn, an))

    print()
    print(f'  cases compared: {cases}')
    if run_fails:
        print(f'  FAIL — {len(run_fails)} case(s) did not run:')
        for f in run_fails[:5]:
            print('   ', f)
        return 1
    if diffs:
        print(f'  FAIL — {len(diffs)} case(s) differ: image-once is not byte-exact')
        for d in diffs[:10]:
            print('   ', d)
        return 1
    print('PASS — image-once is byte-identical to three-pass on every book, mode, dither and AA setting')
    return 0


if __name__ == '__main__':
    sys.exit(main())
