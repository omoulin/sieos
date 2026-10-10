#!/usr/bin/env python3
"""The network test: boot SIEOS (first-start setup on a fresh disk), log in as
root, then: the DHCP lease and counters, ping, DNS, HTTP from a server on the
host (small file and a large download, checked byte for byte on the host
afterwards), a listening socket reached from the host, HTTPS to real sites
with valid certificates, and certificates that must be refused.

Usage: nettest.py <cpus> <host tools dir> <disk image> <qemu command line...>
(see make net-test). Needs the host's internet access for the HTTPS part
(skipped with NET_OFFLINE=1).

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import hashlib, http.server, os, re, select, shutil, socket, subprocess, sys, tempfile, threading, time
SLOW = float(__import__("os").environ.get("SIEOS_SLOW", "1"))   # emulated processors (arm64 on a PC): everything takes longer

cpus, tools, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
copy = image + ".net"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]

# A web server on the host (QEMU's user network shows the host as 10.0.2.2),
# and a forwarded port to reach a listening socket inside SIEOS.
www = tempfile.mkdtemp()
open(os.path.join(www, "small.txt"), "w").write("hello from the host\n")
big = os.urandom(24 << 20)
open(os.path.join(www, "big.bin"), "wb").write(big)
class Quiet(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=www, **k)
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path != "/slow": return super().do_GET()
        self.send_response(200); self.send_header("Content-Length", "100000"); self.end_headers()
        try:
            for i in range(1000): self.wfile.write(b"x" * 100); self.wfile.flush(); time.sleep(SLOW * 0.05)
        except OSError: pass
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
threading.Thread(target=srv.serve_forever, daemon=True).start()
wport = srv.server_address[1]
s = socket.socket(); s.bind(("127.0.0.1", 0)); fport = s.getsockname()[1]; s.close()
cmdline = [a.replace("user,id=n0", f"user,id=n0,hostfwd=tcp:127.0.0.1:{fport}-:8080") for a in cmdline]


# TLS 1.3 servers on the host (openssl s_server), with certificates from a
# test CA that is added to the test disk's /etc/ssl/roots: a valid one (RSA:
# the server signs with RSA-PSS; forced AES-128-GCM; forced P-256, so the
# server first asks again: HelloRetryRequest), an ECDSA P-384 one, and ones
# that must be refused (expired, another name, unknown issuer).
tdir = tempfile.mkdtemp()
def ossl(*a): subprocess.run(["openssl", *a], cwd=tdir, check=True, capture_output=True)
ossl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "30",
     "-subj", "/CN=SIEOS Test CA", "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign")
def leaf(name, key, san, *extra):
    ossl("req", "-newkey", key, "-nodes", "-keyout", name + ".key", "-out", name + ".csr", "-subj", "/CN=test")
    open(os.path.join(tdir, name + ".ext"), "w").write("subjectAltName=" + san + "\n")
    ossl("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
         "-extfile", name + ".ext", "-out", name + ".pem", *extra)
leaf("rsa", "rsa:2048", "IP:10.0.2.2", "-days", "30")
ossl("genpkey", "-genparam", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-384", "-out", "p384.par")
leaf("p384", "ec:p384.par", "IP:10.0.2.2", "-days", "30")
leaf("old", "rsa:2048", "IP:10.0.2.2", "-not_before", "20200101000000Z", "-not_after", "20210101000000Z")
leaf("other", "rsa:2048", "DNS:other.example", "-days", "30")
ossl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "self.key", "-out", "self.pem", "-days", "30",
     "-subj", "/CN=self", "-addext", "subjectAltName=IP:10.0.2.2")
ca_der = subprocess.run(["openssl", "x509", "-in", "ca.pem", "-outform", "DER"], cwd=tdir, capture_output=True, check=True).stdout
roots = subprocess.run([os.path.join(tools, "siefs"), copy, "cat", "/etc/ssl/roots"], capture_output=True).stdout
open(os.path.join(tdir, "roots"), "wb").write(roots + ca_der)
subprocess.run([os.path.join(tools, "siefs"), copy, "put", os.path.join(tdir, "roots"), "/etc/ssl/roots"], check=True)
servers, tls_port = [], {}
def s_server(tag, cert, *opts):
    sk = socket.socket(); sk.bind(("127.0.0.1", 0)); p = sk.getsockname()[1]; sk.close()
    servers.append(subprocess.Popen(["openssl", "s_server", "-accept", f"127.0.0.1:{p}", "-www", "-tls1_3",
                                     "-cert", cert + ".pem", "-key", cert + ".key", *opts],
                                    cwd=tdir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    tls_port[tag] = p
s_server("rsa", "rsa")
s_server("aes", "rsa", "-ciphersuites", "TLS_AES_128_GCM_SHA256")
s_server("p256", "rsa", "-groups", "P-256")
s_server("p384", "p384")
s_server("old", "old")
s_server("other", "other")
s_server("self", "self")
time.sleep(SLOW * 0.5)

# sia's remote backend against a mock OpenAI-compatible server (tools/netmock.py),
# over HTTP and over TLS (the test CA's certificate), with an API key.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netmock, ssl
mock, mport = netmock.start(0, "sekret")
tmock, tport = netmock.start(0, "sekret")
tctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
tctx.minimum_version = ssl.TLSVersion.TLSv1_3
tctx.load_cert_chain(os.path.join(tdir, "rsa.pem"), os.path.join(tdir, "rsa.key"))
tmock.socket = tctx.wrap_socket(tmock.socket, server_side=True)

qemu, out, prompt, log = None, b"", "# ", []

def expect(text, timeout=30):
    timeout *= SLOW
    global out
    end = time.time() + timeout
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            qemu.kill()
            sys.exit(f"FAIL: waiting for {text!r}\n--- output ---\n{(b''.join(log) + out).decode(errors='replace')[-3000:]}")
        if select.select([qemu.stdout], [], [], left)[0]:
            out += qemu.stdout.read1(4096)
    i = out.index(text.encode())
    seen = out[:i + len(text)]
    log.append(seen)
    out = out[i + len(text):]
    return seen.decode(errors="replace")

def send(line):
    qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()

def check(cmd, *answers, timeout=30):
    timeout *= SLOW
    send(cmd)
    seen = ""
    for a in answers: seen += expect(a, timeout)
    seen += expect(prompt, timeout)
    print(f"ok: {cmd}")
    return seen

qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
expect("first start")
for p in ("New password", "Again: "): expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice Example")
for p in ("New password", "Again: "): expect(p); send("alicepass1")
expect("You can log in now")
expect("login: "); send("root"); expect("Password: "); send("rootpass1"); expect(prompt)
print("ok: logged in")

check("fetch -i", "address 10.0.2.15/255.255.255.0", "gateway 10.0.2.2", "DNS 10.0.2.3")
check("fetch -p 10.0.2.2 3", "reply in")
check("fetch -r localhost", "127.0.0.1")
check(f"fetch http://10.0.2.2:{wport}/small.txt", "hello from the host")
seen = check(f"fetch -o /tmp/big.bin http://10.0.2.2:{wport}/big.bin", "bytes in", timeout=120)
m = re.search(r"(\d+) bytes in (\d+) ms \((\d+) KB/s\)", seen)
print(f"   download: {m.group(1)} bytes in {m.group(2)} ms = {int(m.group(3)) / 1024:.1f} MB/s")
check(f"fetch http://10.0.2.2:{wport}/missing", "404")

# a listening socket inside SIEOS, reached from the host
send("fetch -l 8080"); expect("listening on port 8080")
c = socket.create_connection(("127.0.0.1", fport), timeout=10)
c.sendall(b"GET / HTTP/1.1\r\nHost: sieos\r\n\r\n")
resp = b""
while True:
    d = c.recv(4096)
    if not d: break
    resp += d
c.close()
if b"hello from SIEOS" not in resp: sys.exit(f"FAIL: listen: {resp!r}")
expect("answered"); expect(prompt)
print("ok: fetch -l 8080, reached from the host")

if not os.environ.get("NET_OFFLINE"):
    check("fetch -r example.com", "example.com: ")
    seen = check("fetch -v https://example.com/", "HTTP 200", "cipher: TLS_", "Example Domain", timeout=60)
    print("   " + re.search(r"cipher: (\S+)", seen).group(0))
    check("fetch -v -o /tmp/hf.html https://huggingface.co/", "HTTP 200", "bytes in", timeout=60)

# our TLS servers on the host (10.0.2.2), certificates from the test CA
for tag, want in (("rsa", "TLS_CHACHA20_POLY1305_SHA256"), ("aes", "TLS_AES_128_GCM_SHA256"),
                  ("p256", "TLS_"), ("p384", "TLS_")):
    check(f"fetch -v https://10.0.2.2:{tls_port[tag]}/", "HTTP 200", "cipher: " + want, "s_server", timeout=60)
for tag, why in (("old", "expired"), ("other", "another name"), ("self", "unknown issuer")):
    check(f"fetch https://10.0.2.2:{tls_port[tag]}/", "tls: 10.0.2.2:", why, timeout=60)
check(f"fetch -k https://10.0.2.2:{tls_port['self']}/", "s_server", timeout=60)

if os.path.exists(os.path.join(os.path.dirname(image), "siad.elf")):
    for kv in ("backend=remote", f"url=http://10.0.2.2:{mport}/v1", "api_model=mock-model", "api_key=sekret"):
        check(f"sia config {kv}")
        time.sleep(SLOW * 0.3)                   # siad restarts with the new settings
    check("sia info", "remote", "mock-model")
    check("sia hello there", "You said: hello there", "model mock-model", "é✓")
    print(f"   mock saw: {mock.requests[-1]['messages'][-1]}")
    send("sia")
    time.sleep(SLOW * 0.5)
    send("first question"); expect("You said: first question")
    send("second question"); expect("You said: second question")
    send(""); expect(prompt)
    m = mock.requests[-1]["messages"]
    if not any("You said: first question" in x.get("content", "") for x in m if x.get("role") == "assistant"):
        sys.exit(f"FAIL: the conversation was not resent: {m}")
    print(f"ok: sia conversation ({len(m)} messages resent, with the earlier answer)")
    check("sia config api_key=wrong"); time.sleep(SLOW * 0.3)
    check("sia are you there", "HTTP 401", "Incorrect API key")
    check("sia config api_key=sekret"); time.sleep(SLOW * 0.3)
    check(f"sia config url=https://10.0.2.2:{tport}/v1"); time.sleep(SLOW * 0.3)
    check("sia over tls", "You said: over tls")
    print(f"ok: sia over https ({len(tmock.requests)} request)")
else:
    print("skip: sia (no siad yet)")
# the network server restarts during a transfer: the program gets an error, not a hang
send(f"fetch -o /tmp/slow http://10.0.2.2:{wport}/slow &"); expect(prompt)
time.sleep(SLOW * 1.5)
check("svc restart netd")
expect("fetch:", 30)

time.sleep(SLOW * 2)
check("fetch -i", "address 10.0.2.15")
check(f"fetch http://10.0.2.2:{wport}/small.txt", "hello from the host")
print("ok: netd restarted: the transfer ended with an error, the network works again")
seen = check("svc", "vnet", "netd")
for l in seen.splitlines():
    if l.startswith(("vnet", "netd")): print("   " + " ".join(l.split()))
check("sync")
send("poweroff")
qemu.wait(SLOW * 10)
r = subprocess.run([os.path.join(tools, "siefs"), copy, "get", "/tmp/big.bin", copy + ".big"], capture_output=True)
got = open(copy + ".big", "rb").read() if os.path.exists(copy + ".big") else b""
if hashlib.sha256(got).digest() != hashlib.sha256(big).digest():
    sys.exit(f"FAIL: the downloaded file differs ({len(got)} bytes)")
print(f"ok: the {len(got)}-byte download is identical (sha256)")
os.remove(copy + ".big")
srv.shutdown()
for p in servers: p.kill()
print("PASS")
