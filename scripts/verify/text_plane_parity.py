#!/usr/bin/env python3
"""text_plane_parity.py — TEXT RASTER PARITY, enforced per page.

The contract this encodes (docs/ko-oracle-conformance.md):

    TEXT RASTER PARITY — MANDATORY for the reference KoPub face.
      The Korean fork owns everything up to and including text rasterisation, so on a page that
      contains no images its BW / LSB / MSB planes must be BYTE-IDENTICAL to the reference's.

    IMAGE RASTER — XTCKO may intentionally differ.
      Blue-noise dithering, a different image decoder and the preview palette are XTCKO additions
      that live past the parity boundary. Image-bearing pages are reported, never judged.

Page-level granularity is why this is a separate layer from the whole-container comparison: a book with
one illustration would otherwise hide a text-plane divergence on every other page.

usage:
  text_plane_parity.py <portPlanesDir> <oraclePlanesDir> <manifest.json> [--self-control]

--self-control copies the oracle planes, flips one byte in the first image-free page, and requires the
comparison to FAIL. It exits non-zero if that perturbation is not caught, so the gate cannot pass by
comparing nothing.
"""
import json
import os
import shutil
import sys
import tempfile

PLANES = ('.bw', '.lsb', '.msb')


def page_files(directory):
    """(spine, page) -> {suffix: path}, in the engine's emission order (spine, then page)."""
    found = {}
    if not os.path.isdir(directory):
        return found
    for entry in os.listdir(directory):
        for suffix in PLANES:
            if entry.endswith(suffix):
                # the host names them p%05d_%05d.bw — the literal 'p' is part of the format
                stem = entry[: -len(suffix)]
                if stem.startswith('p'):
                    stem = stem[1:]
                try:
                    spine, page = (int(v) for v in stem.split('_'))
                except ValueError:
                    continue
                found.setdefault((spine, page), {})[suffix] = os.path.join(directory, entry)
    return found


def compare(port_dir, oracle_dir, manifest_path):
    """Returns (text_checked, image_skipped, problems[])."""
    problems = []
    pages = json.load(open(manifest_path, encoding='utf-8'))['pages']
    # The manifest lists pages in the same order the engine emits them, so the flat index is the key.
    image_pages = {i for i, p in enumerate(pages) if p.get('images')}

    port = page_files(port_dir)
    oracle = page_files(oracle_dir)

    if not port or not oracle:
        return 0, 0, [f'no plane files: port={len(port)} oracle={len(oracle)} (was --dump-planes passed?)']

    order = sorted(oracle)
    if sorted(port) != order:
        only_port = sorted(set(port) - set(oracle))[:4]
        only_oracle = sorted(set(oracle) - set(port))[:4]
        problems.append(f'page sets differ (port-only {only_port}, oracle-only {only_oracle})')

    text_checked = 0
    image_skipped = 0
    for index, key in enumerate(order):
        if index in image_pages:
            image_skipped += 1
            continue
        text_checked += 1
        for suffix in PLANES:
            a = port.get(key, {}).get(suffix)
            b = oracle.get(key, {}).get(suffix)
            if a is None or b is None:
                problems.append(f'{key}: missing {suffix} (port={a is not None}, oracle={b is not None})')
                continue
            pa = open(a, 'rb').read()
            pb = open(b, 'rb').read()
            if pa != pb:
                differing = sum(1 for x, y in zip(pa, pb) if x != y) + abs(len(pa) - len(pb))
                problems.append(f'spine {key[0]} page {key[1]}: {suffix} differs in {differing} byte(s)')
                break                                   # one broken plane per page is enough to report
    return text_checked, image_skipped, problems


def self_control(port_dir, oracle_dir, manifest_path):
    """Perturb one image-free page and require the comparison to catch it."""
    pages = json.load(open(manifest_path, encoding='utf-8'))['pages']
    image_pages = {i for i, p in enumerate(pages) if p.get('images')}
    order = sorted(page_files(oracle_dir))
    target = None
    for index, key in enumerate(order):
        if index not in image_pages:
            target = key
            break
    if target is None:
        print('  CONTROL VOID: this fixture has no image-free page to perturb', file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as td:
        shutil.copytree(oracle_dir, td, dirs_exist_ok=True)
        victim = os.path.join(td, f'p{target[0]:05d}_{target[1]:05d}.bw')     # the host's p%05d_%05d naming
        raw = bytearray(open(victim, 'rb').read())
        if not raw:
            print('  CONTROL VOID: the victim plane is empty', file=sys.stderr)
            return 1
        raw[len(raw) // 2] ^= 0x01                      # one bit, in the middle of the BW plane
        open(victim, 'wb').write(bytes(raw))

        checked, _skipped, problems = compare(port_dir, td, manifest_path)
        if checked < 1:
            print('  CONTROL VOID: nothing was checked', file=sys.stderr)
            return 1
        if not problems:
            print('  CONTROL FAILED: a flipped bit in a text page was not detected', file=sys.stderr)
            return 1
        print(f'  control ok: one flipped bit in spine {target[0]} page {target[1]} was caught '
              f'({problems[0]})')
        return 0


def main():
    if len(sys.argv) < 4:
        print('usage: text_plane_parity.py <portPlanesDir> <oraclePlanesDir> <manifest.json> '
              '[--self-control]', file=sys.stderr)
        return 2
    port_dir, oracle_dir, manifest = sys.argv[1], sys.argv[2], sys.argv[3]
    if '--self-control' in sys.argv[4:]:
        return self_control(port_dir, oracle_dir, manifest)

    checked, skipped, problems = compare(port_dir, oracle_dir, manifest)
    for p in problems[:8]:
        print(f'  {p}', file=sys.stderr)
    if problems:
        print(f'TEXT RASTER PARITY FAILED — {len(problems)} problem(s) on {checked} image-free page(s)',
              file=sys.stderr)
        return 1
    print(f'TEXT RASTER PARITY OK — {checked} image-free page(s) byte-identical across BW/LSB/MSB; '
          f'{skipped} image page(s) excluded by the contract')
    return 0


if __name__ == '__main__':
    sys.exit(main())
