#!/usr/bin/env python3
"""Бэкенд для бенчмарка: /status, /api/ping, /api/echo (POST), /big, /static/*.

Отдаёт RAW-тела без сжатия — gzip накладывают сами серверы (sparrow/caddy/nginx),
чтобы сравнивать именно их сжатие на лету, а не бэкенда.
"""
import os
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WWW = "/www"
BIG = os.path.join(WWW, "big.txt")

MIME = {".txt": "text/plain; charset=utf-8", ".json": "application/json",
        ".html": "text/html; charset=utf-8", ".bin": "application/octet-stream"}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(data)

    def _file(self, path, ctype=None):
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            self.send_error(404)
            return
        if ctype is None:
            ctype = MIME.get(os.path.splitext(path)[1], "application/octet-stream")
        self._send(200, data, ctype)

    def do_GET(self):
        path = re.sub(r"\?.*$", "", self.path)
        if path == "/status":
            self._send(200, '{"status":"ok","server":"backend"}')
        elif path == "/api/ping":
            self._send(200, '{"pong":true}')
        elif path == "/big":
            self._file(BIG)
        elif path.startswith("/static/"):
            name = os.path.basename(path[len("/static/"):])
            self._file(os.path.join(WWW, name))
        else:
            self.send_error(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        self._send(200, body, "application/octet-stream")


ThreadingHTTPServer(("0.0.0.0", 9090), Handler).serve_forever()
