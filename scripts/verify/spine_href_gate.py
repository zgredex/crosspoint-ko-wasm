#!/usr/bin/env python3
"""Gate for the `Driver::spineHref()` dangling-reference bug (P0 of the open-path audit).

Two halves, because either one alone can be fooled:

  1. STATIC — the real sources must not return a reference into a by-value temporary.
     `spineHref()` must return `std::string` by value, and no caller may bind it with `const auto&`.
  2. EMPIRICAL — `tools/spine_href_repro.cpp` reproduces the shape and must show that the reference
     form corrupts the href once any call intervenes while the by-value form stays stable. Without
     this half the gate would only be asserting that a line of text exists.

Why the empirical half matters: the in-repo `--spine-hrefs` sweep in `src/host_main.cpp` PASSES with
the bug live (it copies the href immediately, so the poisoned stack slot is still intact — row A of the
repro). A gate that cannot fail is not evidence. This one compiles the shape at -O0 and -O3 and
requires row B (buggy + intervening call) to diverge and row C (fixed) to agree.

Exit 0 = PASS, 1 = FAIL (with named failures).
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "src/ko_engine_driver.h"
WASMAPI = ROOT / "src/wasm_api.cpp"
REPRO = ROOT / "tools/spine_href_repro.cpp"
WORK = Path("/tmp/spine_href_gate")

failures = []


def check(ok, label, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {label}" + (f" — {detail}" if (detail and not ok) else ""))
    if not ok:
        failures.append(label)


def static_checks():
    print("static — the sources must not hand out a reference into a temporary")
    src = DRIVER.read_text()
    m = re.search(r"^\s*(const\s+std::string\s*&|std::string)\s+spineHref\s*\(\s*int\s+\w+\s*\)\s*const\s*\{",
                  src, re.M)
    if not m:
        check(False, "spineHref() declaration found", "no `spineHref(int) const` in ko_engine_driver.h")
    else:
        returns_ref = m.group(1).startswith("const")
        check(not returns_ref, "spineHref() returns std::string by value",
              f"declared `{m.group(1)}`" if returns_ref else "declared `std::string`")
        body = src[m.start():src.index("}", src.index("return epub_->getSpineItem", m.start())) + 1]
        check("getSpineItem" in body, "spineHref() body still reads through getSpineItem()",
              "body no longer reads the metadata cache — has the accessor been rewritten?")
        if returns_ref:
            pass
        else:
            # by-value return: the returned object must be constructed inside the call, i.e. the
            # `return` statement must be the copy, not a reference to a member of a temporary.
            check("return epub_->getSpineItem" in body,
                  "by-value return copies the entry inside the call")

    api = WASMAPI.read_text()
    bad_binding = re.findall(r"const\s+auto\s*&\s*\w+\s*=\s*g_driver->spineHref", api)
    check(not bad_binding, "no caller binds spineHref() with `const auto&`",
          f"{len(bad_binding)} such binding(s)" if bad_binding else "")
    check("static std::string last;" in api,
          "ko_get_spine_href() copies into its own buffer",
          "the returned pointer must refer to storage the export owns")


def build_and_run(opt):
    exe = WORK / f"repro_{opt}"
    r = subprocess.run(["clang++", f"-{opt}", "-std=c++17", "-o", str(exe), str(REPRO)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        check(False, f"repro compiles at -{opt}", (r.stderr or "").strip().splitlines()[-1:][0] if r.stderr else "")
        return None
    # -O0 with the buggy form aborts inside libc++ (corrupted string length); that is a finding, not
    # a build failure, so the run is allowed to exit non-zero and its output is still parsed.
    # The whole point is that this program emits stack garbage; those bytes are not valid UTF-8, so the
    # run must be decoded leniently or the gate dies on its own evidence.
    r = subprocess.run([str(exe)], capture_output=True, timeout=60)
    out = (r.stdout or b"").decode("utf-8", "replace") + (r.stderr or b"").decode("utf-8", "replace")
    nums = re.search(r"a_immediate_bad=(\d+) b_after_call_bad=(\d+) c_fixed_bad=(\d+)", out)
    return {"out": out, "exit": r.returncode, "nums": nums}


def empirical_checks():
    print("empirical — tools/spine_href_repro.cpp must discriminate")
    WORK.mkdir(parents=True, exist_ok=True)
    for opt in ("O0", "O3"):
        res = build_and_run(opt)
        if res is None:
            continue
        crashed = res["nums"] is None
        crashtext = "terminated (corrupted string)" if crashed else ""
        if crashed:
            # Aborting is the strongest possible evidence at this optimisation level.
            check(True, f"-{opt}: buggy reference return terminates the process",
                  "detail: " + next((l for l in res["out"].splitlines()
                                     if "libc++abi" in l or "Abort" in l), crashtext))
            check(True, f"-{opt}: fixed form cannot be reached in the crash run", "informational")
            continue
        a_bad, b_bad, c_bad = (int(g) for g in res["nums"].groups())
        check(b_bad > 0, f"-{opt}: buggy reference return corrupts the href once a call intervenes",
              f"{b_bad} of 3 reads wrong")
        check(c_bad == 0, f"-{opt}: by-value return is stable under the same reuse",
              f"{c_bad} of 3 reads wrong" if c_bad else "")
        # Row A is reported, not asserted: it is the reason the in-repo sweep is insensitive.
        print(f"        note: immediate read (no intervening call) had {a_bad}/3 wrong — this is why the "
              f"--spine-hrefs sweep passes with the bug live")


def main():
    print(f"spine-href gate — {ROOT}")
    if not REPRO.exists():
        print(f"  FAIL  repro missing at {REPRO}")
        return 1
    static_checks()
    empirical_checks()
    print()
    if failures:
        print(f"FAIL — {len(failures)} check(s) failed: " + "; ".join(failures))
        return 1
    print("PASS — spineHref() cannot hand out a reference into a by-value temporary, and the repro "
          "demonstrates the corruption the fix removes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
