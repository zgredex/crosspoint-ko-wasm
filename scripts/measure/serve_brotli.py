#!/usr/bin/env python3
"""Static server for cold-boot measurement: brotli when the client accepts it, and no caching.

WHY NOT `python3 -m http.server`: it serves the wasm uncompressed (7 MB on the wire) and lets the
browser cache it heuristically, so a "cold boot" measurement was really a warm one and the transfer
halves of the numbers were fiction. This serves `Content-Encoding: br` for compressible types — the
same encoding Cloudflare uses in production — with `Cache-Control: no-store`, so every navigation
is a genuine cold fetch and the sizes are real.

Brotli output is cached under /tmp so repeated runs do not re-compress 7 MB each time.

usage: serve_brotli.py <dir> [port]
"""
import http.server
import os
import socketserver
import subprocess
import sys
import urllib.parse

BROTLI_TYPES = ('.wasm', '.js', '.html', '.css', '.json', '.epub', '.epd2')
# .epd2 belongs here: an EPD2 blob is a header plus 2-bit-packed bitmaps, which compresses hard
# (ridibatang 2,243,200 -> 236,244). Leaving it out served the raw bytes, which made a lazy face
# fetch look ~10x more expensive than it is. That is a measurement bug, not a finding.
COMPRESSIBLE_FLOOR = 1024          # don't bother below a kilobyte
CACHE = '/tmp/serve-brotli-cache'
DIRECTORY = '.'                    # set in __main__


def brotli_bytes(path, data):
    """Compressed body for `path`, memoised in /tmp — brotli -q 11 on a 7 MB wasm is ~20 s."""
    os.makedirs(CACHE, exist_ok=True)
    key = os.path.join(CACHE, os.path.basename(path) + '.' +
                       str(os.path.getmtime(path)).replace('.', '') + '.br')
    if os.path.exists(key):
        with open(key, 'rb') as fh:
            return fh.read()
    try:
        out = subprocess.run(['brotli', '-q', '11', '-c'], input=data,
                             stdout=subprocess.PIPE, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print(f'[serve] brotli unavailable for {path}: {exc}', file=sys.stderr)
        return None
    with open(key, 'wb') as fh:
        fh.write(out)
    return out


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=DIRECTORY, **kw)

    def end_headers(self):
        # no-store: a cold-boot measurement must not reuse anything
        self.send_header('Cache-Control', 'no-store, must-revalidate')
        super().end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = self.translate_path(parsed.path)
        if not os.path.isfile(path):
            return super().do_GET()
        accepts_br = 'br' in (self.headers.get('Accept-Encoding') or '')
        ext = os.path.splitext(path)[1].lower()
        if not accepts_br or ext not in BROTLI_TYPES:
            return super().do_GET()

        with open(path, 'rb') as fh:
            raw = fh.read()
        if len(raw) < COMPRESSIBLE_FLOOR:
            return super().do_GET()
        comp = brotli_bytes(path, raw)
        if comp is None or len(comp) >= len(raw):
            return super().do_GET()

        self.send_response(200)
        self.send_header('Content-Type', self.guess_type(path))
        self.send_header('Content-Encoding', 'br')
        self.send_header('Content-Length', str(len(comp)))
        self.end_headers()
        self.wfile.write(comp)

    def log_message(self, format, *a):  # noqa: A002 (signature is the stdlib's)
        sys.stderr.write('[serve] ' + (format % a) + '\n')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    DIRECTORY = os.path.abspath(sys.argv[1])
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8898
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(('127.0.0.1', port), Handler) as httpd:
        print(f'[serve] {DIRECTORY} on http://127.0.0.1:{port} '
              f'(brotli for {", ".join(BROTLI_TYPES)}, no-store)', file=sys.stderr)
        httpd.serve_forever()
