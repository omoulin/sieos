#!/usr/bin/env python3
"""TLS (user/lib/tls.c, built on the host as tlsclient) against the host's
openssl s_server: TLS 1.2 with each cipher suite, RSA and ECDSA
certificates, X25519, P-256 and P-384, each kind of ServerKeyExchange signature,
with and without the extended master secret, a certificate request; TLS 1.3
still; and what must fail: a downgrade to 1.2 by someone in the middle (the
server's random says so), CBC-only and static-RSA-only servers, an unknown
issuer. Usage: tls.py TLSCLIENT

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, socket, subprocess, sys, tempfile, threading, time

client = os.path.abspath(sys.argv[1])
d = tempfile.mkdtemp()
def ossl(*a): subprocess.run(["openssl", *a], cwd=d, check=True, capture_output=True)
ossl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "2",
     "-subj", "/CN=SIEOS TLS Test CA", "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign")
ossl("x509", "-in", "ca.pem", "-outform", "DER", "-out", "roots")
for name, key in (("rsa", ["-newkey", "rsa:2048"]), ("p256", ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256"]),
                  ("p384", ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-384"])):
    ossl("req", *key, "-nodes", "-keyout", name + ".key", "-out", name + ".csr", "-subj", "/CN=test")
    open(os.path.join(d, "san"), "w").write("subjectAltName=DNS:tls.test\n")
    ossl("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
         "-extfile", "san", "-out", name + ".pem", "-days", "2")
ossl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "self.key", "-out", "self.pem", "-days", "2",
     "-subj", "/CN=self", "-addext", "subjectAltName=DNS:tls.test")

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def server(cert, opts):
    p = free_port()
    srv = subprocess.Popen(["openssl", "s_server", "-accept", f"127.0.0.1:{p}", "-www", "-cert", cert + ".pem",
                            "-key", cert + ".key", *opts], cwd=d, stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", p), 0.1).close(); break
        except OSError: time.sleep(0.05)
    return srv, p

def mitm(port, mode):
    """A man in the middle. "downgrade": takes TLS 1.3 out of the ClientHello's
    supported_versions; "sig": damages the ServerKeyExchange's signature;
    "record": damages the first encrypted record from the server."""
    ls = socket.socket(); ls.bind(("127.0.0.1", 0)); ls.listen(1)
    def recv_all(a, n):
        b = b""
        while len(b) < n and (x := a.recv(n - len(b))): b += x
        return b
    def pump(a, b, change=None):
        try:
            while True:
                h = recv_all(a, 5)
                if len(h) < 5: break
                body = bytearray(recv_all(a, h[3] << 8 | h[4]))
                if change: change(h, body)
                b.sendall(h + body)
        except OSError: pass
        for z in (a, b):
            try: z.shutdown(socket.SHUT_RDWR)
            except OSError: pass
    seen = {}
    def from_server(h, body):
        if mode == "sig" and h[0] == 22 and body[:1] == b"\x0c": body[-1] ^= 1
        if mode == "record" and h[0] == 23 and not seen: body[-1] ^= 1; seen[1] = 1
    def from_client(h, body):
        if mode == "downgrade" and h[0] == 22 and body[:1] == b"\x01":
            body[:] = body.replace(bytes.fromhex("002b0005040304"), bytes.fromhex("002b0005040303"))
    def run():
        c, _ = ls.accept()
        s = socket.create_connection(("127.0.0.1", port))
        threading.Thread(target=pump, args=(s, c, from_server), daemon=True).start()
        pump(c, s, from_client)
    threading.Thread(target=run, daemon=True).start()
    return ls.getsockname()[1]

T12 = ["-tls1_2"]
cases = [  # name, certificate, s_server options, expected output, proxy
    ("1.2 RSA AES-128-GCM x25519", "rsa", T12 + ["-cipher", "ECDHE-RSA-AES128-GCM-SHA256", "-groups", "X25519"],
     "TLS 1.2 ECDHE-RSA-AES128-GCM-SHA256"),
    ("1.2 RSA AES-256-GCM P-256", "rsa", T12 + ["-cipher", "ECDHE-RSA-AES256-GCM-SHA384", "-groups", "P-256"],
     "TLS 1.2 ECDHE-RSA-AES256-GCM-SHA384"),
    ("1.2 RSA ChaCha20", "rsa", T12 + ["-cipher", "ECDHE-RSA-CHACHA20-POLY1305"], "TLS 1.2 ECDHE-RSA-CHACHA20-POLY1305"),
    ("1.2 RSA PKCS#1 SHA-256", "rsa", T12 + ["-sigalgs", "RSA+SHA256"], "TLS 1.2 ECDHE-RSA-"),
    ("1.2 RSA PKCS#1 SHA-512", "rsa", T12 + ["-sigalgs", "RSA+SHA512"], "TLS 1.2 ECDHE-RSA-"),
    ("1.2 RSA-PSS SHA-384", "rsa", T12 + ["-sigalgs", "rsa_pss_rsae_sha384"], "TLS 1.2 ECDHE-RSA-"),
    ("1.2 ECDSA P-256 AES-128-GCM", "p256", T12 + ["-cipher", "ECDHE-ECDSA-AES128-GCM-SHA256"],
     "TLS 1.2 ECDHE-ECDSA-AES128-GCM-SHA256"),
    ("1.2 ECDSA P-256 AES-256-GCM P-256", "p256", T12 + ["-cipher", "ECDHE-ECDSA-AES256-GCM-SHA384", "-groups", "P-256"],
     "TLS 1.2 ECDHE-ECDSA-AES256-GCM-SHA384"),
    ("1.2 ECDSA P-256 ChaCha20", "p256", T12 + ["-cipher", "ECDHE-ECDSA-CHACHA20-POLY1305"],
     "TLS 1.2 ECDHE-ECDSA-CHACHA20-POLY1305"),
    ("1.2 ECDSA P-256 signing with SHA-384", "p256", T12 + ["-sigalgs", "ECDSA+SHA384"], "TLS 1.2 ECDHE-ECDSA-"),
    ("1.2 ECDSA P-384", "p384", T12, "TLS 1.2 ECDHE-ECDSA-"),
    ("1.2 P-384 key exchange", "rsa", T12 + ["-groups", "P-384"], "TLS 1.2 ECDHE-RSA-"),
    ("1.2 no extended master secret", "rsa", T12 + ["-no_ems"], "TLS 1.2 ECDHE-RSA-"),
    ("1.2 certificate request", "rsa", T12 + ["-verify", "1"], "TLS 1.2 ECDHE-RSA-"),
    ("1.2 server's preference", "rsa", T12 + ["-serverpref", "-cipher", "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256"],
     "TLS 1.2 ECDHE-RSA-AES256-GCM-SHA384"),
    ("1.3 ChaCha20", "rsa", ["-tls1_3"], "TLS 1.3 TLS_CHACHA20_POLY1305_SHA256"),
    ("1.3 AES-128-GCM", "p256", ["-tls1_3", "-ciphersuites", "TLS_AES_128_GCM_SHA256"], "TLS 1.3 TLS_AES_128_GCM_SHA256"),
    ("1.3 HelloRetryRequest for P-256", "p384", ["-tls1_3", "-groups", "P-256"], "TLS 1.3 "),
    ("1.3 HelloRetryRequest for P-384", "rsa", ["-tls1_3", "-groups", "P-384"], "TLS 1.3 "),
    ("1.3 certificate request", "rsa", ["-tls1_3", "-verify", "1"], "TLS 1.3 "),
    ("1.3 preferred over 1.2", "rsa", [], "TLS 1.3 "),
    ("downgrade to 1.2 refused", "rsa", [], "downgrade to TLS 1.2 refused", "downgrade"),
    ("1.2 damaged ServerKeyExchange: refused", "rsa", T12, "bad ServerKeyExchange", "sig"),
    ("1.2 damaged record: refused", "p256", T12, "got 0 bytes", "record"),
    ("CBC only: refused", "rsa", T12 + ["-cipher", "ECDHE-RSA-AES128-SHA256"], "handshake failed"),
    ("static RSA only: refused", "rsa", T12 + ["-cipher", "AES128-GCM-SHA256"], "handshake failed"),
    ("1.2 unknown issuer: refused", "self", T12, "unknown issuer"),
]
fails = 0
for name, cert, opts, want, *proxy in cases:
    srv, port = server(cert, opts)
    if proxy: port = mitm(port, proxy[0])
    r = subprocess.run([client, str(port), "tls.test", os.path.join(d, "roots")], capture_output=True, text=True, timeout=30)
    srv.kill(); srv.wait()
    ok = want in r.stdout and (r.returncode == 0) == want.startswith("TLS")
    if not ok: fails += 1
    print(("ok:   " if ok else "FAIL: ") + name + ("" if ok else "\n" + r.stdout + r.stderr))
print(f"tls: {len(cases) - fails} of {len(cases)} against openssl s_server passed")
sys.exit(fails != 0)
