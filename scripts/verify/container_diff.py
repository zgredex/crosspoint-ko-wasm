#!/usr/bin/env python3
"""Per-page, per-plane comparison of two XTC/XTCH containers.

The container-level byte comparison answers "are these the same file"; this answers
"which page, which plane, how many bytes" — the difference between a gate that fails and
a gate that tells you why. Both records are parsed by their documented layout:

  XTG (1-bit): 22-byte header + 48000 bytes, logical row-major.
  XTH (2-bit): 22-byte header + 2 x 48000 bytes (plane 1, plane 2), physical column-major.

usage: container_diff.py <a> <b> [--limit N] [--quiet]
"""
import sys
import hashlib

XTG_REC = 22 + 48000
XTH_REC = 22 + 2 * 48000


def container(path):
    """(pages, header) parsed through the container's own index — not by scanning for
    magic bytes, which desynchronises the moment dithered pixel data happens to contain
    'XTG\\x00'. Layout is the writer's (src/xtch_writer.h finish()): 56B header,
    256B metadata, 96B/chapter, then a 16B/page index (u64 offset, u32 size, u16 w, u16 h),
    then the page records."""
    data = open(path, 'rb').read()
    if len(data) < 56:
        raise SystemExit(f'{path}: too short to be a container')
    magic = data[0:4]
    page_count = data[6] | (data[7] << 8)
    index_offset = int.from_bytes(data[0x18:0x20], 'little')
    data_offset = int.from_bytes(data[0x20:0x28], 'little')
    meta = data[56:56 + 256]
    title = meta[0:128].split(b'\x00')[0].decode('utf-8', 'replace')
    # createTime is a Unix timestamp: it MUST differ between two runs made at different
    # seconds, and it must not be reported as a content difference.
    create_time = int.from_bytes(meta[0xF0:0xF4], 'little')
    pages = []
    offsets = []
    for i in range(page_count):
        e = index_offset + i * 16
        off = int.from_bytes(data[e:e + 8], 'little')
        size = int.from_bytes(data[e + 8:e + 12], 'little')
        w = int.from_bytes(data[e + 12:e + 14], 'little')
        h = int.from_bytes(data[e + 14:e + 16], 'little')
        # Validated rather than sliced-and-hoped. A page index that runs past the buffer used to
        # return a short record, which compares unequal to a full one and reports as a content
        # difference; the real fault is a broken container, and it should say so.
        if size <= 0 or off < index_offset + page_count * 16 or off + size > len(data):
            raise SystemExit(f'{path}: invalid page index {i}: offset {off} size {size} '
                             f'(container {len(data)} bytes)')
        rec = data[off:off + size]
        if rec[:4] not in (b'XTH\x00', b'XTG\x00'):
            raise SystemExit(f'{path}: page {i} at {off} has magic {rec[:4]!r}, not a page record')
        pages.append(rec)
        offsets.append(off)
    # The count in the header IS the number of records by construction; asserting it keeps that
    # true for anyone who later edits this loop.
    if len(pages) != page_count:
        raise SystemExit(f'{path}: walked {len(pages)} records, header says {page_count}')
    header = {'magic': magic, 'page_count': page_count, 'index_offset': index_offset,
              'data_offset': data_offset, 'title': title, 'create_time': create_time,
              'offsets': offsets}
    return pages, header


def planes(rec):
    """(payload, name) pairs. XTH carries two planes, XTG one; the record magic decides."""
    if rec[:4] == b'XTH\x00':
        return [(rec[22:22 + 48000], 'p1'), (rec[22 + 48000:22 + 96000], 'p2')]
    return [(rec[22:22 + 48000], 'bw')]


def ink_bits(plane):
    return sum(bin(b).count('1') for b in plane)


def main():
    # Value flags take the token after them; everything else is positional.
    value_flags = {'--limit'}
    args, skip = [], False
    for i, a in enumerate(sys.argv[1:], start=1):
        if skip:
            skip = False
            continue
        if a in value_flags:
            skip = True
            continue
        if a.startswith('--'):
            continue
        args.append(a)
    limit = int(sys.argv[sys.argv.index('--limit') + 1]) if '--limit' in sys.argv else 12
    if len(args) != 2:
        print(__doc__)
        return 2
    a_pages, a_hdr = container(args[0])
    b_pages, b_hdr = container(args[1])
    print(f'{args[0].split("/")[-1]}: {len(a_pages)} pages, magic {a_hdr["magic"]!r}, '
          f'createTime {a_hdr["create_time"]}')
    print(f'{args[1].split("/")[-1]}: {len(b_pages)} pages, magic {b_hdr["magic"]!r}, '
          f'createTime {b_hdr["create_time"]}')
    if a_hdr['create_time'] != b_hdr['create_time']:
        print('  note: createTime differs (wall clock, not content)')
    if len(a_pages) != len(b_pages):
        print(f'PAGE COUNT DIFFERS: {len(a_pages)} vs {len(b_pages)}')
    n = min(len(a_pages), len(b_pages))
    differing = []
    for i in range(n):
        ra, rb = a_pages[i], b_pages[i]
        if ra[:4] != rb[:4]:
            differing.append((i, 'record-magic', abs(len(ra) - len(rb))))
            continue
        pa, pb = planes(ra), planes(rb)
        for (x, name), (y, _n) in zip(pa, pb):
            if x != y:
                d = sum(1 for u, v in zip(x, y) if u != v)
                differing.append((i, name, d))
    if not differing:
        print(f'IDENTICAL: {n} pages, every plane byte-equal')
        return 0
    pages_hit = sorted({p for p, _, _ in differing})
    print(f'{len(differing)} differing plane(s) across {len(pages_hit)} page(s): '
          f'{pages_hit[:10]}{" ..." if len(pages_hit) > 10 else ""}')
    for i, name, d in differing[:limit]:
        pa = {n: b for b, n in planes(a_pages[i])}
        pb = {n: b for b, n in planes(b_pages[i])}
        if name in pa and name in pb:
            print(f'  page {i:5d} {name:3s} {d:6d} bytes differ   ink {ink_bits(pa[name]):7d} vs '
                  f'{ink_bits(pb[name]):7d}')
        else:
            print(f'  page {i:5d} {name}')
    if len(differing) > limit:
        print(f'  ... {len(differing) - limit} more')
    print(f'total differing bytes: {sum(d for _, _, d in differing)}')
    return 1


if __name__ == '__main__':
    sys.exit(main())
