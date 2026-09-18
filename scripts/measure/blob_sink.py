#!/usr/bin/env python3
"""Accept a POSTed blob and write it to a file, so browser-produced output can be diffed on disk.

Why this exists: an export the browser produced could only be hashed in the page, and a hash tells
you THAT two files differ without telling you WHERE. Comparing page records and container metadata
needs the bytes, and the bytes live in the browser's blob. A hash mismatch with no explanation is not
a verification, so this small sink closes the loop: POST the blob, diff it with the repo's own
container reader.
"""
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

OUT = sys.argv[2] if len(sys.argv) > 2 else '/tmp/ab'
# The destination is named by the REQUEST (/save/<name>), not fixed at startup: the first version had
# one fixed path and silently overwrote an export that had not been compared yet.


class Handler(BaseHTTPRequestHandler):
    def _cors(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Headers', '*')
        self.send_header('Access-Control-Allow-Methods', 'POST, OPTIONS')

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n) if n else b''
        name = self.path.rstrip('/').split('/')[-1] or 'uploaded.bin'
        # no traversal: the name is a bare filename or nothing
        if not all(c.isalnum() or c in '._-' for c in name) or name.startswith('.'):
            self.send_response(400); self._cors(); self.end_headers()
            self.wfile.write(b'bad name')
            return
        dest = os.path.join(OUT, name)
        with open(dest, 'wb') as fh:
            fh.write(body)
        self.send_response(200)
        self._cors()
        self.send_header('Content-Type', 'text/plain')
        self.end_headers()
        self.wfile.write(f'saved {len(body)} bytes to {dest}'.encode())
        print(f'received {len(body)} bytes -> {dest}', flush=True)

    def log_message(self, *a):
        pass


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8898
    print(f'blob sink on {port} -> {OUT}/<name from the request path>', flush=True)
    HTTPServer(('127.0.0.1', port), Handler).serve_forever()
