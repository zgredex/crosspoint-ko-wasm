#!/usr/bin/env python3
"""Control for the container readers: prove the magic-scan failure is real, and that the
index-driven readers are immune to it.

The failure this guards against is quiet. A walker that finds page records by searching for the
record magic and advancing 22 bytes past each hit is anchored by *content*: as soon as a payload
contains `XTH\\0` within 22 bytes before the next real record, the walk re-anchors on the payload
bytes and skips the real record. The observed symptom is not a smaller count — it is the same count
of WRONG offsets, one bogus hit replacing one real one. Downstream that is worse than a missing page:

  * slicing a record at a bogus offset yields garbage, which compares unequal to its counterpart, so
    a conformance run reports a difference that is an artefact of the walker;
  * if both files are mis-anchored the same way, the run reports a pass while never having compared
    the pages it claims to have compared.

So this control does not take the fix on trust. It manufactures the exact hostile container and
requires all three:

  1. on the unmodified file, the scan and the index AGREE (otherwise the scan would be trivially
     broken and prove nothing about the injection);
  2. on the hostile file, the scan's offsets DIFFER from the index's — and specifically the scan
     has lost a real offset and gained a bogus one;
  3. the index-driven reader returns every page, at the index's own offsets, on the hostile file.

usage: container_reader_controls.py <container.xtc|.xtch> [--work DIR]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import container_diff as cd  # noqa: E402

STRIDE = 22          # what the retired walker advanced past each hit
INJECT_BACKOFF = 10  # place the false positive inside the walker's 22-byte advance window


def magic_scan(path, magic=b'XTH\x00', stride=STRIDE):
    """The walker this repo used to rely on: search for the magic, advance a little, repeat."""
    d = open(path, 'rb').read()
    out, pos = [], 0
    while True:
        i = d.find(magic, pos)
        if i < 0:
            break
        out.append(i)
        pos = i + stride
    return out


def hostify(src, dest, page_index=1):
    """Write a copy in which the walker's advance window hides the NEXT record's magic.

    The skip mechanism is precise, and getting it wrong makes the control vacuous — injecting the
    magic mid-payload only ADDS a hit, because the scan still finds every real record afterwards.
    (The first version of this did exactly that, and said so instead of passing.) The walk is:

        hit at p  →  pos = p + 22  →  next search starts there

    so a false positive at p hides the real record at R only when p + 22 > R, i.e. when p lies in the
    22 bytes before R. The injection therefore goes in the TAIL of `page_index`'s record: a payload
    that happens to contain the magic right at its end, which is what a dithered photo plane can do
    without being asked.
    """
    data = bytearray(open(src, 'rb').read())
    page_count = data[6] | (data[7] << 8)
    if page_count < 2:
        raise SystemExit('need at least two pages to make the walk skip one')
    index_offset = int.from_bytes(data[0x18:0x20], 'little')
    e = index_offset + page_index * 16
    off = int.from_bytes(data[e:e + 8], 'little')
    size = int.from_bytes(data[e + 8:e + 12], 'little')
    if page_index + 1 >= page_count:
        raise SystemExit('the last record has no successor to hide; pick an earlier page')
    next_off = int.from_bytes(data[e + 16:e + 24], 'little')
    inject_at = next_off - INJECT_BACKOFF
    # The magic must land inside page_index's own record (past its 22-byte header) and finish before
    # the next record begins — otherwise the control is mutating the wrong thing.
    if not (off + 22 <= inject_at and inject_at + 4 <= off + size and inject_at + 4 <= next_off):
        raise SystemExit(f'cannot place the magic in the tail of record {page_index} '
                         f'(off {off}, size {size}, next {next_off})')
    data[inject_at:inject_at + 4] = b'XTH\x00'
    with open(dest, 'wb') as fh:
        fh.write(data)
    return page_count, next_off


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    work = sys.argv[sys.argv.index('--work') + 1] if '--work' in sys.argv else '/tmp/ko-reader-controls'
    if len(args) != 1:
        print(__doc__)
        return 2
    src = args[0]
    os.makedirs(work, exist_ok=True)
    hostile = os.path.join(work, 'hostile.xtch')

    page_count, hidden = hostify(src, hostile)
    records, header = cd.container(src)
    idx = header['offsets']
    scan_src = magic_scan(src)
    scan_hostile = magic_scan(hostile)
    recs_hostile, hdr_hostile = cd.container(hostile)

    print(f'  source: header {page_count} pages, index reader {len(records)}, scan {len(scan_src)}')
    print(f'  hostile: injected XTH\\0 {INJECT_BACKOFF} bytes before the record at {hidden}; '
          f'scan {len(scan_hostile)}, index reader {len(recs_hostile)}')

    failures = 0
    if len(records) != page_count or scan_src != idx:
        print('  x  CONTROL VOID: on the unmodified file the scan and the index already disagree, '
              'so an injection proves nothing')
        failures += 1
    else:
        print('  OK on the clean file the scan and the index agree exactly — the walker works '
              'when payloads cooperate')
    lost = [o for o in idx if o not in scan_hostile]
    bogus = [o for o in scan_hostile if o not in idx]
    if lost and bogus and len(scan_hostile) == len(scan_src):
        print(f'  OK the injection re-anchors the walk: lost the real offset {lost}, gained the '
              f'bogus {bogus} — same count, wrong pages')
    else:
        print(f'  x  CONTROL VOID: expected exactly one lost offset and one bogus offset with an '
              f'unchanged count; got lost={lost} bogus={bogus} '
              f'({len(scan_hostile)} vs {len(scan_src)})')
        failures += 1
    if len(recs_hostile) == page_count and hdr_hostile['offsets'] == idx:
        print(f'  OK the index reader is unaffected: {len(recs_hostile)} records at the index\'s own '
              f'offsets')
    else:
        print(f'  x  the index reader returned {len(recs_hostile)} records (expected {page_count}) '
              f'at {hdr_hostile["offsets"][:3]}… (expected {idx[:3]}…)')
        failures += 1

    print()
    if failures:
        print(f'FAIL — {failures} control assertion(s) failed')
        return 1
    print('PASS — the magic-scan mis-anchoring is reproducible on demand and the index reader is '
          'immune to it')
    return 0


if __name__ == '__main__':
    sys.exit(main())
