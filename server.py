#!/usr/bin/env python3
"""Dev server for the CrossPoint-KO preview.

Serves the static web/ directory AND exposes:
  POST /api/convert-font  — OTF/TTF → .epdfont via the KO fork's own
                           ttf_to_epdfont.py (exact device runtime format).

Form fields (multipart):
  font      — the OTF/TTF file
  name      — font name (default "custom")
  size      — point size (default 14, the KO reader size)
  twoBit    — "1" → 2-bit grayscale (KO default), "0"/absent → 1-bit
  extraIntervals — optional comma-separated "MIN,MAX" hex ranges (repeatable)

Requires freetype-py + fonttools for the python3 that runs this server.
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.parse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(HERE, "web")
CONVERTER = os.path.join(HERE, "tools", "ttf_to_epdfont_fast.py")
# fall back to the fork's stock converter if the fast one is missing
if not os.path.exists(CONVERTER):
    CONVERTER = os.path.join(HERE, "..", "ko-wasm-src", "lib", "EpdFont", "scripts", "ttf_to_epdfont.py")
    if not os.path.exists(CONVERTER):
        for cand in ("/Users/patryk/keyboard decomp/ko-wasm-src/lib/EpdFont/scripts/ttf_to_epdfont.py",):
            if os.path.exists(cand):
                CONVERTER = cand
                break

PORT = int(os.environ.get("PORT", "8765"))
PYTHON = os.environ.get("FONTCONVERT_PYTHON", "/usr/bin/python3")
FREETYPE_LIB = "/opt/homebrew/opt/freetype/lib"

# Memoize conversions: key = sha256(font bytes) + size + twoBit + intervals +
# spacePx + weight (name excluded — cosmetic). Re-tuning the same uploaded font
# (very common in the live-preview flow) returns the previous .epdfont without
# re-rasterizing ~12k glyphs.
import hashlib
_CONVERT_CACHE = {}
_CONVERT_CACHE_MAX = 24   # slider scrubbing visits many (size,weight) combos

# Raw uploaded fonts, keyed by sha256 hex ("fontId"). The first request per
# font uploads the bytes; every later knob change sends just the fontId + tiny
# params — the 3–10 MB file never crosses the wire again. LRU-bounded.
_RAW_FONT_CACHE = {}
_RAW_FONT_CACHE_MAX = 6


def _read_body(handler):
    """Read the full request body (Content-Length framed)."""
    n = int(handler.headers.get("Content-Length") or 0)
    return handler.rfile.read(n) if n > 0 else b""


def _parse_multipart(body, content_type):
    """Minimal fast multipart/form-data parser → {field: str|bytes}.

    cgi.FieldStorage costs ~1 s on a 3 MB upload (the scrub path's whole
    budget) — this splits on the boundary directly (~ms). File fields keep
    raw bytes; text fields are decoded UTF-8.
    """
    m = re.search(r'boundary="?([^";]+)"?', content_type or "")
    if not m:
        return {}
    boundary = ("--" + m.group(1)).encode("latin-1")
    parts = body.split(boundary)
    out = {}
    for part in parts:
        if part[:2] == b"--" or part.strip(b"\r\n") == b"":
            continue  # closing marker or empty
        # split headers from content
        head, _, payload = part.partition(b"\r\n\r\n")
        if payload.endswith(b"\r\n"):
            payload = payload[:-2]
        disp = re.search(rb'Content-Disposition:[^\r]*?name="([^"]*)"', head)
        if not disp:
            continue
        name = disp.group(1).decode("utf-8", "replace")
        has_file = rb'filename="' in head
        if has_file:
            out[name] = payload            # raw bytes (TTF/OTF)
        else:
            out[name] = payload.decode("utf-8", "replace")
    return out


def parse_convert_form(handler):
    """Return (fields, err). fields['font'] is raw bytes when uploaded; a
    fields['fontId'] is honored instead when the font was registered before."""
    ctype = handler.headers.get("Content-Type", "")
    if not ctype.startswith("multipart/form-data"):
        return None, "expected multipart/form-data"
    body = _read_body(handler)
    fields = _parse_multipart(body, ctype)
    raw = fields.get("font")
    if isinstance(raw, bytes) and raw:
        h = hashlib.sha256(raw).hexdigest()
        _RAW_FONT_CACHE[h] = raw
        while len(_RAW_FONT_CACHE) > _RAW_FONT_CACHE_MAX:
            _RAW_FONT_CACHE.pop(next(iter(_RAW_FONT_CACHE)))
        fields["fontId"] = h          # echo back for future requests
    else:
        fid = fields.get("fontId")
        if not fid or not isinstance(fid, str) or fid not in _RAW_FONT_CACHE:
            return None, "missing 'font' file (first upload registers a fontId)"
        fields["font"] = _RAW_FONT_CACHE[fid]
    return fields, None


def cache_key(raw, name, size, two_bit, no_hangul, extra_intervals, space_px, weight):
    # name and space_px are intentionally NOT part of the key: name is cosmetic
    # (no re-raster for a rename) and space_px is patched post-hoc into the
    # raster cache (patch_space_advance rewrites one glyph record), so space
    # scrubbing is instant without re-rastering ~12k glyphs.
    del name, space_px
    h = hashlib.sha256(raw).hexdigest()
    return (h, size, two_bit, no_hangul, tuple(extra_intervals), weight)


def patch_space_advance(data, px):
    """Set the U+0020 glyph record's advance_x (byte +2 of its 16-byte record).

    v1 .epdfont: header 32B, then interval table (start u32, end u32, offset
    u32 = running glyph index), then glyph records (16B each). U+0020 lives in
    the Basic Latin interval (0x0000-0x007F).
    """
    import struct
    if len(data) < 32:
        return
    _, _, _, _, _, _, _, _, icnt, _, ioff, goff, _ = struct.unpack("<IHBBBBBB5I", data[:32])
    # locate interval containing 0x20
    for i in range(icnt):
        base = ioff + i * 12
        if base + 12 > len(data):
            return
        start, end, gidx = struct.unpack("<3I", data[base:base + 12])
        if start <= 0x20 <= end:
            rec = goff + (gidx + (0x20 - start)) * 16
            if rec + 16 <= len(data):
                data[rec + 2] = px & 0xFF
            return


def font_wght_axis(path):
    """(min, max) of the font's wght axis, or None if not variable wght."""
    try:
        from fontTools.ttLib import TTFont
        tt = TTFont(path, lazy=True, fontNumber=0)
        out = None
        fvar = tt.get("fvar")
        if fvar:
            for a in fvar.axes:
                if getattr(a, "axisTag", "") == "wght":
                    out = (int(a.minValue), int(a.maxValue))
                    break
        tt.close()
        return out
    except Exception:
        return None


_FONT_PROBE_CACHE = {}    # sha256(raw) → {has_wght: bool, native: int}
_FONT_PROBE_CACHE_MAX = 12


def font_probe(raw):
    """Memoized per-font (by sha256) wght-axis + OS/2 native-weight probe.

    fontTools opening a 3–10 MB font costs ~150 ms; scrubbing the weight slider
    asks for many (size, weight) rasters of the SAME font, so probe once per
    uploaded file instead of once per request.
    """
    h = hashlib.sha256(raw).hexdigest()
    got = _FONT_PROBE_CACHE.get(h)
    if got is not None:
        return got
    import tempfile as _tf
    with _tf.TemporaryDirectory(prefix="koprobe_") as td:
        src = os.path.join(td, "font" + (".otf" if raw[:4] == b"OTTO" else ".ttf"))
        with open(src, "wb") as fh:
            fh.write(raw)
        has_wght = font_has_wght_axis(src)
        native = None if has_wght else font_native_weight(src)
    got = {"has_wght": has_wght, "native": native}
    if len(_FONT_PROBE_CACHE) >= _FONT_PROBE_CACHE_MAX:
        _FONT_PROBE_CACHE.pop(next(iter(_FONT_PROBE_CACHE)))
    _FONT_PROBE_CACHE[h] = got
    return got


def font_has_wght_axis(path):
    return font_wght_axis(path) is not None


def instance_wght(path, out_path, weight):
    """Pin a variable font's wght axis to `weight`, saving a static TTF/OTF.

    NotoSansKR-VariableFont etc. declare fvar default wght=100 (Thin) — the
    rasterizer would otherwise bake hairline glyphs. Instancing produces a
    static face whose outlines ARE the requested weight; the regular converter
    then runs unchanged on it. The weight is clamped to the axis range.
    """
    from fontTools.ttLib import TTFont
    from fontTools.varLib.instancer import instantiateVariableFont
    rng = font_wght_axis(path)
    if rng:
        weight = max(rng[0], min(weight, rng[1]))
    tt = TTFont(path, fontNumber=0)
    instantiateVariableFont(tt, {"wght": weight}, inplace=True)
    tt.save(out_path)
    tt.close()
    return weight


def font_native_weight(path):
    """OS/2 usWeightClass of a static font (400 default when unknown)."""
    try:
        from fontTools.ttLib import TTFont
        tt = TTFont(path, lazy=True, fontNumber=0)
        w = 400
        if tt.get("OS/2"):
            w = tt["OS/2"].usWeightClass or 400
        tt.close()
        return int(w)
    except Exception:
        return 400


def weight_to_embolden_px64(weight, native_weight, size):
    """Map a user weight (100–900) to an FT_Outline_Embolden strength.

    Static faces (no wght axis) get synthetic bold relative to their native
    OS/2 weight: 100 weight steps ≈ +0.5 px stroke at the KO default 14 pt /
    150 dpi (measured: Nanum Regular 400 → 500 with +0.5 px ≈ RIDIBatang's ink
    coverage). Strength scales with the raster ppem (size × 150/72), capped at
    +2.5 px to avoid blob strokes. Requesting ≤ native weight → 0 (already
    heavy enough, and we never thin a face).
    """
    delta = weight - native_weight
    if delta <= 0:
        return 0
    ppem = size * 150.0 / 72.0
    px = 0.5 * (delta / 100.0) * (ppem / 29.17)
    px = max(0.0, min(px, 2.5))
    return int(round(px * 64))


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=WEB, **kw)

    def do_GET(self):
        # rewrite "/" → index.html handled by SimpleHTTPRequestHandler already
        return super().do_GET()

    def end_headers(self):
        # Never let browsers cache dev assets heuristically (no-cache headers =
        # last-modified heuristics that bite after quick successive edits).
        name = self.path.split("?")[0].split("/")[-1]
        if name in ("app.js", "style.css", "index.html", "ko.worker.js", "ko_xtch_wasm.js", "ko_xtch_wasm.wasm"):
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/convert-font":
            self.handle_convert()
        else:
            self.send_error(404, "unknown endpoint")

    def handle_convert(self):
        import time as _t
        _t0 = _t.time()
        fields, ferr = parse_convert_form(self)
        if ferr:
            self.send_json(400, {"error": ferr})
            return
        raw = fields["font"]
        _t1 = _t.time()
        if len(raw) < 4 or raw[:4] not in (b"OTTO", b"\x00\x01\x00\x00", b"ttcf", b"true"):
            self.send_json(400, {"error": "not a TTF/OTF file (bad magic)"})
            return

        name = re.sub(r"[^\w-]", "", fields.get("name") or "custom") or "custom"
        try:
            size = int(fields.get("size") or "14")
        except ValueError:
            size = 14
        size = max(6, min(size, 72))
        try:
            weight = int(fields.get("weight") or "500")
        except ValueError:
            weight = 500
        weight = max(100, min(weight, 900))
        two_bit = (fields.get("twoBit") or "1") == "1"
        # The converter only includes Hangul when the font NAME says korean or
        # an explicit interval is given. KO reader faces need full modern
        # Hangul (U+AC00..U+D7AF) — pass it by default, like the doc command.
        extra = fields.get("extraIntervals") or ""
        extra_intervals = []
        if (fields.get("noHangul") or "0") != "1":
            extra_intervals.append("0xAC00,0xD7AF")
        for chunk in extra.split(","):
            chunk = chunk.strip()
            if not chunk:
                continue
            m = re.match(r"^(0[xX][0-9a-fA-F]+)\s*[-:]\s*(0[xX][0-9a-fA-F]+)$", chunk)
            if m:
                extra_intervals.append("0x%X,0x%X" % (int(m.group(1), 16), int(m.group(2), 16)))
        # spacePx patch value (default: no patch)
        sp_raw = fields.get("spacePx")
        space_px = None
        if sp_raw:
            try:
                space_px = int(sp_raw)
            except ValueError:
                space_px = None
            if space_px is not None and not (0 <= space_px <= 255):
                space_px = None
        font_id = fields.get("fontId") or ""

        ck = cache_key(raw, name, size, two_bit, (fields.get("noHangul") or "0") == "1",
                       extra_intervals, space_px, weight)
        cached = _CONVERT_CACHE.get(ck)
        data = None
        weight_mode = "none"      # how the requested weight was realized
        applied_embolden = 0
        native_weight = None
        effective_weight = weight
        if cached is not None:
            data, weight_mode, applied_embolden, native_weight, effective_weight = cached
            data = bytearray(data)   # per-request copy: space patch below mutates
        else:
            with tempfile.TemporaryDirectory(prefix="koconv_") as td:
                src = os.path.join(td, "font" + (".otf" if raw[:4] == b"OTTO" else ".ttf"))
                with open(src, "wb") as fh:
                    fh.write(raw)
                # Variable wght fonts (e.g. NotoSansKR-VariableFont, whose fvar
                # default is Thin 100) are instanced at the requested weight so
                # the baked glyphs carry real outlines — no synthetic bold.
                # probe is memoized per fontId (one fontTools open per FILE, not
                # per knob state).
                probe = font_probe(raw)
                if probe["has_wght"]:
                    inst = os.path.join(td, "font_w%d.ttf" % weight)
                    effective_weight = instance_wght(src, inst, weight)
                    convert_src = inst
                    weight_mode = "wght-instance"
                else:
                    native_weight = probe["native"]
                    applied_embolden = weight_to_embolden_px64(weight, native_weight, size)
                    convert_src = src
                    if applied_embolden > 0:
                        weight_mode = "embolden"
                    else:
                        weight_mode = "native"
                out = os.path.join(td, name + ".epdfont")
                cmd = [PYTHON, CONVERTER, name, str(size), convert_src,
                       "--2bit" if two_bit else ""]
                cmd = [c for c in cmd if c]
                for iv in extra_intervals:
                    cmd += ["--additional-intervals", iv]
                if applied_embolden > 0:
                    cmd += ["--embolden", str(applied_embolden)]
                cmd += ["-o", out]
                env = dict(os.environ)
                if os.path.isdir(FREETYPE_LIB):
                    env["DYLD_LIBRARY_PATH"] = FREETYPE_LIB + (":" + env["DYLD_LIBRARY_PATH"] if env.get("DYLD_LIBRARY_PATH") else "")
                    env["FREETYPE_LIB_DIR"] = FREETYPE_LIB
                proc = subprocess.run(cmd, capture_output=True, env=env, timeout=300)
                if proc.returncode != 0:
                    self.send_json(500, {
                        "error": "conversion failed",
                        "stderr": (proc.stderr or b"").decode("utf-8", "replace")[-2000:],
                        "stdout": (proc.stdout or b"").decode("utf-8", "replace")[-2000:],
                    })
                    return
                data = bytearray(open(out, "rb").read())
                if len(_CONVERT_CACHE) >= _CONVERT_CACHE_MAX:
                    _CONVERT_CACHE.pop(next(iter(_CONVERT_CACHE)))
                _CONVERT_CACHE[ck] = (bytes(data), weight_mode, applied_embolden, native_weight,
                                      effective_weight)

        # Optional U+0020 advance override (KO typography knob) — applied on the
        # per-request copy, so the spacePx slider never re-rasters. The KO build
        # replaced space with a synthetic glyph advance of 9 px (doc's 146 in
        # 12.4 fixed point = 9.125 nominal). v1 .epdfont stores integer-pixel
        # advances, so an override of N writes N into the space record.
        if space_px is not None:
            patch_space_advance(data, space_px)

        data = bytes(data)

        import base64
        import struct as _s
        _t3 = _t.time()
        hdr = data[:32]
        if len(hdr) == 32:
            _, _, is2, _, ay, asc, desc, _, icnt, gcnt, *_rest = _s.unpack("<IHBBBBBB5I", hdr)
        else:
            is2, ay, asc, desc, icnt, gcnt = 0, 0, 0, 0, 0, 0
        self.send_json(200, {
            "ok": True,
            "name": name,
            "size": size,
            "weight": effective_weight,
            "nativeWeight": native_weight,
            "weightMode": weight_mode,
            "emboldenPx64": applied_embolden,
            "fontId": font_id,
            "twoBit": two_bit,
            "bytes": len(data),
            "advanceY": ay,
            "ascender": asc if asc < 128 else asc - 256,
            "descender": desc if desc < 128 else desc - 256,
            "intervals": icnt,
            "glyphs": gcnt,
            "epdfont": base64.b64encode(data).decode("ascii"),
        })
        sys.stderr.write("conv=%.0fms resp=%.0fms\n" % ((_t3 - _t2) * 1000, (_t.time() - _t3) * 1000))

    def send_json(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("[srv] %s\n" % (fmt % args))


if __name__ == "__main__":
    if not os.path.exists(CONVERTER):
        sys.stderr.write("converter not found: %s\n" % CONVERTER)
        sys.exit(1)
    sys.stderr.write("serving %s on :%d (converter=%s)\n" % (WEB, PORT, CONVERTER))
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
