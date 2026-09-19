#!/usr/bin/env python3
"""Assert that a packaged dist/ describes itself consistently.

The failure this exists to prevent: a build externalizes a reader face (so the module has no font in
it) and the package does not carry that face's EPD2 blob. Nothing else would notice — the deploy
succeeds, the site loads, and the reader gets blank pages because the engine has no font and the
worker's fetch 404s. So packaging asserts the invariant instead of hoping.

Invariant, in one line: **every face the engine does not embed must be shipped as data, and every
face it does embed must not be.**

usage: package_consistency.py <dist-dir> <embedded-face...> -- <external-face...>
       (the split is passed explicitly by build_dist.sh from the engine's own ko_build_info.json)

       Invocation matters, and getting it wrong LOOKS like a packaging failure: called with no arguments
       this exits non-zero and prints the usage text, so a batch prints

           package_consistency    FAIL
           usage: package_consistency.py <dist-dir> <embedded-face...> -- <external-face...>

       That is an invocation error, not a broken package — it cost a false alarm once. The correct call for
       the deployed variant C (no embedded face, both faces shipped as data) is

           /usr/bin/python3 scripts/verify/package_consistency.py dist -- kopub ridibatang

       or simply let scripts/build_dist.sh call it with the split it reads from ko_build_info.json.
"""
import json
import os
import subprocess
import sys

# the product's faces -> the data-asset name each is shipped as
BLOB_FOR = {'kopub': 'kopub_14.epd2', 'ridibatang': 'ridibatang_14.epd2'}

# generated artifacts that must be in every package, whatever the configuration
REQUIRED = ('ko_xtch_wasm.js', 'ko_xtch_wasm.wasm', 'ko_build_info.js')


def check(dist, embedded, external):
    """Return a list of problems; empty means the package is consistent."""
    problems = []
    manifest_path = os.path.join(dist, 'assets.json')
    if not os.path.exists(manifest_path):
        return [f'no manifest at {manifest_path}']
    assets = json.load(open(manifest_path))

    for name in REQUIRED:
        if name not in assets:
            problems.append(f'{name} is not in the manifest')
        elif not os.path.exists(os.path.join(dist, assets[name])):
            problems.append(f'{assets[name]} is named in the manifest but absent from {dist}')

    for face in external:
        blob = BLOB_FOR.get(face)
        if blob is None:
            problems.append(f'unknown face {face!r}: no data-asset name is known for it')
            continue
        if blob not in assets:
            problems.append(f'face {face} is not embedded, but {blob} is not in the manifest')
            continue
        path = os.path.join(dist, assets[blob])
        if not os.path.exists(path):
            problems.append(f'{assets[blob]} is named in the manifest but absent from {dist}')
            continue
        problems.extend(check_blob_encoding(face, path))

    for face, blob in BLOB_FOR.items():
        if face in embedded and blob in assets:
            problems.append(f'face {face} is embedded, yet {blob} was packaged anyway '
                            f'(dead weight on every visit)')
    return problems


def check_blob_encoding(face, path):
    """The package ships each face brotli-compressed, and _headers labels it Content-Encoding: br.

    These two facts have to agree or the reader gets nothing: raw bytes under a br label are decoded
    as brotli and come out as garbage, and encoded bytes with no label are handed to the font parser
    as-is and rejected. So the encoding is asserted here, where a wrong package fails the build,
    rather than discovered by a reader whose engine has no font.
    """
    try:
        with open(path, 'rb') as fh:
            head = fh.read(4)
    except OSError as exc:
        return [f'{face}: cannot read {path}: {exc}']

    if head == b'EPD2':
        return [f'face {face}: {os.path.basename(path)} is RAW EPD2 but _headers declares '
                f'Content-Encoding: br — the browser would decode raw bytes as brotli']

    try:
        out = subprocess.run(['brotli', '-d', '-c', path], capture_output=True, check=True).stdout
    except FileNotFoundError:
        return []                      # no brotli CLI: cannot verify, and silence beats a false fail
    except subprocess.CalledProcessError as exc:
        return [f'face {face}: {os.path.basename(path)} is not brotli-decodable ({exc})']

    if out[:4] != b'EPD2':
        return [f'face {face}: {os.path.basename(path)} decodes to {out[:4]!r}, not EPD2']
    return []


PAGES_FILE_LIMIT = 25 * 1024 * 1024   # Cloudflare Pages rejects any single file above 25 MiB


def oversized(dist):
    bad = []
    for root, _dirs, files in os.walk(dist):
        for f in files:
            p = os.path.join(root, f)
            if os.path.getsize(p) > PAGES_FILE_LIMIT:
                bad.append((os.path.relpath(p, dist), os.path.getsize(p)))
    return bad


def main(argv):
    if '--' not in argv:
        print(__doc__)
        return 2
    cut = argv.index('--')
    dist = argv[0]
    embedded = argv[1:cut]
    external = argv[cut + 1:]

    problems = check(dist, embedded, external)
    # A file the host will not accept must fail here, not at the edge during a deploy: Cloudflare
    # Pages rejects any single file above 25 MiB, and an oversized fixture in web/ reached it once
    # ("Pages only supports files up to 25 MiB in size: demo-large.epub is 80 MiB") after every local
    # check had passed, because nothing local knew about the host's limit.
    for name, size in oversized(dist):
        problems.append(f'{name} is {size / 1048576:.1f} MiB, above the host limit of 25 MiB')
    if problems:
        print('FAIL: the package does not describe itself consistently:', file=sys.stderr)
        for p in problems:
            print('  - ' + p, file=sys.stderr)
        return 1
    print('package consistent: engine carries [%s]; ships as data [%s]'
          % (', '.join(embedded) or 'no reader face', ', '.join(external) or 'nothing'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
