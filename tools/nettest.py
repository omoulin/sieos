#!/usr/bin/env python3
"""The network test: boot SIEOS (first-start setup on a fresh disk), log in as
root, then: the DHCP lease and counters, ping, DNS, HTTP from a server on the
host (small file and a large download, checked byte for byte on the host
afterwards), a listening socket reached from the host, HTTPS to real sites
with valid certificates, and certificates that must be refused.

Usage: nettest.py <cpus> <host tools dir> <disk image> <qemu command line...>
(see make net-test). Needs the host's internet access for the HTTPS part
(skipped with NET_OFFLINE=1).

NET_NIC=CARD (QEMU's -device, e.g. e1000) replaces the command line's
network card, NET_MACHINE=q35 adds "-M q35" (x86-64), and NET_QUICK=1 checks
only the essentials on it: the card found, DHCP, ping, DNS, HTTP, HTTPS
(make net-nic-test runs it on each kind of card).

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import hashlib, http.server, os, re, select, shutil, socket, subprocess, sys, tempfile, threading, time
import qserial                                      # (tools/qserial.py: typing on the console)
SLOW = float(__import__("os").environ.get("SIEOS_SLOW", "1"))   # emulated processors (arm64 on a PC): everything takes longer

cpus, tools, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
copy = image + ".net"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
NIC, QUICK = os.environ.get("NET_NIC"), os.environ.get("NET_QUICK")
if NIC: cmdline = [NIC + ",netdev=n0" if "netdev=n0" in a else a for a in cmdline]
if os.environ.get("NET_MACHINE"): cmdline[1:1] = ["-M", os.environ["NET_MACHINE"]]

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
def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close()
    return p
fport, nport, uport = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
cmdline = [a.replace("user,id=n0", f"user,id=n0,hostfwd=tcp:127.0.0.1:{fport}-:8080,"
                     f"hostfwd=tcp:127.0.0.1:{nport}-:9000,hostfwd=udp:127.0.0.1:{uport}-:9001") for a in cmdline]

# For nc: a TCP and a UDP server on the host that answer in capitals.
def tcp_upper(ls):
    while True:
        c, _ = ls.accept()
        with c:
            while (d := c.recv(4096)): c.sendall(d.upper())
tls_ = socket.socket(); tls_.bind(("127.0.0.1", 0)); tls_.listen(4); eport = tls_.getsockname()[1]
threading.Thread(target=tcp_upper, args=(tls_,), daemon=True).start()
def udp_upper(us):
    while True:
        d, a = us.recvfrom(4096); us.sendto(d.upper(), a)
us_ = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); us_.bind(("127.0.0.1", 0)); ueport = us_.getsockname()[1]
threading.Thread(target=udp_upper, args=(us_,), daemon=True).start()


# TLS 1.3 servers on the host (openssl s_server), with certificates from a
# test CA that is added to the test disk's /etc/ssl/roots: a valid one (RSA:
# the server signs with RSA-PSS; forced AES-128-GCM; forced P-256, so the
# server first asks again: HelloRetryRequest), an ECDSA P-384 one, and ones
# that must be refused (expired, another name, unknown issuer); and TLS 1.2
# only servers (RSA; ECDSA P-384 with AES-256-GCM).
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
    servers.append(subprocess.Popen(["openssl", "s_server", "-accept", f"127.0.0.1:{p}", "-www",
                                     *([] if "-tls1_2" in opts else ["-tls1_3"]),
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
s_server("rsa12", "rsa", "-tls1_2")
s_server("p384_12", "p384", "-tls1_2", "-cipher", "ECDHE-ECDSA-AES256-GCM-SHA384")
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
    qserial.type_line(qemu, line)

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
card = re.search(r"vnet: card [^\r\n]*", b"".join(log).decode(errors="replace"))
print("   " + (card.group(0) if card else "vnet: (no card line)"))
if QUICK:
    check("fetch -p 10.0.2.2 3", "reply in")
    check(f"fetch http://10.0.2.2:{wport}/small.txt", "hello from the host")
    seen = check(f"fetch -o /tmp/big.bin http://10.0.2.2:{wport}/big.bin", "bytes in", timeout=120)
    print("   " + re.search(r"\d+ bytes in .*", seen).group(0).strip())
    if not os.environ.get("NET_OFFLINE"):
        check("host example.com", "example.com has address ")
        check("fetch -v https://example.com/", "HTTP 200", "cipher: TLS 1.", "Example Domain", timeout=60)
    check(f"fetch -v https://10.0.2.2:{tls_port['rsa']}/", "HTTP 200", "s_server", timeout=60)
    send("poweroff"); qemu.wait(SLOW * 10)
    srv.shutdown()
    for p in servers: p.kill()
    print(f"PASS ({NIC or 'default card'})")
    sys.exit(0)
check("fetch -p 10.0.2.2 3", "reply in")
check("fetch -r localhost", "127.0.0.1")
check(f"fetch http://10.0.2.2:{wport}/small.txt", "hello from the host")
seen = check(f"fetch -o /tmp/big.bin http://10.0.2.2:{wport}/big.bin", "bytes in", timeout=120)
m = re.search(r"(\d+) bytes in (\d+) ms \((\d+) KB/s\)", seen)
print(f"   download: {m.group(1)} bytes in {m.group(2)} ms = {int(m.group(3)) / 1024:.1f} MB/s")
check(f"fetch http://10.0.2.2:{wport}/missing", "404")

# a listening socket inside SIEOS (seen by netstat), reached from the host
send("fetch -l 8080 &"); expect("listening on port 8080")
check("netstat", "proto", "8080", "listen")
c = socket.create_connection(("127.0.0.1", fport), timeout=10)
c.sendall(b"GET / HTTP/1.1\r\nHost: sieos\r\n\r\n")
resp = b""
while True:
    d = c.recv(4096)
    if not d: break
    resp += d
c.close()
if b"hello from SIEOS" not in resp: sys.exit(f"FAIL: listen: {resp!r}")
expect("answered")
print("ok: fetch -l 8080, reached from the host")

# the network's commands: links to /bin/fetch
check("help", "ping", "ifconfig")
check("ifconfig", "nic0: the cable, link ", "address 10.0.2.15/255.255.255.0", "gateway 10.0.2.2", "DNS 10.0.2.3", "received ")
check("ping 10.0.2.2 -c 2", "10.0.2.2: reply in", "2 sent, 2 received")
check("host localhost", "localhost has address 127.0.0.1")
check("nslookup 10.0.2.3", "10.0.2.3 has address 10.0.2.3")
check("traceroute 10.0.2.2", " 1  10.0.2.2  ")
check("cd /tmp")
check(f"wget http://10.0.2.2:{wport}/small.txt", "bytes in")
check("cat small.txt", "hello from the host")
check(f"curl -o c.txt http://10.0.2.2:{wport}/small.txt", "bytes in")
check("cat c.txt", "hello from the host")
check("cd")
def ctrl_d(): qemu.stdin.write(b"\x04"); qemu.stdin.flush()
send(f"nc 10.0.2.2 {eport}"); time.sleep(SLOW * 0.5)
send("hello over tcp"); expect("HELLO OVER TCP")
ctrl_d(); expect(prompt)
print("ok: nc to a host's TCP server")
send(f"nc -u 10.0.2.2 {ueport}"); time.sleep(SLOW * 0.5)
send("hello over udp"); expect("HELLO OVER UDP")
ctrl_d(); expect(prompt)
print("ok: nc -u to a host's UDP server")
send("nc -l 9000"); time.sleep(SLOW * 1)
c = socket.create_connection(("127.0.0.1", nport), timeout=10)
c.sendall(b"from the host\n"); expect("from the host")
send("from sieos")
got = b""
while b"\n" not in got: got += c.recv(100)
if got != b"from sieos\n": sys.exit(f"FAIL: nc -l: the host got {got!r}")
c.close(); expect("the other side closed"); send(""); expect(prompt)
print("ok: nc -l 9000, from the host and back")
send("nc -l -u 9001"); time.sleep(SLOW * 1)
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.settimeout(10)
u.sendto(b"datagram from the host\n", ("127.0.0.1", uport)); expect("datagram from the host")
send("datagram back")
if u.recvfrom(100)[0] != b"datagram back\n": sys.exit("FAIL: nc -l -u: no answer")
ctrl_d(); expect(prompt)
print("ok: nc -l -u 9001, from the host and back")

if not os.environ.get("NET_OFFLINE"):
    check("host example.com", "example.com has address ")
    seen = check("fetch -v https://example.com/", "HTTP 200", "cipher: TLS 1.", "Example Domain", timeout=60)
    print("   " + re.search(r"cipher: [^\r\n]+", seen).group(0))
    check("fetch -v -o /tmp/hf.html https://huggingface.co/", "HTTP 200", "bytes in", timeout=60)

# our TLS servers on the host (10.0.2.2), certificates from the test CA
for tag, want in (("rsa", "TLS 1.3 TLS_CHACHA20_POLY1305_SHA256"), ("aes", "TLS 1.3 TLS_AES_128_GCM_SHA256"),
                  ("p256", "TLS 1.3 "), ("p384", "TLS 1.3 "), ("rsa12", "TLS 1.2 ECDHE-RSA-CHACHA20-POLY1305"),
                  ("p384_12", "TLS 1.2 ECDHE-ECDSA-AES256-GCM-SHA384")):
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
