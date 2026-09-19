#!/usr/bin/env python3
"""Host-side gates for the defensive correctness pass (stages 5, 6, 7, 10).

Each stage here is host-checkable, so it gets a gate that runs without a browser. The JS-side stages
(1, 2, 3, 4, 9) are covered by scripts/verify/defensive_probe.js, which drives the real worker.

What each check proves, and why it is not decorative:

  stage 6  owned-buffer double free — a truncated EPUB through the ADOPTING path must fail cleanly. Before
           the fix the host freed a pointer the storage had already freed, so malformed-input testing
           aborted the harness; the check also re-runs a valid book afterwards, because a harness that
           survives but is broken proves nothing.
  stage 7a short external reads — a reader may return a short positive read; the window fill must loop
           instead of serving the gap as file content. Proven by clamping the host's reader and requiring
           a byte-identical container.
  stage 7b generic read helpers — readFile / readFileToBuffer / readFileToStream must work on an external
           mount, whose Blob has data == nullptr by design. Before the fix these dereferenced that null.
  stage 10 gray-plane control — the rejected state must still be *reachable* (so the control can show it
           is wrong) and must be *unreachable* from product code (no boolean in the product API, macro
           host-only). Both halves are asserted; either alone is a story.

Run: /usr/bin/python3 scripts/verify/defensive_gate.py
"""
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path("/Users/patryk/krxtc/ko-wasm")
HOST = ROOT / "build/ko_xtch_host"
OUT = Path("/tmp/defensive_gate")
BOOK = "web/demo-images.epub"

failures = []


def check(ok, label, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {label}" + (f" — {detail}" if (detail and not ok) else ""))
    if not ok:
        failures.append(label)


def run(args, env=None, book=BOOK, timeout=900):
    e = dict(os.environ)
    e.update(env or {})
    return subprocess.run([str(HOST), str(ROOT / book)] + args, capture_output=True, timeout=timeout, env=e)


def masked_sha(path):
    b = bytearray(Path(path).read_bytes())
    b[296:300] = b"\0\0\0\0"          # createTime: a wall clock, must differ between runs
    return hashlib.sha256(bytes(b)).hexdigest()[:24]


def stage6_owned_double_free():
    print("stage 6 — the adopting path must not free a buffer the storage already freed")
    src = ROOT / "oracle/fixtures/ko-text.epub"
    truncated = OUT / "truncated.epub"
    truncated.write_bytes(src.read_bytes()[: max(1, src.stat().st_size // 3)])

    r = subprocess.run([str(HOST), str(truncated), str(OUT / "t.xtch"), "--owned"],
                       capture_output=True, timeout=300)
    err = (r.stderr or b"").decode("utf-8", "replace")
    aborted = ("double free" in err.lower() or "abort" in err.lower()
               or r.returncode in (-6, 134) or "corrupt" in err.lower())
    check(not aborted, "truncated EPUB via --owned fails without an allocator abort",
          f"exit={r.returncode} stderr={err.strip().splitlines()[-1:] }")
    check(r.returncode != 0, "the truncated book is reported as a failure")
    check("Epub::load failed" in err or "load failed" in err.lower(),
          "the failure is the parse failure, not a crash")

    # The harness must still work afterwards — a fix that makes malformed input "safe" by breaking the
    # valid path is not a fix.
    r2 = run([str(OUT / "ok-owned.xtch"), "--owned"])
    check(r2.returncode == 0, "a valid book still loads through --owned after the failure")


def stage7_short_reads():
    print("stage 7a — a short external read must be looped, not believed")
    full = OUT / "ext-full.xtch"
    short = OUT / "ext-short.xtch"
    r1 = run([str(full), "--external"])
    r2 = run([str(short), "--external"], env={"KO_EXTERNAL_MAX_READ": "4096"})
    if r1.returncode or r2.returncode:
        check(False, "both external arms run", f"exit {r1.returncode}/{r2.returncode}")
        return
    check(masked_sha(full) == masked_sha(short),
          "clamped 4 KiB reads produce a byte-identical container",
          f"{masked_sha(full)} vs {masked_sha(short)}")
    crossings = re.search(r"crossings=(\d+)", r2.stderr.decode("utf-8", "replace"))
    check(crossings is not None and int(crossings.group(1)) > 0,
          "the clamped run still reports its crossings")
    if crossings:
        print(f"        clamped reader: {crossings.group(1)} window fills for a {Path(ROOT / BOOK).stat().st_size:,} byte book")


def stage7_generic_reads():
    print("stage 7b — the generic read helpers must survive an external Blob (data == nullptr)")
    r = run([str(OUT / "helpers.xtch"), "--external", "--read-helpers"])
    err = r.stderr.decode("utf-8", "replace")
    m = re.search(r"READ_HELPERS buffer=(\d+) fileSize=(\d+) whole=(\d+) streamed=(\d+) streamBytes=(\d+)", err)
    check(m is not None, "the read helpers ran at all",
          "no READ_HELPERS line — a null dereference would have crashed instead")
    if m:
        buf, size, whole, streamed, streamed_bytes = (int(g) for g in m.groups())
        expected = Path(ROOT / BOOK).stat().st_size
        check(size == expected, "the external mount reports the real file size", f"{size} != {expected}")
        check(buf == min(1023, expected), "readFileToBuffer filled the buffer it was given", f"{buf}")
        check(whole == expected, "readFile returned the whole file", f"{whole} != {expected}")
        check(streamed == 1 and streamed_bytes == expected, "readFileToStream streamed every byte",
              f"streamed={streamed} bytes={streamed_bytes} expected={expected}")


def stage10_gray_plane_control():
    print("stage 10 — the rejected state must stay demonstrable and stay out of the product")
    pair = []
    for mode, flags in (("1bit", ["--1bit"]),):
        a, b = OUT / f"{mode}-product.xtch", OUT / f"{mode}-control.xtch"
        run([str(a)] + flags)
        run([str(b)] + flags + ["--drop-gray-planes"])
        pair.append((mode, masked_sha(a), masked_sha(b), a.read_bytes() != b.read_bytes()))
    for mode, prod, ctrl, differ in pair:
        check(differ, f"{mode}: the control still changes the container (so the gate can see the hazard)",
              f"{prod} == {ctrl} — the control no longer discriminates")

    # Product API: no boolean that can drop the gray planes, and the macro is host-only.
    driver = (ROOT / "src/ko_engine_driver.h").read_text()
    check(re.search(r"bool renderPage\(int pageIndex,\s*const Spec& spec,\s*RenderedPage& out,\s*"
                    r"ManifestPage\* probe = nullptr,\s*int spineIndex = 0\)", driver) is not None,
          "the product renderPage() takes no gray-plane boolean")
    check("renderPageDroppingGrayForTest" in driver and "KO_TEST_NEGATIVE_CONTROLS" in driver,
          "the test-only entry point exists behind a macro")
    cm = (ROOT / "CMakeLists.txt").read_text()
    defs = [ln.strip() for ln in cm.splitlines() if "KO_TEST_NEGATIVE_CONTROLS" in ln]
    check(len(defs) == 1 and "ko_xtch_host" in cm[max(0, cm.index(defs[0]) - 400):cm.index(defs[0])],
          "the macro is defined for the host target only, never globally",
          f"{len(defs)} definition line(s)")


def stage_storage_path_normalisation():
    """A relative EPUB path must load, and yield the SAME container as the absolute path.

    The host shim's path contract: mountBlob/mountOwnedBlob store the blob under normalisePath(path), while
    openFileForRead used to look up the RAW argument. So every path without a leading '/' mounted
    successfully and then failed to open, surfacing three layers away as "Could not find or size
    META-INF/container.xml" followed by "Could not find content.opf in zip". Both the port binary and the
    reference build were affected (they share this shim), and no gate saw it because run() above always
    builds an absolute path via ROOT / book.

    The absolute arm is the control: it certifies the two containers are comparable at all, so a relative
    arm that "passes" by producing nothing cannot pass this check.
    """
    rel_out, bare_out, abs_out = OUT / "relative.xtch", OUT / "bare.xtch", OUT / "absolute.xtch"
    env = dict(os.environ)

    def invoke(book_arg, out, extra=(), cwd=None):
        if Path(out).exists():
            Path(out).unlink()
        return subprocess.run([str(HOST), book_arg, str(out), *extra],
                              capture_output=True, timeout=900, env=env, cwd=str(cwd or ROOT))

    rel = invoke(BOOK, rel_out)                      # relative, with a slash — as a human types it
    check(rel.returncode == 0 and rel_out.exists(),
          "a RELATIVE epub path loads", (rel.stderr or b"").decode()[-200:])

    # The host reads the file from the real filesystem with fopen BEFORE mounting it, so a bare filename
    # only resolves when the book's own directory is the working directory. That cwd is the point: the
    # mount key then has no slash at all, which is the other half of the contract under test.
    bare = invoke(Path(BOOK).name, bare_out, cwd=(ROOT / BOOK).parent)
    # bare_out, NOT rel_out. rel_out already exists from the check above, so asserting on it let a bare
    # invocation that returned 0 without writing anything pass this check — and the byte-identity check
    # further down was then skipped, because it guards on bare_out.exists().
    check(bare.returncode == 0 and bare_out.exists(),
          "a BARE filename (no slash) loads", (bare.stderr or b"").decode()[-200:])

    abso = invoke(str(ROOT / BOOK), abs_out)
    check(abso.returncode == 0 and abs_out.exists(), "the absolute arm runs (control)")
    if rel.returncode == 0 and abso.returncode == 0 and rel_out.exists() and abs_out.exists():
        check(masked_sha(rel_out) == masked_sha(abs_out),
              "relative and absolute paths produce a byte-identical container")
    if bare.returncode == 0 and abso.returncode == 0 and bare_out.exists() and abs_out.exists():
        # Three SEPARATE outputs. Writing the bare arm into rel_out deleted the relative result, so this
        # check used to compare bare-vs-absolute while claiming to compare relative-vs-absolute.
        check(masked_sha(bare_out) == masked_sha(abs_out),
              "bare-filename and absolute paths produce a byte-identical container")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    print(f"defensive host gate — {ROOT}")
    if not HOST.exists():
        print(f"  FAIL  host binary missing at {HOST}")
        return 1
    stage_storage_path_normalisation()
    stage6_owned_double_free()
    stage7_short_reads()
    stage7_generic_reads()
    stage10_gray_plane_control()
    print()
    if failures:
        print(f"FAIL — {len(failures)} check(s) failed: " + "; ".join(failures))
        return 1
    print("PASS — a relative epub path loads identically to an absolute one, owned load fails closed without "
          "a double free, short external reads are looped, the generic read helpers survive an external Blob, "
          "and the rejected gray-plane state is test-only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
