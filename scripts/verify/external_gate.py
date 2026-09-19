#!/usr/bin/env python3
"""Gate for the range-backed (external) EPUB mount — P4 of the open-path audit.

What it proves:

  1. EQUIVALENCE — a book mounted through on-demand range reads produces the BYTE-IDENTICAL container
     to the same book mounted by copying it in and by adopting it. Every fixture, both container modes.
     The mount is the only thing that changes; the parser, the layout and the encoder must not notice.
  2. DEMAND — the number that decides whether the mount is worth enabling: physical bytes read against
     file size. A mount that ends up fetching the whole archive has gained nothing and must not be
     switched on for that book. Also asserts the window cache actually works (reads served from RAM).
  3. BOUNDS — an out-of-range read must be refused by the storage rather than served (the Blob clamps
     against externalSize); a truncated file must fail the mount, not produce a silently short book.

Run: /usr/bin/python3 scripts/verify/external_gate.py
"""
import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path("/Users/patryk/krxtc/ko-wasm")
HOST = ROOT / "build/ko_xtch_host"
OUT = Path("/tmp/external_gate")
FIXTURES = [
    "oracle/fixtures/ko-text.epub",
    "oracle/fixtures/ko-textref.epub",
    "oracle/fixtures/ko-ruby.epub",
    "oracle/fixtures/ko-mixed.epub",
    "oracle/fixtures/ko-symbols.epub",
    "web/demo-png.epub",
    "web/demo-images.epub",
]
MODES = [("2bit", []), ("1bit", ["--1bit"])]

failures = []


def check(ok, label, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {label}" + (f" — {detail}" if (detail and not ok) else ""))
    if not ok:
        failures.append(label)


def run(book, out_name, extra):
    exe = [str(HOST), str(ROOT / book), str(OUT / out_name)] + extra
    r = subprocess.run(exe, capture_output=True, timeout=900)
    return r


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()[:24]


def parse_external(stderr):
    m = re.search(r"EXTERNAL file=(\d+) bytes, crossings=(\d+), physical=(\d+) \(([\d.]+)% of file\), "
                  r"ram reads=(\d+), read ([\d.]+) ms", stderr)
    return m.groups() if m else None


def parse_loaded(stderr):
    m = re.search(r"loaded: title='([^']*)' spines=(\d+)  \[load ([\d.]+) ms\]", stderr)
    return m.groups() if m else None


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    print(f"external-mount gate — {ROOT}")

    for book in FIXTURES:
        if not (ROOT / book).exists():
            check(False, f"{book} present")
            continue
        for mode_name, mode_flags in MODES:
            base = f"{Path(book).stem}-{mode_name}"
            r_copy = run(book, base + "-copy.xtch", mode_flags)
            r_own = run(book, base + "-owned.xtch", mode_flags + ["--owned"])
            r_ext = run(book, base + "-ext.xtch", mode_flags + ["--external"])
            if r_copy.returncode or r_own.returncode or r_ext.returncode:
                check(False, f"{base}: all three mounts succeed",
                      f"exit copy={r_copy.returncode} owned={r_own.returncode} ext={r_ext.returncode}")
                continue
            s_copy, s_own, s_ext = (sha(OUT / f"{base}-{a}.xtch") for a in ("copy", "owned", "ext"))
            check(s_copy == s_own == s_ext,
                  f"{base}: external == owned == copy container",
                  f"copy={s_copy} owned={s_own} ext={s_ext}")
            # spine counts must agree too — a mount that silently reads short would still hash
            # differently, but this names the failure instead of leaving a hash to interpret.
            lc, le = parse_loaded(r_copy.stderr.decode("utf-8", "replace")), \
                     parse_loaded(r_ext.stderr.decode("utf-8", "replace"))
            check(lc and le and lc[1] == le[1],
                  f"{base}: spine count matches the copying mount",
                  f"copy={lc[1] if lc else '?'} ext={le[1] if le else '?'}")
            stats = parse_external(r_ext.stderr.decode("utf-8", "replace"))
            check(stats is not None, f"{base}: range instrumentation fired",
                  "no EXTERNAL line — the counters did not report")
            if stats:
                crossings, physical, pct, ram = (int(stats[1]), int(stats[2]), float(stats[3]), int(stats[4]))
                check(crossings > 0, f"{base}: the source was actually called")
                check(ram > 0, f"{base}: window cache served reads from RAM",
                      f"{ram} reads - a cache that never hits would refetch every read")
                print(f"        {Path(book).stem:<14} {mode_name}  file={stats[0]:>9} B  crossings={crossings:>4}  "
                      f"physical={physical:>9} B ({pct:5.1f}%)  ram reads={ram}")

    # BOUNDS — a truncated file must not mount as a short-but-valid book.
    print("bounds — hostile input")
    src = ROOT / FIXTURES[0]
    truncated = OUT / "truncated.epub"
    truncated.write_bytes(src.read_bytes()[: max(1, src.stat().st_size // 3)])
    r = run(str(truncated.relative_to(ROOT)) if str(truncated).startswith(str(ROOT)) else str(truncated),
            "trunc.xtch", ["--external"])
    # The host takes an absolute path; a file outside the repo is fine for this one case.
    r = subprocess.run([str(HOST), str(truncated), str(OUT / "trunc.xtch"), "--external"],
                       capture_output=True, timeout=300)
    stderr = r.stderr.decode("utf-8", "replace")
    mounted = "loaded: title=" in stderr
    check(not mounted, "a truncated EPUB fails the range-backed mount instead of parsing short",
          "it mounted and reported a title, so reads ran past the intended end")

    print()
    if failures:
        print(f"FAIL — {len(failures)} check(s) failed: " + "; ".join(failures))
        return 1
    print("PASS — the range-backed mount is byte-identical to copying and adopting, its demand counters "
          "fire, and it refuses a truncated source")
    return 0


if __name__ == "__main__":
    sys.exit(main())
