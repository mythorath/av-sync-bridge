#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Serve only the two public reference assets on IPv4 loopback; no uploads."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

ASSETS = {
    '/': ('av-reference.html', 'text/html; charset=utf-8'),
    '/av-reference.html': ('av-reference.html', 'text/html; charset=utf-8'),
    '/reference-timeline.mjs': ('reference-timeline.mjs', 'text/javascript; charset=utf-8'),
}


def asset_for_path(path):
    """Exact allowlist: no decoding, directory listing, or arbitrary file paths."""
    try:
        parsed = urlsplit(path)
    except ValueError:
        return None
    if parsed.scheme or parsed.netloc:
        return None
    return ASSETS.get(parsed.path)


class ReferenceHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        asset = asset_for_path(self.path)
        if asset is None:
            self.send_error(404)
            return
        try:
            content = (Path(__file__).resolve().parent / asset[0]).read_bytes()
        except OSError:
            self.send_error(500, 'Reference asset unavailable')
            return
        self.send_response(200)
        self.send_header('Content-Type', asset[1])
        self.send_header('Content-Length', str(len(content)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        self.wfile.write(content)

    def log_message(self, format, *args):
        # No URL or request-header logging; no timing reports are accepted.
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8765)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error('port must be 1024..65535')
    with ThreadingHTTPServer(('127.0.0.1', args.port), ReferenceHandler) as server:
        print(f'Open http://127.0.0.1:{args.port}/av-reference.html; Ctrl+C stops this local server.', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
