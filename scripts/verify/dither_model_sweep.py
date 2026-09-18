#!/usr/bin/env python3
"""Model sweep: export a book with each image-dither model at each tone depth and report
page-payload equality vs a baseline, determinism, and file size.

Page records: XTG = 22-byte header + 48,000-byte plane; XTH = 22-byte header + 2 planes.
Only the payload is compared - the container header carries a run-varying field.
NOTE: the host needs ABSOLUTE paths; relative ones fail with "Could not find
META-INF/container.xml" (it looks the file up through a storage shim).
Outputs are hashed then deleted: a 2,034-page novel is 195 MB per run.
"""
import hashlib
import os
import subprocess
import sys

HOST = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build/ko_xtch_host')
BOOK = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else 'dist/demo-images.epub')
OUT = '/tmp/ab/sweep'
os.makedirs(OUT, exist_ok=True)

MODELS = [
    (2, 'blue-noise'), (1, 'bayer'), (3, 'fs'), (4, 'atk'), (5, 'jjn'),
    (6, 'stucki'), (7, 'burkes'), (9, 'zhou-fang'), (0, 'none'), (8, 'ko-hash'),
]
REC_ONEBIT = 22 + 48000
REC_TWOBIT = 22 + 2 * 48000


def page_payloads(path):
    d = open(path, 'rb').read()
    out = []
    for magic, rec in (('XTH', REC_TWOBIT), ('XTG', REC_ONEBIT)):
        p = 0
        sig = magic.encode() + b'\x00'
        while True:
            q = d.find(sig, p)
            if q < 0:
                break
            out.append(d[q + 22:q + rec])
            p = q + 22
    return out


def run(tag, args):
    path = os.path.join(OUT, tag + '.bin')
    subprocess.run([HOST, BOOK, path] + args, capture_output=True)
    if not os.path.exists(path):
        return None
    pages = page_payloads(path)
    size = os.path.getsize(path)
    os.remove(path)
    return pages, size


def main():
    print('host  :', HOST)
    print('book  :', os.path.basename(BOOK), os.path.getsize(BOOK), 'B')
    for depth, flag in ((4, []), (2, ['--1bit'])):
        print('\n--- tone depth %d (%s) ---' % (depth, 'XTCH 4 tones' if depth == 4 else 'XTC 2 tones'))
        rows = []
        for code, name in MODELS:
            r1 = run('d%d-%s' % (depth, name), flag + ['--image-dither', str(code)])
            r2 = run('d%d-%s-r2' % (depth, name), flag + ['--image-dither', str(code)])
            if not r1:
                print('  %-12s FAILED' % name)
                continue
            pages, size = r1
            det = 'deterministic' if r2 and pages == r2[0] else 'NON-DETERMINISTIC'
            h = hashlib.sha256(b''.join(pages)).hexdigest()[:12]
            rows.append((name, h, len(pages), size, det))
            print('  %-12s pages %4d  %9d B  sha %s  %s' % (name, len(pages), size, h, det))
        seen = {}
        for name, h, _n, _s, _d in rows:
            seen.setdefault(h, []).append(name)
        print('  distinct outputs: %d of %d models' % (len(seen), len(rows)))
        for h, names in seen.items():
            if len(names) > 1:
                print('    identical: %s' % ', '.join(names))


main()
