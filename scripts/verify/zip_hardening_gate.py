#!/usr/bin/env python3
"""zip_hardening_gate.py — negative controls for the structural ZIP checks (stages 6, 7, 11, 13).

Every check added to the ZIP/XML paths is only worth what its control proves, so this gate mutates a real
EPUB in one specific way per case and requires the ENGINE TO REJECT IT. A mutant that loads is a failed
control, and the unmutated book has to keep working or the control proves nothing.

Usage: /usr/bin/python3 scripts/verify/zip_hardening_gate.py [book.epub]
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOST = os.path.join(ROOT, 'build', 'ko_xtch_host')
BOOK = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'web', 'demo-png.epub')

problems = []
results = []


def load(path, out):
    """Run the host CLI. Returns (exit_code, stderr_tail)."""
    proc = subprocess.run([HOST, path, out], capture_output=True, timeout=180)
    tail = (proc.stderr.decode('utf-8', 'replace') or proc.stdout.decode('utf-8', 'replace')).strip()
    return proc.returncode, tail[-300:]


def find(data, sig, start=0):
    i = data.find(sig, start)
    if i < 0:
        raise SystemExit(f'signature {sig!r} not found — is {BOOK} a zip?')
    return i


def eocd_offset(data):
    return data.rfind(b'PK\x05\x06')


def cd_entries(d):
    """Yield (record_offset, name, local_header_offset, uncompressed_size) for each central record."""
    e = eocd_offset(d)
    cd_off = struct.unpack_from('<I', d, e + 16)[0]
    n = struct.unpack_from('<H', d, e + 10)[0]
    pos = cd_off
    for _ in range(n):
        if bytes(d[pos:pos + 4]) != b'PK\x01\x02':
            return
        name_len = struct.unpack_from('<H', d, pos + 28)[0]
        extra_len = struct.unpack_from('<H', d, pos + 30)[0]
        comment_len = struct.unpack_from('<H', d, pos + 32)[0]
        yield (pos, bytes(d[pos + 46:pos + 46 + name_len]),
               struct.unpack_from('<I', d, pos + 42)[0],      # local header offset is at record byte 42
               struct.unpack_from('<I', d, pos + 24)[0])
        pos += 46 + name_len + extra_len + comment_len


def mutant(name, mutate, expect_hint=None):
    """Write a mutated copy, require rejection, and require no container to be produced."""
    data = bytearray(open(BOOK, 'rb').read())
    mutate(data)
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, 'mutant.epub')
        out = os.path.join(td, 'out.xtch')
        open(src, 'wb').write(bytes(data))
        rc, tail = load(src, out)
        produced = os.path.exists(out) and os.path.getsize(out) > 0
        ok = (rc != 0) and not produced
        detail = f'exit={rc} produced={produced}'
        if expect_hint and expect_hint not in tail:
            ok = False
            detail += f' | expected {expect_hint!r} in: {tail[:160]}'
        results.append((ok, name, detail, tail))
        if not ok:
            problems.append(name)


# ---- control: the unmutated book must still load ------------------------------------------------
with tempfile.TemporaryDirectory() as td:
    out = os.path.join(td, 'ok.xtch')
    rc, tail = load(BOOK, out)
    ok = rc == 0 and os.path.exists(out) and os.path.getsize(out) > 0
    results.append((ok, 'control: the unmutated book loads', f'exit={rc}', tail))
    if not ok:
        problems.append('control')

# ---- 1. multi-disk archive ----------------------------------------------------------------------
def multi_disk(d):
    e = eocd_offset(d)
    struct.pack_into('<H', d, e + 4, 1)          # this disk number -> disk 1
    struct.pack_into('<H', d, e + 6, 1)          # central directory start disk
    struct.pack_into('<H', d, e + 8, 0xffff)     # entries on this disk -> disagree with the total

mutant('multi-disk EOCD is rejected', multi_disk, 'Multi-disk')

# ---- 2. the central directory runs past the EOCD record (but is still inside the file) -----------
def cd_past_eocd(d):
    e = eocd_offset(d)
    cd_off = struct.unpack_from('<I', d, e + 16)[0]
    # end the directory 4 bytes before EOF: inside the FILE (so the old check passes) but after the
    # EOCD record that describes it (so the new check must fire)
    struct.pack_into('<I', d, e + 12, len(d) - cd_off - 4)

mutant('central directory past the EOCD record is rejected', cd_past_eocd, 'does not end before the EOCD')

# ---- 3. a corrupt central-directory record (partial cache must not be published) -----------------
def corrupt_cd_record(d):
    # Break the FIRST record: every walk of the directory starts there, so this is the variant that must be
    # caught. (A corrupt record for an entry the reader never touches can legitimately go unread when
    # lookups are by name — that is a design choice, not an unchecked path, so it is not asserted here.)
    pos = next(iter(cd_entries(d)))[0]
    d[pos:pos + 4] = b'\x00\x00\x00\x00'         # signature gone

mutant('a corrupt central-directory record is rejected', corrupt_cd_record)

# ---- 4. local header whose lengths push the data past EOF ---------------------------------------
def bogus_local_lengths(d):
    # Target the OPF's own local header, not "the first local header in the file": the reader resolves
    # items through the central directory, so a local header it never resolves proves nothing.
    target = None
    for _pos, name, lho, _usz in cd_entries(d):
        if name.lower().endswith(b'.opf'):
            target = lho
            break
    if target is None:
        raise SystemExit('no OPF entry found to retarget')
    struct.pack_into('<H', d, target + 26, 0xffff)   # filename length
    struct.pack_into('<H', d, target + 28, 0xffff)   # extra length

mutant('local header lengths that push the data past EOF are rejected', bogus_local_lengths)

# ---- 5. an OPF claiming an absurd uncompressed size ---------------------------------------------
def huge_opf(d):
    e = eocd_offset(d)
    cd_off = struct.unpack_from('<I', d, e + 16)[0]
    n = struct.unpack_from('<H', d, e + 10)[0]
    pos = cd_off
    for _ in range(n):
        name_len = struct.unpack_from('<H', d, pos + 28)[0]
        extra_len = struct.unpack_from('<H', d, pos + 30)[0]
        comment_len = struct.unpack_from('<H', d, pos + 32)[0]
        name = bytes(d[pos + 46:pos + 46 + name_len])
        if name.lower().endswith(b'.opf'):
            struct.pack_into('<I', d, pos + 24, 0x80000000)   # 2 GiB uncompressed
            return
        pos += 46 + name_len + extra_len + comment_len
    raise SystemExit('no OPF entry found to mutate')

mutant('an OPF claiming 2 GiB uncompressed is rejected', huge_opf, 'too large')

# ---- report -------------------------------------------------------------------------------------
print(f'zip-hardening gate — {os.path.basename(BOOK)}')
for ok, name, detail, tail in results:
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    if not ok and tail:
        print(f"         engine said: {tail[:160]}")
print()
if problems:
    print(f"FAIL — {len(problems)} control(s) did not behave: {', '.join(problems)}")
    sys.exit(1)
print(f'PASS — all {len(results)} controls behave: the healthy book loads, and every hostile archive is '
      f'rejected with a container never produced')
