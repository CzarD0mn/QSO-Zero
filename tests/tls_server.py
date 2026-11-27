#!/usr/bin/env python3
"""Tiny HTTPS server for test_uplink: logs each request line, replies 200."""
import http.server, ssl, sys
port, cert, key, log = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        open(log, 'a').write(self.path + '\n')
        self.send_response(200); self.end_headers(); self.wfile.write(b'OK')
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        open(log, 'a').write('POST ' + self.path + ' ' + body.decode() + '\n')
        self.send_response(200); self.end_headers(); self.wfile.write(b'RESULT=OK&LOGID=7')
    def log_message(self, *a): pass
srv = http.server.HTTPServer(('127.0.0.1', port), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print('ready', flush=True)
srv.serve_forever()
