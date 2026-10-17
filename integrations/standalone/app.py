# SPDX-License-Identifier: MIT
"""Local browser spectrum/waterfall application for the MT7961 receiver."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import secrets
import socket
import sys
import threading
from urllib.parse import parse_qs, urlsplit
import webbrowser
import zipfile

from receiver import Receiver, RESOURCE


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = os.name != 'nt'

    def server_bind(self):
        if os.name == 'nt':
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()

    def __init__(self, address, receiver):
        super().__init__(address, Handler)
        self.receiver = receiver
        self.token = secrets.token_urlsafe(32)
        self.origin = f'http://127.0.0.1:{self.server_port}'
        self.hosts = {f'127.0.0.1:{self.server_port}', f'localhost:{self.server_port}'}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send(self, code, data=b'', content='application/json'):
        self.send_response(code)
        self.send_header('Content-Type', content)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(data)

    def json(self, code, value):
        self.send(code, json.dumps(value, allow_nan=False).encode())

    def allowed(self):
        if self.headers.get('Host') not in self.server.hosts:
            self.json(403, {'error': 'Local host header required'})
            return False
        return True

    def do_GET(self):
        if not self.allowed():
            return
        path = urlsplit(self.path).path
        try:
            if path == '/api/state':
                self.json(200, self.server.receiver.state() | {'token': self.server.token})
            elif path == '/api/devices':
                self.json(200, self.server.receiver.devices())
            elif path == '/api/frame':
                try:
                    after = int(parse_qs(urlsplit(self.path).query).get('after', ['-1'])[0])
                except ValueError:
                    self.json(400, {'error': 'Invalid frame sequence'})
                    return
                with self.server.receiver.lock:
                    frame = self.server.receiver.frame
                if frame and int.from_bytes(frame[4:8], 'little') == after:
                    frame = None
                self.send(200 if frame else 204, frame or b'', 'application/octet-stream')
            elif path in ('/', '/app.js', '/style.css'):
                name, kind = {'/': ('index.html', 'text/html; charset=utf-8'),
                              '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
                              '/style.css': ('style.css', 'text/css; charset=utf-8')}[path]
                self.send(200, (RESOURCE / 'web' / name).read_bytes(), kind)
            else:
                self.json(404, {'error': 'Not found'})
        except Exception as error:
            self.json(500, {'error': str(error)})

    def do_POST(self):
        if not self.allowed():
            return
        if not secrets.compare_digest(self.headers.get('X-Birria-Token', ''), self.server.token):
            self.json(403, {'error': 'Invalid control token'})
            return
        origin = self.headers.get('Origin')
        if origin and origin not in {self.server.origin, f'http://localhost:{self.server.server_port}'}:
            self.json(403, {'error': 'Local origin required'})
            return
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 <= length <= 4096:
                raise ValueError('Request too large')
            body = json.loads(self.rfile.read(length) or b'{}')
            if not isinstance(body, dict):
                raise ValueError('Expected an object')
            path = urlsplit(self.path).path
            r = self.server.receiver
            if path == '/api/start':
                r.start()
            elif path == '/api/stop':
                r.stop()
            elif path == '/api/configure':
                r.configure(body)
            elif path == '/api/snapshot':
                iq, metadata = r.snapshot()
                buffer = io.BytesIO()
                with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as z:
                    z.writestr('capture.cs16', iq)
                    z.writestr('capture.json', json.dumps(metadata, indent=2))
                self.send(200, buffer.getvalue(), 'application/zip')
                return
            elif path == '/api/shutdown':
                r.stop()
                self.json(200, {'status': 'closed'})
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            else:
                self.json(404, {'error': 'Not found'})
                return
            self.json(200, r.state())
        except (ValueError, TypeError) as error:
            self.json(400, {'error': str(error)})
        except Exception as error:
            self.json(409, {'error': str(error)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--demo', action='store_true', help='Use synthetic signals without accessing USB')
    parser.add_argument('--port', type=int, default=8073, help='Local HTTP port, zero selects a free port')
    parser.add_argument('--no-browser', action='store_true')
    args = parser.parse_args()
    if not 0 <= args.port <= 65535:
        parser.error('Port must be between 0 and 65535')
    receiver = Receiver(demo=args.demo)
    try:
        server = Server(('127.0.0.1', args.port), receiver)
    except OSError as error:
        # An exclusive Windows listener reports WSAEACCES rather than
        # WSAEADDRINUSE when this server requests SO_REUSEADDR.
        if error.errno not in (13, 98, 10013, 10048) and getattr(error, 'winerror', None) not in (10013, 10048):
            receiver.close()
            raise
        server = Server(('127.0.0.1', 0), receiver)
        print(f'Port {args.port} is unavailable. Using port {server.server_port}.', flush=True)
    print(f'BirriaSDR {"DEMO" if args.demo else "MT7961"}: {server.origin}', flush=True)
    print('Use Stop to release the receiver. Use Exit app or Ctrl+C to close this server.', flush=True)
    if not args.no_browser:
        webbrowser.open(server.origin)
    try:
        server.serve_forever(poll_interval=.1)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        receiver.close()
        print('Receiver closed.', flush=True)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(f'Unable to run BirriaSDR: {error}', file=sys.stderr, flush=True)
        if getattr(sys, 'frozen', False) and '--no-browser' not in sys.argv:
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, str(error), 'BirriaSDR startup error', 0x10)
        sys.exit(1)
