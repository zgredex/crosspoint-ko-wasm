#!/usr/bin/env python3
"""zip_hardening_gate.py — negative controls for the structural ZIP checks (stages 6, 7, 11, 13).

Every check added to the ZIP/XML paths is only worth what its control proves, so this gate mutates a real
EPUB in one specific way per case and requires the ENGINE TO REJECT IT. A mutant that loads is a failed
control, and the unmutated book has to keep working or the control proves nothing.

Usage: /usr/bin/python3 scripts/verify/zip_hardening_gate.py [book.epub]
"""
import io
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

# ---- 6. a STORED entry whose two sizes disagree -------------------------------------------------
# The fixture stores only "mimetype" that way, and the reader never resolves it through the central
# directory — so the control needs a book whose CONTENT is stored. Built here rather than assumed.
def make_stored_book():
    import zipfile
    src = zipfile.ZipFile(BOOK)
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_STORED) as dst:
        for info in src.infolist():
            dst.writestr(info.filename, src.read(info.filename))
    src.close()
    return out.getvalue()

STORED_BOOK = make_stored_book()

with tempfile.TemporaryDirectory() as td:
    p = os.path.join(td, 'stored.epub')
    out = os.path.join(td, 'stored.xtch')
    open(p, 'wb').write(STORED_BOOK)
    rc, tail = load(p, out)
    ok = rc == 0 and os.path.exists(out) and os.path.getsize(out) > 0
    results.append((ok, 'control: an all-STORED book loads', f'exit={rc}', tail))
    if not ok:
        problems.append('stored control')

def stored_size_mismatch(d):
    for pos, name, _lho, usz in cd_entries(d):
        if name.lower().endswith(b'.opf'):
            struct.pack_into('<I', d, pos + 24, usz + 4096)     # uncompressed != compressed
            return
    raise SystemExit('no OPF entry found to mutate')

def run_stored_mutant():
    data = bytearray(STORED_BOOK)
    stored_size_mismatch(data)
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, 'm.epub'); out = os.path.join(td, 'm.xtch')
        open(src, 'wb').write(bytes(data))
        rc, tail = load(src, out)
        produced = os.path.exists(out) and os.path.getsize(out) > 0
        ok = rc != 0 and not produced and 'STORED entry size mismatch' in tail
        results.append((ok, 'a STORED entry with mismatched sizes is rejected', f'exit={rc} produced={produced}', tail))
        if not ok:
            problems.append('stored mismatch')

run_stored_mutant()

# ---- 7. an encrypted member ---------------------------------------------------------------------
def encrypted_entry(d):
    for _pos, name, lho, _usz in cd_entries(d):
        if name.lower().endswith(b'.opf'):
            flags = struct.unpack_from('<H', d, lho + 6)[0]
            struct.pack_into('<H', d, lho + 6, flags | 0x0001)   # the encryption bit
            return
    raise SystemExit('no OPF entry found to mutate')

mutant('an encrypted entry is rejected', encrypted_entry, 'encrypted')

# ---- 8. local and central records disagreeing about the method ----------------------------------
def method_contradiction(d):
    for pos, name, lho, _usz in cd_entries(d):
        if name.lower().endswith(b'.opf'):
            # read the central method first, then make the local one CONTRADICT it — writing a constant
            # here was a no-op whenever the entry already used that method
            central = struct.unpack_from('<H', d, pos + 10)[0]
            struct.pack_into('<H', d, lho + 8, 0 if central != 0 else 8)
            return
    raise SystemExit('no OPF entry found to mutate')

mutant('a local/central method contradiction is rejected', method_contradiction, 'contradicts')

# ---- 9. entry data running into the central directory -------------------------------------------
def data_overlaps_cd(d):
    e = eocd_offset(d)
    cd_off = struct.unpack_from('<I', d, e + 16)[0]
    for pos, name, _lho, _usz in cd_entries(d):
        if name.lower().endswith(b'.opf'):
            # A member that reaches INTO the directory but stays inside the FILE: claiming the whole
            # distance to the directory would be caught by the older "outside the file" test instead, and
            # then this control would prove nothing about the overlap rule.
            cd_off2 = struct.unpack_from('<I', d, struct.unpack_from('<I', d, 0)[0])[0] if False else cd_off
            lho_here = struct.unpack_from('<I', d, pos + 42)[0]
            name_len = struct.unpack_from('<H', d, lho_here + 26)[0]
            extra_len = struct.unpack_from('<H', d, lho_here + 28)[0]
            data_off = lho_here + 30 + name_len + extra_len
            struct.pack_into('<I', d, pos + 20, cd_off2 - data_off + 100)
            return
    raise SystemExit('no OPF entry found to mutate')

mutant('entry data overlapping the central directory is rejected', data_overlaps_cd, 'overlaps')

# ---- 10. corruption in a MIDDLE record (not the first, which the walker always touches) ----------
def middle_record_corrupt(d):
    entries = list(cd_entries(d))
    pos = entries[len(entries) // 2][0]
    d[pos:pos + 4] = b'\x00\x00\x00\x00'

mutant('a corrupt MIDDLE central-directory record is rejected', middle_record_corrupt)

# ---- 11. duplicate entry name -------------------------------------------------------------------
def duplicate_name(d):
    entries = list(cd_entries(d))
    by_len = {}
    for pos, name, _lho, _usz in entries:
        by_len.setdefault(len(name), []).append((pos, name))
    for _length, group in by_len.items():
        if len(group) >= 2:
            (p0, n0), (p1, _n1) = group[0], group[1]
            d[p1 + 46:p1 + 46 + len(n0)] = n0          # same length, so every later offset stays valid
            return
    raise SystemExit('no two entries share a name length to make a duplicate from')

# Name uniqueness is now validated once for the complete directory before any
# lazy lookup can trust an entry, so the ordinary host load must observe this.
mutant('a duplicate central-directory name is rejected globally', duplicate_name, 'duplicate central-directory name')

# ---- 12. central/local flag contradiction -------------------------------------------------------
def flag_contradiction(d):
    for pos, name, lho, _usz in cd_entries(d):
        if name.lower().endswith(b'.opf'):
            flags = struct.unpack_from('<H', d, pos + 8)[0]
            struct.pack_into('<H', d, pos + 8, flags | 0x0008)   # data-descriptor bit only in the central record
            return
    raise SystemExit('no OPF entry found to mutate')

mutant('a local/central flag contradiction is rejected', flag_contradiction, 'flag contradiction')

# ---- 13. local filename contradicting the central directory -------------------------------------
def local_name_contradiction(d):
    for _pos, name, lho, _usz in cd_entries(d):
        if name.lower().endswith(b'.opf'):
            nlen = struct.unpack_from('<H', d, lho + 26)[0]
            assert nlen == len(name)
            d[lho + 30:lho + 30 + nlen] = b'x' * nlen          # same length, different bytes
            return
    raise SystemExit('no OPF entry found to mutate')

mutant('a local filename contradicting the central directory is rejected', local_name_contradiction, 'local filename')

# ---- 14. a corrupted payload byte: the CRC must catch it ----------------------------------------
def corrupt_deflated_payload(d):
    for _pos, name, lho, _usz in cd_entries(d):
        if name.lower().endswith(b'.opf'):
            nlen = struct.unpack_from('<H', d, lho + 26)[0]
            elen = struct.unpack_from('<H', d, lho + 28)[0]
            data = lho + 30 + nlen + elen
            d[data + 8] ^= 0xFF                                 # a byte inside the deflate stream
            return
    raise SystemExit('no OPF entry found to mutate')

mutant('a damaged DEFLATE payload is rejected (inflate failure or CRC)', corrupt_deflated_payload)

def corrupt_stored_payload(d):
    for _pos, name, lho, _usz in cd_entries(d):
        if name.lower().endswith(b'.xhtml'):
            nlen = struct.unpack_from('<H', d, lho + 26)[0]
            elen = struct.unpack_from('<H', d, lho + 28)[0]
            d[lho + 30 + nlen + elen + 16] ^= 0xFF              # one byte of a STORED member
            return
    raise SystemExit('no stored XHTML entry found to mutate')

def run_stored_crc_mutant():
    data = bytearray(STORED_BOOK)
    corrupt_stored_payload(data)
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, 'c.epub'); out = os.path.join(td, 'c.xtch')
        open(src, 'wb').write(bytes(data))
        rc, tail = load(src, out)
        produced = os.path.exists(out) and os.path.getsize(out) > 0
        ok = rc != 0 and not produced
        results.append((ok, 'a damaged STORED payload is rejected (CRC)', f'exit={rc} produced={produced}', tail))
        if not ok:
            problems.append('stored crc')

run_stored_crc_mutant()

# CRC 0 is a legitimate value, not an "unset" sentinel. Corrupt a complete
# STORED member and replace its central CRC with zero: the reader must compute
# the real CRC and reject instead of skipping integrity verification.
def run_zero_crc_mutant():
    data = bytearray(STORED_BOOK)
    for pos, name, lho, _usz in cd_entries(data):
        if name.lower().endswith(b'.xhtml'):
            nlen = struct.unpack_from('<H', data, lho + 26)[0]
            elen = struct.unpack_from('<H', data, lho + 28)[0]
            data[lho + 30 + nlen + elen + 16] ^= 0xFF
            struct.pack_into('<I', data, pos + 16, 0)
            break
    else:
        raise SystemExit('no stored XHTML entry found for zero-CRC control')
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, 'z.epub'); out = os.path.join(td, 'z.xtch')
        open(src, 'wb').write(bytes(data))
        rc, tail = load(src, out)
        produced = os.path.exists(out) and os.path.getsize(out) > 0
        ok = rc != 0 and not produced and 'CRC mismatch' in tail
        results.append((ok, 'a STORED payload with central CRC zero is still verified',
                        f'exit={rc} produced={produced}', tail))
        if not ok:
            problems.append('zero crc')

run_zero_crc_mutant()

# ---- 15. variable-length fields crossing the declared directory end ------------------------------
def fields_cross_cd_end(d):
    for pos, name, _lho, _usz in cd_entries(d):
        if name.lower().endswith(b'.opf'):
            struct.pack_into('<H', d, pos + 30, 0xFFF0)         # extraLen far past cdEnd
            return
    raise SystemExit('no OPF entry found to mutate')

mutant('central fields crossing the directory end are rejected', fields_cross_cd_end)

# ---- report -------------------------------------------------------------------------------------
print(f'zip-hardening gate — {os.path.basename(BOOK)}')
for ok, name, detail, tail in results:
    label = 'PASS' if ok else ('SKIP' if ok is None else 'FAIL')
    print(f"  {label}  {name}")
    if ok is False and tail:
        print(f"         engine said: {tail[:160]}")
print()
if problems:
    print(f"FAIL — {len(problems)} control(s) did not behave: {', '.join(problems)}")
    sys.exit(1)
skipped = [n for ok, n, _d, _t in results if ok is None]
verdict = (f'PASS — {len(results) - len(skipped)} of {len(results)} controls behave: the healthy books load, '
           f'and every hostile archive is rejected with a container never produced')
if skipped:
    verdict += f' | NOT PROVEN (path unreachable from this harness): {len(skipped)}'
print(verdict)
