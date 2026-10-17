"""
tests/webdeps/servers.py - The web servers wtest-soup talks to, on the host
(QEMU's user network shows it as 10.0.2.2), for tools/ktest.py webdeps:
  plain   HTTP/1.1
  tls     HTTP/1.1 over TLS 1.3 (ALPN http/1.1), a certificate from a test CA
          that start() adds to the disk's /etc/ssl/roots
  h2      HTTP/2 over TLS 1.3 (ALPN h2): a small server written here (no
          HPACK decoding needed: every request gets the same answer)
  self    HTTP/1.1 over TLS, a self-signed certificate (to be refused)
The HTTP/1.1 paths: /small, /big (1 MiB, byte i = i * 7 % 251), POST /echo
("got N sum S"). start() returns wtest-soup's arguments: the four ports.

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import http.server, os, socket, ssl, struct, subprocess, tempfile, threading

BIG = bytes(i * 7 % 251 for i in range(1 << 20))

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def answer(self, body):
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        if self.path == "/big": return self.answer(BIG)
        self.answer(b"hello from the host")
    def do_POST(self):
        d = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.answer(f"got {len(d)} sum {sum(d)}".encode())

def serve(ctx):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    if ctx: srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1]

def frame(kind, flags, sid, payload=b""):
    return struct.pack(">I", len(payload))[1:] + bytes([kind, flags]) + struct.pack(">I", sid) + payload

def h2_conn(c):
    """One HTTP/2 connection: settings, pings, and for each request
    :status 200 with a body (GET: 20000 bytes of "h2 "; with a body: its size)."""
    def recv(n):
        b = b""
        while len(b) < n:
            d = c.recv(n - len(b))
            if not d: raise EOFError
            b += d
        return b
    def answer(sid, body):
        c.sendall(frame(1, 4, sid, b"\x88"))                    # HEADERS, END_HEADERS: :status 200
        for i in range(0, len(body), 16384):
            c.sendall(frame(0, 1 if i + 16384 >= len(body) else 0, sid, body[i:i + 16384]))
    try:
        recv(24)                                                # the client's preface
        c.sendall(frame(4, 0, 0))
        got = {}
        while True:
            h = recv(9)
            n, kind, flags, sid = int.from_bytes(h[:3], "big"), h[3], h[4], int.from_bytes(h[5:], "big") & 0x7fffffff
            p = recv(n)
            if kind == 4 and not flags & 1: c.sendall(frame(4, 1, 0))
            elif kind == 6 and not flags & 1: c.sendall(frame(6, 1, 0, p))
            elif kind == 7: break
            elif kind == 1:
                got[sid] = b""
                if flags & 1: answer(sid, b"h2 " * 6667)
            elif kind == 0:
                d = p[1:len(p) - p[0]] if flags & 8 else p
                got[sid] = got.get(sid, b"") + d
                if n: c.sendall(frame(8, 0, 0, struct.pack(">I", n)) + frame(8, 0, sid, struct.pack(">I", n)))
                if flags & 1: answer(sid, f"got {len(got[sid])} sum {sum(got[sid])}".encode())
    except (EOFError, OSError):
        pass
    c.close()

def serve_h2(ctx):
    ls = socket.socket(); ls.bind(("127.0.0.1", 0)); ls.listen(8)
    def loop():
        while True:
            c, _ = ls.accept()
            try: c = ctx.wrap_socket(c, server_side=True)
            except (ssl.SSLError, OSError): c.close(); continue
            threading.Thread(target=h2_conn, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()
    return ls.getsockname()[1]

def start(image, siefs):
    d = tempfile.mkdtemp()
    def ossl(*a): subprocess.run(["openssl", *a], cwd=d, check=True, capture_output=True)
    ossl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "30",
         "-subj", "/CN=SIEOS test CA", "-addext", "basicConstraints=critical,CA:TRUE")
    ossl("req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-keyout", "leaf.key",
         "-out", "leaf.csr", "-subj", "/CN=test")
    open(os.path.join(d, "ext"), "w").write("subjectAltName=IP:10.0.2.2\n")
    ossl("x509", "-req", "-in", "leaf.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
         "-out", "leaf.pem", "-days", "30", "-extfile", "ext")
    ossl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "self.key", "-out", "self.pem", "-days", "30",
         "-subj", "/CN=self", "-addext", "subjectAltName=IP:10.0.2.2")
    ca = subprocess.run(["openssl", "x509", "-in", "ca.pem", "-outform", "DER"], cwd=d, capture_output=True, check=True).stdout
    roots = subprocess.run([siefs, image, "cat", "/etc/ssl/roots"], capture_output=True).stdout
    open(os.path.join(d, "roots"), "wb").write(roots + ca)
    subprocess.run([siefs, image, "put", os.path.join(d, "roots"), "/etc/ssl/roots"], check=True)
    def ctx(name, alpn):
        c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        c.minimum_version = ssl.TLSVersion.TLSv1_3
        c.load_cert_chain(os.path.join(d, name + ".pem"), os.path.join(d, name + ".key"))
        c.set_alpn_protocols([alpn])
        return c
    ports = (serve(None), serve(ctx("leaf", "http/1.1")), serve_h2(ctx("leaf", "h2")), serve(ctx("self", "http/1.1")))
    return " ".join(map(str, ports))
