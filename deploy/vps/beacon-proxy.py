#!/usr/bin/env python3
"""The public, read-only face of the gifter's chain: games take their beacon block from it.

Forwards only getLastBlockId and getBlock (JSON-RPC 2.0, single requests, small bodies) to the
sequencer on localhost; everything else gets 403. Usage: beacon-proxy.py LISTEN_PORT UPSTREAM_URL
"""
import json, sys, urllib.request
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

PORT, UPSTREAM = int(sys.argv[1]), sys.argv[2]
ALLOWED = {"getLastBlockId", "getBlock"}

class H(BaseHTTPRequestHandler):
    def reply(self, code, body):
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get("content-length") or 0)
        if n <= 0 or n > 512:
            return self.reply(413, b'{"error":"request too large"}')
        raw = self.rfile.read(n)
        try:
            req = json.loads(raw)
            ok = isinstance(req, dict) and req.get("method") in ALLOWED and isinstance(req.get("params", []), list)
        except ValueError:
            ok = False
        if not ok:
            return self.reply(403, b'{"error":"only getLastBlockId and getBlock"}')
        try:
            up = urllib.request.Request(UPSTREAM, data=raw, headers={"content-type": "application/json"})
            with urllib.request.urlopen(up, timeout=10) as r:
                self.reply(200, r.read(1 << 20))
        except Exception:
            self.reply(502, b'{"error":"chain unreachable"}')

    def log_message(self, *a):
        pass

ThreadingHTTPServer(("0.0.0.0", PORT), H).serve_forever()
