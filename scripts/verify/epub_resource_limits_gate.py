#!/usr/bin/env python3
"""Adversarial EPUB controls for parser nesting and OPF cardinality."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import zipfile


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HOST = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "build", "ko_xtch_host")

CONTAINER = """<?xml version="1.0"?>
<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>"""

NAV = """<!DOCTYPE html><html xmlns="http://www.w3.org/1999/xhtml"><body>
<nav epub:type="toc" xmlns:epub="http://www.idpf.org/2007/ops"><ol>
<li><a href="chapter.xhtml">Chapter</a></li>
</ol></nav></body></html>"""


def write_epub(path: str, chapter: str, extra_manifest_items: int = 0) -> None:
    extras = "".join(
        f'<item id="unused{i}" href="missing{i}.xhtml" media-type="application/xhtml+xml"/>'
        for i in range(extra_manifest_items)
    )
    opf = f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Resource limit</dc:title></metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
    {extras}
  </manifest>
  <spine><itemref idref="chapter"/></spine>
</package>"""
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        archive.writestr("META-INF/container.xml", CONTAINER)
        archive.writestr("OEBPS/content.opf", opf)
        archive.writestr("OEBPS/nav.xhtml", NAV)
        archive.writestr("OEBPS/chapter.xhtml", chapter)


def run_host(epub: str, output: str) -> tuple[int, str]:
    proc = subprocess.run(
        [HOST, epub, output, "--max-pages", "1"],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=45,
        check=False,
    )
    return proc.returncode, proc.stdout


def check(ok: bool, label: str, detail: str = "") -> None:
    print(("PASS " if ok else "FAIL ") + label)
    if not ok:
        if detail:
            print(detail[-4000:])
        raise SystemExit(1)


with tempfile.TemporaryDirectory(prefix="xtcko-resource-limits-") as tmp:
    normal = os.path.join(tmp, "normal.epub")
    depth_boundary = os.path.join(tmp, "depth-boundary.epub")
    nested = os.path.join(tmp, "nested.epub")
    manifest_boundary = os.path.join(tmp, "manifest-boundary.epub")
    manifest = os.path.join(tmp, "manifest.epub")

    write_epub(normal, '<html xmlns="http://www.w3.org/1999/xhtml"><body><p>ok</p></body></html>')
    rc, log = run_host(normal, os.path.join(tmp, "normal.xtch"))
    check(rc == 0, "ordinary EPUB remains accepted", log)

    boundary_body = "<div>" * 510 + "at boundary" + "</div>" * 510
    write_epub(depth_boundary,
               f'<html xmlns="http://www.w3.org/1999/xhtml"><body>{boundary_body}</body></html>')
    rc, log = run_host(depth_boundary, os.path.join(tmp, "depth-boundary.xtch"))
    check(rc == 0, "512 simultaneously open XHTML elements remain accepted", log)

    # html + body + 600 divs exceeds the independent 512-element boundary.
    body = "<div>" * 600 + "too deep" + "</div>" * 600
    write_epub(nested, f'<html xmlns="http://www.w3.org/1999/xhtml"><body>{body}</body></html>')
    rc, log = run_host(nested, os.path.join(tmp, "nested.xtch"))
    check(rc != 0 and "XHTML element nesting exceeds supported depth" in log,
          "513+ XHTML nesting is rejected cleanly", log)

    # The two real records plus 32,766 unused records hit the exact boundary.
    write_epub(manifest_boundary,
               '<html xmlns="http://www.w3.org/1999/xhtml"><body><p>ok</p></body></html>',
               extra_manifest_items=32766)
    rc, log = run_host(manifest_boundary, os.path.join(tmp, "manifest-boundary.xtch"))
    check(rc == 0, "32,768 OPF manifest items remain accepted", log)

    # One more item must stop before another index/store allocation.
    write_epub(manifest,
               '<html xmlns="http://www.w3.org/1999/xhtml"><body><p>ok</p></body></html>',
               extra_manifest_items=32767)
    rc, log = run_host(manifest, os.path.join(tmp, "manifest.xtch"))
    check(rc != 0 and "OPF manifest exceeds supported item count" in log,
          "32,769 OPF manifest items are rejected cleanly", log)

print("epub-resource-limits gate OK")
