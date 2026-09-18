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

# ko_build_info.js is REQUIRED, not optional: it is the declaration the worker reads to know which
# faces the engine carries, and a package that ships an engine without it is a package whose font
# configuration is unknown. Fingerprinting it also means a content-addressed engine can never be
# paired with a different build's declaration.
GENERATED = ['ko_xtch_wasm.js', 'ko_xtch_wasm.wasm', 'ko_build_info.js']

OPTIONAL_GENERATED_BASE = ['ft_wasm.js', 'ft_wasm.wasm']

# Optional: externalized built-in faces. Present only when a build ships a face as data instead of
# compiling it in, so their absence is not an error — but when present they MUST be fingerprinted,
# because the worker resolves them through the same manifest the engine uses.
OPTIONAL_GENERATED = ['kopub_14.epd2', 'ridibatang_14.epd2']


def main():
    dist = sys.argv[1] if len(sys.argv) > 1 else 'dist'
    mapping = {}
    # required artifacts: the site is broken without these, so their absence is fatal
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

    # optional artifacts: FreeType's wasm (present when the custom-font path is built) and the
    # externalized built-in faces, present only when a build ships a face as
    # data. Fingerprinted when present, silently skipped when not — a build that embeds every face
    # legitimately has none, and that must not be an error.
    for name in OPTIONAL_GENERATED_BASE + OPTIONAL_GENERATED:
        path = os.path.join(dist, name)
        if not os.path.exists(path):
            continue
        h = hashlib.sha256(open(path, 'rb').read()).hexdigest()[:8]
        stem, ext = os.path.splitext(name)
        hashed = '%s.%s%s' % (stem, h, ext)
        os.replace(path, os.path.join(dist, hashed))
        mapping[name] = hashed
        print('  %-22s -> %s  (external face)' % (name, hashed))

    # the manifest is a 1-line script so a worker can importScripts it synchronously
    with open(os.path.join(dist, 'assets.js'), 'w') as fh:
        fh.write('self.__ASSETS=%s;\n' % json.dumps(mapping, separators=(',', ':')))
    # and a JSON copy for the deploy verifier / humans
    with open(os.path.join(dist, 'assets.json'), 'w') as fh:
        json.dump(mapping, fh, indent=1)
    print('fingerprinted %d generated artifacts' % len(mapping))


if __name__ == '__main__':
    main()
