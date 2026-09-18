#!/usr/bin/env python3
"""§26 asset fingerprinting.

The generated engine/FreeType artifacts are content-addressed (`name.<hash8>.ext`) and published
immutable; `assets.js` maps the logical name the code uses to the fingerprinted file, and is fetched
under the same cache-bust as the worker that reads it. So a version can never mix an old JS glue
with a new .wasm: the name IS the hash.

index.html/app.js/ko.worker.js/style.css keep revalidation (`?v=` chain) — they are hand-written and
small, and revalidation costs a 304.

Usage: fingerprint_assets.py <dist-dir>
"""
import hashlib
import json
import os
import sys

GENERATED = ['ko_xtch_wasm.js', 'ko_xtch_wasm.wasm', 'ft_wasm.js', 'ft_wasm.wasm']


def main():
    dist = sys.argv[1] if len(sys.argv) > 1 else 'dist'
    mapping = {}
    for name in GENERATED:
        path = os.path.join(dist, name)
        if not os.path.exists(path):
            sys.exit('fingerprint: missing %s' % path)
        h = hashlib.sha256(open(path, 'rb').read()).hexdigest()[:8]
        stem, ext = os.path.splitext(name)
        hashed = '%s.%s%s' % (stem, h, ext)
        os.replace(path, os.path.join(dist, hashed))
        mapping[name] = hashed
        print('  %-22s -> %s' % (name, hashed))

    # the manifest is a 1-line script so a worker can importScripts it synchronously
    with open(os.path.join(dist, 'assets.js'), 'w') as fh:
        fh.write('self.__ASSETS=%s;\n' % json.dumps(mapping, separators=(',', ':')))
    # and a JSON copy for the deploy verifier / humans
    with open(os.path.join(dist, 'assets.json'), 'w') as fh:
        json.dump(mapping, fh, indent=1)
    print('fingerprinted %d generated artifacts' % len(mapping))


if __name__ == '__main__':
    main()
