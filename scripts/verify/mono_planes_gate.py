#!/usr/bin/env python3
"""Gate for the rejected "1-bit output needs no gray planes" optimization (the audit's P5).

The proposal was: a 1-bit consumer reads only the BW plane, so a 1-bit preview or export can skip the
two gray render passes and save a full extra image decode per page. The codebase says otherwise, in two
places:

  * `src/xtch_writer.h::addMonoPage()` requires and reads `lsb`/`msb`, using them to turn grey pixels
    into ink dots and to thin anti-aliased ink.
  * `src/wasm_api.cpp::ko_compose_rgba()` reads them too: "A 1-bit page is never a pure function of the
    BW plane: grey pixels become ink dots."

So the optimization is not available, and the entry point that implemented it (`ko_render_page_mode`,
`renderPageEngine(page, mono)`) has been removed rather than left as a switch somebody could flip.

What this gate asserts:
  1. The product has no path that drops the gray planes for 1-bit output — statically, in every file that
     could pass the flag.
  2. The writer rejects a 1-bit page whose gray planes were dropped.
  3. The same flag does NOT change a 2-bit container — it is specifically a 1-bit hazard.

Run: /usr/bin/python3 scripts/verify/mono_planes_gate.py
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path("/Users/patryk/krxtc/ko-wasm")
HOST = ROOT / "build/ko_xtch_host"
DIFF = ROOT / "scripts/verify/container_diff.py"
OUT = Path("/tmp/mono_planes_gate")
FIXTURES = [
    "oracle/fixtures/ko-text.epub",
    "oracle/fixtures/ko-textref.epub",
    "web/demo-png.epub",
]

failures = []


def check(ok, label, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {label}" + (f" — {detail}" if (detail and not ok) else ""))
    if not ok:
        failures.append(label)


def static_checks():
    print("static — nothing in the product may drop the gray planes for 1-bit output")
    for rel in ("web/ko.worker.js", "src/wasm_api.cpp"):
        src = (ROOT / rel).read_text()
        # `renderPage(..., false)` is the correct call; a non-literal, or a `true`, is the hazard.
        bad = [m.group(0) for m in re.finditer(r"renderPage\([^;]*?(mono\w*|true)\s*\)", src)]
        bad += re.findall(r"_ko_render_page_mode", src)
        check(not bad, f"{rel}: no gray-plane-skipping call remains",
              f"found {bad[:2]}")
    host = (ROOT / "src/host_main.cpp").read_text()
    check("--drop-gray-planes" in host, "host keeps the negative control for this gate",
          "the control is what makes the assertion below possible")
    m = re.search(r"g_dropGrayPlanes = (true|false)", host)
    check(m and m.group(1) == "false", "the control is OFF by default",
          f"default is {m.group(1) if m else '?'} — the correct path must be the default")


def container(path):
    r = subprocess.run(["/usr/bin/python3", str(DIFF), str(path), str(path), "--quiet"],
                       capture_output=True, text=True)
    return r.stdout + r.stderr


def differing_bytes(a, b):
    """Per-page/per-plane differing bytes between two containers, createTime excluded."""
    r = subprocess.run(["/usr/bin/python3", str(DIFF), str(a), str(b), "--quiet"],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    m = re.search(r"total differing bytes:\s*(\d+)", out)
    if m:
        return int(m.group(1))
    if "identical" in out.lower() or "no differences" in out.lower():
        return 0
    return None


def behavioural_checks():
    print("behaviour — the writer must reject missing gray planes for 1-bit output")
    OUT.mkdir(parents=True, exist_ok=True)
    for book in FIXTURES:
        if not (ROOT / book).exists():
            continue
        name = Path(book).stem
        correct = OUT / f"{name}-1bit-correct.xtch"
        r1 = subprocess.run([str(HOST), str(ROOT / book), str(correct), "--1bit"],
                            capture_output=True, timeout=900)
        r2 = subprocess.run([str(HOST), str(ROOT / book), str(OUT / f"{name}-rejected.xtch"),
                             "--1bit", "--drop-gray-planes"],
                            capture_output=True, timeout=900)
        check(r1.returncode == 0, f"{name}: complete three-plane 1-bit page succeeds",
              f"exit {r1.returncode}")
        log = (r2.stdout + r2.stderr).decode(errors="replace")
        check(r2.returncode != 0 and "page encoder refused" in log,
              f"{name}: missing gray planes are rejected", f"exit {r2.returncode}; log={log[-200:]}")

    # The flag must be inert for 2-bit output: it is a 1-bit-only hazard, and if 2-bit changed too the
    # flag would be doing something other than what it claims.
    book = FIXTURES[0]
    name = Path(book).stem
    c2 = OUT / f"{name}-2bit-correct.xtch"
    d2 = OUT / f"{name}-2bit-dropped.xtch"
    subprocess.run([str(HOST), str(ROOT / book), str(c2)], capture_output=True, timeout=900)
    subprocess.run([str(HOST), str(ROOT / book), str(d2), "--drop-gray-planes"], capture_output=True, timeout=900)
    n2 = differing_bytes(c2, d2)
    check(n2 == 0, "2-bit output is unaffected by the flag", f"{n2} differing bytes — the flag is not 1-bit-specific")


def main():
    print(f"mono-planes gate — {ROOT}")
    if not HOST.exists():
        print(f"  FAIL  host binary missing at {HOST}")
        return 1
    static_checks()
    behavioural_checks()
    print()
    if failures:
        print(f"FAIL — {len(failures)} check(s) failed: " + "; ".join(failures))
        return 1
    print("PASS — no product path skips gray planes, the mono writer rejects missing planes, and the "
          "2-bit path is unaffected by the mono-only negative control")
    return 0


if __name__ == "__main__":
    sys.exit(main())
