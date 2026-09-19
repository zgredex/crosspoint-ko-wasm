#!/usr/bin/env python3
"""P0 control: reinstate the dangling-reference form of Driver::spineHref() and prove the
--spine-hrefs sweep (and ASAN) actually FAIL on it. Restores the fixed sources on exit.

The fixed tree is saved to /tmp/p0-fixed/ first, so a failure mid-run cannot lose the fix.
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path("/Users/patryk/krxtc/ko-wasm")
DRIVER = ROOT / "src/ko_engine_driver.h"
WASMAPI = ROOT / "src/wasm_api.cpp"
SAVE = Path("/tmp/p0-fixed")

FIXED_DRIVER = '''  // RETURNS BY VALUE. This used to return `const std::string&` bound to
  // `epub_->getSpineItem(i).href`, but getSpineItem() returns BookMetadataCache::SpineEntry BY VALUE
  // (Epub.h), so the reference outlived the temporary that owned the string — dangling, and because
  // short hrefs live in the string's SSO buffer the garbage was read out of a dead stack frame. That is
  // what produced nondeterministic labels like "\u43c6" / U+070F U+0006. Copying here is a few bytes and
  // removes the UB at the source instead of reducing the number of times it is observed.
  std::string spineHref(int i) const {
    if (!epub_ || i < 0 || i >= spineCount()) return {};
    return epub_->getSpineItem(i).href;
  }'''

BUGGY_DRIVER = '''  const std::string& spineHref(int i) const { return epub_->getSpineItem(i).href; }'''


def read(p):
    return p.read_text()


def write(p, s):
    p.write_text(s)


def main():
    SAVE.mkdir(parents=True, exist_ok=True)
    drv_fixed = read(DRIVER)
    api_fixed = read(WASMAPI)
    (SAVE / "ko_engine_driver.h").write_text(drv_fixed)
    (SAVE / "wasm_api.cpp").write_text(api_fixed)

    assert FIXED_DRIVER in drv_fixed, "the fixed spineHref body was not found"
    # wasm_api currently consumes the by-value accessor directly; the buggy control must bind it
    # through a reference to reproduce the original corruption site.
    live = "  static std::string last;\n  last = g_driver->spineHref(spineIndex);"
    original = "  const auto& item = g_driver->spineHref(spineIndex);\n  static std::string last;\n  last = item;"
    assert live in api_fixed, "the fixed ko_get_spine_href body was not found"

    write(DRIVER, drv_fixed.replace(FIXED_DRIVER, BUGGY_DRIVER))
    write(WASMAPI, api_fixed.replace(live, original))
    print("control applied: reference-returning spineHref + `const auto&` binding")

    env = {"PATH": "/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin",
           "ASAN_OPTIONS": "detect_stack_use_after_return=1:detect_leaks=0"}
    rc = 0
    try:
        for d in ("build", "build-asan"):
            print(f"--- rebuilding {d} ---", flush=True)
            r = subprocess.run(["cmake", "--build", d, "-j8"], cwd=ROOT, env=env,
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout[-2000:], r.stderr[-2000:])
                return 2
        books = sorted(str(p.relative_to(ROOT)) for p in (ROOT / "oracle/fixtures").glob("*.epub"))
        books += ["web/demo-png.epub", "web/demo-images.epub"]
        for label, exe, extra in (("release", "build/ko_xtch_host", {}),
                                  ("ASAN", "build-asan/ko_xtch_host", env)):
            e = dict(env)
            e.update(extra)
            print(f"=== control, {label} ===")
            for b in books:
                r = subprocess.run([f"./{exe}", str(ROOT / b), "/tmp/ab/control.xtch", "--spine-hrefs"],
                                   cwd=ROOT, env=e, capture_output=True, text=True)
                out = r.stdout + r.stderr
                verdict = next((ln for ln in out.splitlines()
                                if "SPINE_HREFS" in ln or "SPINE_HREF_BAD" in ln
                                or "ERROR: AddressSanitizer" in ln or "SUMMARY: AddressSanitizer" in ln),
                               "(no verdict line)")
                print(f"  {b.split('/')[-1]:<24} exit={r.returncode}  {verdict.strip()}")
                if r.returncode != 0:
                    rc = 1
    finally:
        write(DRIVER, drv_fixed)
        write(WASMAPI, api_fixed)
        print("fixed sources restored; rebuilding both configs")
        for d in ("build", "build-asan"):
            subprocess.run(["cmake", "--build", d, "-j8"], cwd=ROOT, env=env,
                           capture_output=True, text=True)
    print("CONTROL DISCRIMINATES" if rc else "CONTROL DID NOT FAIL — the sweep is insensitive to the bug")
    return rc


if __name__ == "__main__":
    sys.exit(0 if main() == 1 else 3)
