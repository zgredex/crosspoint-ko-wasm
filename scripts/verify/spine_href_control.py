#!/usr/bin/env python3
"""Negative control for scripts/verify/spine_href_gate.py: reinstate the bug, prove the gate FAILS,
restore. Touches only two source files, no rebuild — the gate's static half reads text and its
empirical half compiles its own repro, so this runs in seconds.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path("/Users/patryk/krxtc/ko-wasm")
DRIVER = ROOT / "src/ko_engine_driver.h"
API = ROOT / "src/wasm_api.cpp"
GATE = ROOT / "scripts/verify/spine_href_gate.py"

FIXED_DRIVER = """  std::string spineHref(int i) const {
    if (!epub_ || i < 0 || i >= spineCount()) return {};
    return epub_->getSpineItem(i).href;
  }"""
BUGGY_DRIVER = "  const std::string& spineHref(int i) const { return epub_->getSpineItem(i).href; }"

FIXED_API = """  static std::string last;
  last = g_driver->spineHref(spineIndex);"""
BUGGY_API = """  const auto& item = g_driver->spineHref(spineIndex);
  static std::string last;
  last = item;"""


def main():
    drv, api = DRIVER.read_text(), API.read_text()
    assert FIXED_DRIVER in drv, "fixed driver body not found"
    assert FIXED_API in api, "fixed api body not found"
    DRIVER.write_text(drv.replace(FIXED_DRIVER, BUGGY_DRIVER, 1))
    API.write_text(api.replace(FIXED_API, BUGGY_API, 1))
    print("control applied: reference-returning spineHref + `const auto&` binding\n")
    rc = 1
    try:
        r = subprocess.run(["/usr/bin/python3", str(GATE)], capture_output=True, text=True)
        print(r.stdout, end="")
        rc = r.returncode
        print(f"\ngate exit with the bug reinstated: {rc}")
        fails = [ln.strip() for ln in r.stdout.splitlines() if "FAIL" in ln]
        for ln in fails:
            print(f"  caught: {ln}")
        print("CONTROL DISCRIMINATES — the gate fails when the bug is reinstated"
              if rc != 0 else "CONTROL DID NOT FAIL — the gate is insensitive")
    finally:
        DRIVER.write_text(drv)
        API.write_text(api)
        print("fixed sources restored")
    return 0 if rc != 0 else 3


if __name__ == "__main__":
    sys.exit(main())
