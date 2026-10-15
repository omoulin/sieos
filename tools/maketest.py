#!/usr/bin/env python3
"""make on SIEOS (/bin/make): boot SIEOS without a window, then as alice on
the serial console: copy /src/hello-make (a program in three files) to
/tmp/t, build it with make and /bin/cc, run it; nothing to do the second
time; a touched file rebuilds only what needs it; make -n shows and does
not run; functions, VAR=value, -B, -C, clean; a failing recipe's error,
-k and '-'. Then SIEOS's own hello, sh and make (/src/sieos) built on
SIEOS, and that make building hello-make again.
Usage: maketest.py <cpus> <disk image> <qemu command line...> (make make-test)

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import select, shutil, subprocess, sys, time
import qserial                                      # (tools/qserial.py: typing on the console)
SLOW = float(__import__("os").environ.get("SIEOS_SLOW", "1"))   # emulated processors: everything takes longer

cpus, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
copy = image + ".make"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
import atexit
atexit.register(lambda: qemu.poll() is None and qemu.kill())
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

def run(cmd, timeout=120):
    """A command at alice's prompt -> its output (the prompt ends in "$ ")."""
    send(cmd)
    return expect("$ ", timeout)

def ok(name, cond, detail=""):
    global fails
    print(f"{'ok' if cond else 'FAIL'}: {name}{(' (' + detail + ')') if detail else ''}")
    fails += not cond

def short(r):
    return " | ".join(l.strip() for l in r.strip().splitlines()[1:])[-300:]

expect(f"mk: {cpus} CPU")
expect("first start")
for p in ("New password", "Again: "): expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice")
for p in ("New password", "Again: "): expect(p); send("alicepass1")
expect("login: "); send("alice")
expect("Password: "); send("alicepass1")
expect("alice@sieos:"); expect("$ ")

src = "/src/hello-make/"
run("cd /tmp/t")
run("mkdir hm")
r = run("cp " + " ".join(src + f for f in ("Makefile", "main.c", "util.c", "util.h")) + " hm")
ok("cp the project to /tmp/t/hm", "error" not in r and "denied" not in r, short(r))
run("cd hm")
r = run("make", 300)
ok("make: compiles both files, links", "/bin/cc -O2 -c main.c -o main.o" in r and "/bin/cc -O2 -c util.c -o util.o" in r
   and "/bin/cc main.o util.o -o hello" in r and "Error" not in r, short(r))
r = run("./hello")
ok("./hello runs", "hello from make: dear SIEOS, sum 1..10 = 55" in r, short(r))
r = run("make")
ok("make again: nothing to do", "make: Nothing to be done for 'all'." in r and "cc" not in r, short(r))
r = run("make hello")
ok("make hello: up to date", "make: 'hello' is up to date." in r, short(r))
run("touch util.c")
r = run("make", 300)
ok("util.c touched: util.o and hello only", "-c util.c -o util.o" in r and "-o hello" in r and "main.c" not in r, short(r))
run("touch main.c")
r = run("make -n")
ok("make -n: shows main.o and hello", "-c main.c -o main.o" in r and "-o hello" in r and "util.c" not in r, short(r))
r = run("make -n")
ok("make -n ran nothing (the same again)", "-c main.c -o main.o" in r, short(r))
r = run("make", 300)
ok("make: then builds them", "-c main.c -o main.o" in r and "-o hello" in r and "Error" not in r, short(r))
r = run("make info")
ok("functions: wildcard, patsubst, notdir, addprefix, subst",
   "sources: main.c util.c, objects: [main] [util]" in r and "in hm: ./hello built with cc" in r, short(r))
r = run("make -B CFLAGS=-O0 util.o", 300)
ok("-B and VAR=value on the command line", "/bin/cc -O0 -c util.c -o util.o" in r, short(r))
r = run("make clean")
r2 = run("ls")
ok("make clean (rm -f)", "rm -f main.o util.o hello" in r and "main.o" not in r2 and "hello" not in r2.replace("hello@", ""), short(r2))
run("cd /tmp/t")
r = run("make -C hm -s", 300)
ok("make -C hm -s: built, silently", "Entering" not in r and "cc" not in r and "Error" not in r, short(r))
r = run("hm/hello make")
ok("hm/hello", "dear make" in r, short(r))

r = run("make -f fail.mk", 120)
ok("a failing recipe: GNU's message, make's status 2", "make: *** [bad] Error 1" in r and "ok1 ran" in r and
   "ok2 ran" not in r and "make: exit status 2" in r, short(r))
r = run("make -k -f fail.mk", 120)
ok("-k: the rest is made", "make: *** [bad] Error 1" in r and "ok2 ran" in r and
   "make: Target 'all' not remade because of errors." in r, short(r))
r = run("make -f fail.mk ignored", 120)
ok("'-': an error ignored", "Error 1 (ignored)" in r and "after the ignored error" in r and "exit status" not in r, short(r))
r = run("make -f fail.mk nothing")
ok("no rule for a target", "make: *** No rule to make target 'nothing'.  Stop." in r, short(r))

# SIEOS's own programs, built on SIEOS
run("mkdir sv")
run("cp /src/sieos/Makefile /src/sieos/hello.c /src/sieos/sh.c /src/sieos/make.c sv")
run("cd sv")
r = run("make", 600)
ok("SIEOS's hello, sh and make built by make", "cc -O2 hello.c -o hello" in r and "cc -O2 make.c -o make" in r
   and "Error" not in r and "error" not in r, short(r))
r = run("./hello a b")
ok("./hello (built on SIEOS)", "Hello from the disk!" in r and "2 arguments: a b" in r, short(r))
r = run("./sh -c echo this sh was built on SIEOS")
ok("./sh -c (built on SIEOS)", "this sh was built on SIEOS" in r, short(r))
r = run("./make -C ../hm -B", 300)
ok("./make (built on SIEOS) builds hello-make", "-c main.c -o main.o" in r and "-o hello" in r and "Error" not in r, short(r))
r = run("../hm/hello again")
ok("... and that hello runs", "dear again" in r, short(r))
send("exit")
time.sleep(0.5)
qemu.kill()
print("PASS" if not fails else f"FAIL: {fails} check(s)")
sys.exit(1 if fails else 0)
