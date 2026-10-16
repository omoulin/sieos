#!/usr/bin/env python3
"""/bin/python (MicroPython, docs/python.md): boot SIEOS without a window,
then as alice on the serial console: python -c with integers, floats and
strings; a script (tests/python/files.py, put in /tmp/t) that writes and
reads files, lists folders, imports a module beside it and uses json, re,
struct, binascii, hashlib, random, collections, time and math; deep
recursion and a growing heap; an uncaught exception's traceback and exit
status; the interactive prompt with a function typed on several lines;
sockets: HTTP from a server on the host, TLS 1.3 (openssl s_server), and a
listening socket reached from the host.
Usage: pythontest.py <cpus> <disk image> <qemu command line...> (make py-test)

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import http.server, os, select, shutil, socket, subprocess, sys, tempfile, threading, time
import qserial                                      # (tools/qserial.py: typing on the console)
SLOW = float(os.environ.get("SIEOS_SLOW", "1"))     # emulated processors: everything takes longer

cpus, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
copy = image + ".py"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]

# On the host: a web server (SIEOS sees the host as 10.0.2.2), a TLS one,
# and a port forwarded to SIEOS's port 8080 (net.py serve).
www = tempfile.mkdtemp()
open(os.path.join(www, "hello.txt"), "w").write("hello from the host\n")
class Quiet(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=www, **k)
    def log_message(self, *a): pass
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
threading.Thread(target=srv.serve_forever, daemon=True).start()
def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
wport, tport, fport = srv.server_address[1], free_port(), free_port()
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "k.pem", "-out", "c.pem",
                "-days", "2", "-subj", "/CN=10.0.2.2"], cwd=www, check=True, capture_output=True)
tls = subprocess.Popen(["openssl", "s_server", "-accept", f"127.0.0.1:{tport}", "-www", "-tls1_3", "-cert", "c.pem",
                        "-key", "k.pem"], cwd=www, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
cmdline = [a.replace("user,id=n0", f"user,id=n0,hostfwd=tcp:127.0.0.1:{fport}-:8080") for a in cmdline]

qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
import atexit
atexit.register(lambda: (qemu.poll() is None and qemu.kill(), tls.kill()))
out = b""
fails = 0

def expect(text, timeout=30):
    """Wait for text; return what came before it."""
    timeout *= SLOW
    global out
    end = time.time() + timeout
    while text.encode() not in out:
        left = end - time.time()
        if left <= 0 or qemu.poll() is not None:
            qemu.kill()
            sys.exit(f"FAIL: waiting for {text!r}\n--- output ---\n{out.decode(errors='replace')[-3000:]}")
        if select.select([qemu.stdout], [], [], left)[0]:
            out += qemu.stdout.read1(65536)
    i = out.index(text.encode())
    before, out = out[:i], out[i + len(text):]
    return before.decode(errors="replace")

def send(line):
    qserial.type_line(qemu, line)

def run(cmd, timeout=60):
    """A command at alice's prompt -> its output (the prompt ends in "$ ")."""
    send(cmd)
    return expect("$ ", timeout)

def ok(name, cond, detail=""):
    global fails
    print(f"{'ok' if cond else 'FAIL'}: {name}{(' (' + detail + ')') if detail else ''}")
    fails += not cond

def short(r):
    return " | ".join(l.strip() for l in r.strip().splitlines()[1:])[-600:]

expect(f"mk: {cpus} CPU")
expect("first start")
for p in ("New password", "Again: "): expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice")
for p in ("New password", "Again: "): expect(p); send("alicepass1")
expect("login: "); send("alice")
expect("Password: "); send("alicepass1")
expect("alice@sieos:"); expect("$ ")
run("cd /tmp/t")

r = run("python -c print(1+2, 7//2, 2**100, -7 % 3, 'ab'*3, 'SIEOS'.lower(), len('héllo'))")
ok("python -c: integers, big integers, strings", "3 3 1267650600228229401496703205376 2 ababab sieos 5" in r, short(r))
r = run("python -c import math; print(7/2, 0.1+0.2, 1e300*1e10, math.sqrt(2), math.sin(math.pi/2), round(2.5), '%.3f' % math.e, float('1.5e-3'))")
ok("python -c: floats and math", "3.5 0.30000000000000004 inf 1.4142135623730951 1.0 2 2.718 0.0015" in r, short(r))
r = run("python -c print(5e-324, 1e16, 1e-5, 1e15, -0.0, 2/3, 2**0.5, '%.2f %e %g' % (3.14159, 12345.678, 0.0001), float('nan'))")
ok("python -c: floats as text (the shortest that reads back)", "5e-324 1e+16 1e-05 1000000000000000.0 -0.0 0.6666666666666666 "
   "1.4142135623730951 3.14 1.234568e+04 0.0001 nan" in r, short(r))
r = run("python -c import sys; print(sys.platform, sys.argv, sys.implementation.name)")
ok("python -c: sys", "sieos ['-c'] micropython" in r, short(r))
r = run("python --version")
ok("python --version", "MicroPython v1.29.0" in r, short(r))

r = run("python files.py one two", 120)
want = ["argv ['one', 'two']", "lines ['line 0\\n', 'line 1\\n', 'line 2\\n']", "size 26", "seek b'0\\nline' 11",
        "listdir ['moved.txt'] False", "json True {'a': [1, 'x']}", "re a<1>b<22>c<333> sieos",
        "struct b'\\x01\\x00\\x02\\x00\\x00\\x00' (256,)", "binascii b'01ab' b'U0lFT1M=\\n'",
        "sha256 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "oserror True",
        "random True 5", "collections 1 P(x=1, y=2)", "slept True True", "math 1.4142135623730951 -3 3.0 3.142",
        "import 42", "done False"]
missing = [w for w in want if w not in r]
ok("a script: files, folders, import, json, re, struct, binascii, hashlib, random, collections, time",
   not missing and "Traceback" not in r, "missing " + repr(missing) + ": " + short(r) if missing else "")

r = run("python deep.py", 120)
ok("recursion 500 deep, too deep -> RuntimeError; the heap grows (GC)", "deep 500" in r and "limit RuntimeError" in r
   and "heap 100000 True 20000000 True" in r, short(r))

r = run("python /src/python/primes.py 1000")
ok("the example /src/python/primes.py", "168 primes below 1000" in r, short(r))
r = run("python /src/python/words.py /etc/motd")
ok("the example /src/python/words.py", "/etc/motd:" in r and "most common:" in r, short(r))

r = run("python exc.py")
ok("an exception: traceback, exit status 1", "Traceback (most recent call last):" in r and 'File "exc.py", line 7, in <module>' in r
   and 'File "exc.py", line 6, in outer' in r and 'File "exc.py", line 4, in inner' in r
   and "ZeroDivisionError: divide by zero" in r and "python: exit status 1" in r, short(r))
r = run("python -c import sys; sys.exit(3)")
ok("sys.exit(3): exit status 3", "python: exit status 3" in r, short(r))
r = run("python nothing.py")
ok("a missing script: OSError", "OSError" in r and "exit status 1" in r, short(r))

# the interactive prompt
send("python")
expect(">>> ")
send("def f(x):"); expect("... ")
send("    y = x * 2"); expect("... ")
send("    return y"); expect("... ")
send(""); expect(">>> ")
send("f(21), [i * i for i in range(5)]")
r = expect(">>> ")
ok("the prompt: a function on several lines", "(42, [0, 1, 4, 9, 16])" in r, r.strip())
send("for i in range(3):"); expect("... ")
send("    print('n', i)"); expect("... ")
send("")
r = expect(">>> ")
ok("the prompt: a loop", "n 0" in r and "n 2" in r, r.strip())
send("1/0")
r = expect(">>> ")
ok("the prompt: an exception, then it goes on", "ZeroDivisionError" in r, r.strip())
send("name = input('who? ')"); expect("who? ")
send("SIEOS"); expect(">>> ")
send("print('hi', name)")
r = expect(">>> ")
ok("the prompt: input()", "hi SIEOS" in r, r.strip())
qemu.stdin.write(b"\x04"); qemu.stdin.flush()     # Ctrl-D: the end
expect("$ ")
ok("Ctrl-D leaves python", True)

# sockets
r = run(f"python net.py 10.0.2.2 {wport} {tport}", 120)
ok("socket: HTTP from the host, TLS 1.3 to openssl s_server", "http b'HTTP/1.0 200 OK' b'hello from the host\\n'" in r
   and "https b'HTTP/1.0 200 ok'" in r, short(r))
send("python net.py serve 8080 &")
expect("listening")
got = b""
for _ in range(int(20 * SLOW)):
    try:
        c = socket.create_connection(("127.0.0.1", fport), timeout=10 * SLOW)
        c.sendall(b"shout this")
        got = c.recv(100)
        c.close()
        if got: break
    except OSError:
        time.sleep(0.5)
r = expect("served")
ok("socket: listen and accept (the host connects)", got == b"SHOUT THIS", repr(got))
run("")

send("exit")
time.sleep(0.5)
qemu.kill()
print("PASS" if not fails else f"FAIL: {fails} check(s)")
sys.exit(1 if fails else 0)
