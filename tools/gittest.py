#!/usr/bin/env python3
"""git on SIEOS (/bin/git, docs/git.md) against the host's git. On the host:
a bare repository with history (packed, with deltas), served by
"git daemon" (git://, pushes allowed) and by "git http-backend" behind a
small HTTP server here (smart HTTP; pushing needs a password). In SIEOS, as
alice: clone, log, status, diff, add, commit, push; the host checks the
pushed commit (git fsck); a host commit pulled (fast-forward), changes on
both sides merged (different files) and refused (the same file);
branches; then the same over HTTP with a password asked, then one from the
configuration. Last, repositories made by SIEOS (a git init one, and the
clone with the packs and indexes SIEOS wrote) are copied out of the disk
and checked by the host's git fsck.
Usage: gittest.py <cpus> <host tools dir> <disk image> <qemu command line...> (make git-test)

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import base64, http.server, os, select, shutil, socket, subprocess, sys, tempfile, threading, time
import qserial                                      # (tools/qserial.py: typing on the console)
SLOW = float(os.environ.get("SIEOS_SLOW", "1"))     # emulated processors: everything takes longer

cpus, hosttools, image, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
tmp = tempfile.mkdtemp(prefix="sieos-git-")
srv = os.path.join(tmp, "srv")
os.makedirs(srv)
env = dict(os.environ, HOME=tmp, GIT_CONFIG_NOSYSTEM="1", GIT_AUTHOR_NAME="Host", GIT_AUTHOR_EMAIL="host@example.org",
           GIT_COMMITTER_NAME="Host", GIT_COMMITTER_EMAIL="host@example.org")
fails = 0

def hg(*args, cwd=None, check=True):
    """The host's git -> its output."""
    r = subprocess.run(["git", *args], cwd=cwd, env=env, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f"host git {' '.join(args)} failed:\n{r.stdout}{r.stderr}")
    return r.stdout + r.stderr

def ok(name, cond, detail=""):
    global fails
    print(f"{'ok' if cond else 'FAIL'}: {name}{(' (' + detail + ')') if detail else ''}")
    fails += not cond

def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p

def write(path, text, mode=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    if mode:
        os.chmod(path, mode)

# ---- the host's repository: history worth packing with deltas
work = os.path.join(tmp, "work")
hg("init", "-q", "-b", "main", work)
main_c = "".join(f"int f{i}(int x) {{ return x * {i} + {i * i}; }}\n" for i in range(150))
write(f"{work}/README.md", "# proj\n\nA project for SIEOS's git.\n")
write(f"{work}/src/main.c", main_c)
write(f"{work}/tools/run.sh", "#!/bin/sh\necho run\n", 0o755)
write(f"{work}/.gitignore", "*.o\nbuild/\n")
hg("add", "-A", cwd=work); hg("commit", "-q", "-m", "First commit", cwd=work)
main_c += "int extra(void) { return 42; }\n"
write(f"{work}/src/main.c", main_c)
hg("commit", "-q", "-am", "Second: extra()", cwd=work)
write(f"{work}/README.md", "# proj\n\nA project for SIEOS's git.\nIt has a history.\n")
hg("commit", "-q", "-am", "Third: README", cwd=work)
hg("checkout", "-q", "-b", "feature", cwd=work)
write(f"{work}/feature.txt", "a feature\n")
hg("add", "feature.txt", cwd=work); hg("commit", "-q", "-m", "A feature", cwd=work)
hg("checkout", "-q", "main", cwd=work)
bare = os.path.join(srv, "proj.git")
hg("clone", "-q", "--bare", work, bare)
hg("repack", "-q", "-adf", "--depth=10", cwd=bare)
hg("config", "http.receivepack", "true", cwd=bare)
host_log = hg("log", "--oneline", "main", cwd=bare)

# ---- git daemon (git://) and smart HTTP (git http-backend as CGI, a password to push)
gport, hport = free_port(), free_port()
daemon = subprocess.Popen(start_new_session=True, args=["git", "daemon", "--export-all", "--enable=receive-pack", "--reuseaddr", f"--base-path={srv}",
                           "--listen=127.0.0.1", f"--port={gport}", srv], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
AUTH = "Basic " + base64.b64encode(b"alice:s3cret").decode()
asked = []

class Git(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def run(self, method):
        path, _, query = self.path.partition("?")
        if "git-receive-pack" in self.path and self.headers.get("Authorization") != AUTH:
            asked.append(self.path)
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="git"')
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))) if method == "POST" else b""
        e = dict(env, GIT_PROJECT_ROOT=srv, GIT_HTTP_EXPORT_ALL="1", PATH_INFO=path, QUERY_STRING=query,
                 REQUEST_METHOD=method, CONTENT_TYPE=self.headers.get("Content-Type", ""), CONTENT_LENGTH=str(len(body)),
                 REMOTE_USER="alice", REMOTE_ADDR="10.0.2.15")
        r = subprocess.run(["git", "http-backend"], input=body, env=e, capture_output=True).stdout
        head, _, data = r.partition(b"\r\n\r\n")
        status, headers = 200, []
        for line in head.decode().split("\r\n"):
            k, _, v = line.partition(": ")
            if k.lower() == "status": status = int(v.split()[0])
            elif k: headers.append((k, v))
        self.send_response(status)
        for k, v in headers: self.send_header(k, v)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def do_GET(self): self.run("GET")
    def do_POST(self): self.run("POST")

web = http.server.ThreadingHTTPServer(("127.0.0.1", hport), Git)
threading.Thread(target=web.serve_forever, daemon=True).start()

# ---- SIEOS
copy = image + ".git"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
import atexit
atexit.register(lambda: (qemu.poll() is None and qemu.kill(), os.killpg(daemon.pid, 9), web.shutdown(), shutil.rmtree(tmp, True)))
out = b""

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

def short(r):
    return " | ".join(l.strip() for l in r.strip().splitlines()[1:])[-400:]

expect(f"mk: {cpus} CPU")
expect("first start")
for p in ("New password", "Again: "): expect(p); send("rootpass1")
expect("user name"); send("alice")
expect("full name"); send("Alice")
for p in ("New password", "Again: "): expect(p); send("alicepass1")
expect("login: "); send("alice")
expect("Password: "); send("alicepass1")
expect("alice@sieos:"); expect("$ ")

run("git config --global user.name Alice Tester")
r = run("git config --global user.email alice@example.org")
r = run("git config user.name")
ok("git config --global, read back", "Alice Tester" in r, short(r))
run("cd /tmp/t")
url = f"git://10.0.2.2:{gport}/proj.git"
r = run(f"git clone {url}", 300)
ok("git clone git://", "Cloning into 'proj'" in r and "fatal" not in r and "* [new branch]      main -> origin/main" in r, short(r))
run("cd proj")
r = run("git log --oneline")
want = [l for l in host_log.splitlines()]
ok("git log --oneline: the host's history", all(l in r for l in want) and r.find(want[0]) < r.find(want[-1]), short(r))
r = run("git log -n 1")
ok("git log -n 1", "Author: Host <host@example.org>" in r and "Third: README" in r and "Date:" in r and "Second" not in r, short(r))
r = run("git status")
ok("git status: clean after the clone", "On branch main" in r and "nothing to commit, working tree clean" in r, short(r))
r = run("ls -l tools")
ok("run.sh checked out executable", "rwx" in r and "run.sh" in r, short(r))
r = run("ls -l src")
ok("src/main.c (built from deltas): its size", f" {len(main_c)} " in r, short(r))
r = run("git branch -a")
ok("git branch -a", "* main" in r and "remotes/origin/feature" in r, short(r))
r = run("git show HEAD~1")
ok("git show HEAD~1: a unified diff", "Second: extra()" in r and "+int extra(void) { return 42; }" in r and "@@ -148,3 +148,4 @@" in r, short(r))

run("echo hello from SIEOS >> README.md")
run("echo some notes > notes.txt")
run("cp src/main.c copy.c")
run("echo int sieos(void); >> copy.c")
run("echo junk > x.o")
r = run("git status -s")
ok("git status -s: modified, untracked (and *.o ignored)", " M README.md" in r and "?? notes.txt" in r and "x.o" not in r, short(r))
r = run("git diff")
ok("git diff", "diff --git a/README.md b/README.md" in r and "+hello from SIEOS" in r and "@@ -2,3 +2,4 @@" in r, short(r))
r = run("git add README.md notes.txt copy.c")
r = run("git status")
ok("git add; git status", "Changes to be committed:" in r and "modified:   README.md" in r and "new file:   notes.txt" in r, short(r))
r = run("git diff --cached")
ok("git diff --cached", "new file mode 100644" in r and "+some notes" in r and "+hello from SIEOS" in r, short(r))
r = run("git commit -m Edit from SIEOS")
ok("git commit -m", "[main " in r and "] Edit from SIEOS" in r, short(r))
r = run("git push", 300)
ok("git push (git://)", "main -> main" in r and "fatal" not in r, short(r))
fsck = hg("fsck", "--strict", cwd=bare, check=False)
ok("host: git fsck of the pushed-to repository", "error" not in fsck and "missing" not in fsck, fsck.strip()[-300:])
ok("host: the pushed commit", hg("log", "-1", "--format=%s|%an|%ae", "main", cwd=bare).strip() == "Edit from SIEOS|Alice Tester|alice@example.org"
   and "hello from SIEOS" in hg("show", "main:README.md", cwd=bare))

# a host commit, pulled (fast-forward); then changes on both sides (merged), then the same file (refused)
w2 = os.path.join(tmp, "w2")
hg("clone", "-q", bare, w2)
copy_c = main_c + "int sieos(void);\nint host(void) { return 7; }\n"   # (its old version is loose on the server: a delta base for a thin pack)
write(f"{w2}/copy.c", copy_c)
hg("commit", "-q", "-am", "Host change", cwd=w2); hg("push", "-q", cwd=w2)
r = run("git pull", 300)
ok("git pull: fast-forward", "Fast-forward" in r and "fatal" not in r, short(r))
r = run("git log --oneline -n 1")
r2 = run("ls -l")
ok("... the host's commit is here", "Host change" in r and f" {len(copy_c)} " in r2, short(r))
# a thin pack (deltas against objects only we have), from a minimal upload-pack here: host
# git's own pack-objects --thin (git daemon only sends them when its heuristics say so)
old = hg("rev-parse", "HEAD", cwd=w2).strip()
write(f"{w2}/copy.c", copy_c + "int thin(void);\n")
hg("commit", "-q", "-am", "Thin change", cwd=w2)
new = hg("rev-parse", "HEAD", cwd=w2).strip()
tport = free_port()
def thin_server():
    ls = socket.create_server(("127.0.0.1", tport))
    c, _ = ls.accept()
    f = c.makefile("rb")
    def pkt():
        n = int(f.read(4), 16)
        return f.read(n - 4) if n else None
    pkt()                                           # git-upload-pack /thin.git
    adv = f"{new} refs/heads/main\0ofs-delta thin-pack\n".encode()
    c.sendall(b"%04x" % (len(adv) + 4) + adv + b"0000")
    while (line := pkt()) is None or line != b"done\n": pass
    pack = subprocess.run(["git", "pack-objects", "--thin", "--stdout", "--revs", "-q"], input=f"{new}\n^{old}\n".encode(),
                          cwd=w2, env=env, capture_output=True).stdout
    c.sendall(b"0008NAK\n" + pack)
    c.close(); ls.close()
threading.Thread(target=thin_server, daemon=True).start()
run(f"git remote add thin git://10.0.2.2:{tport}/thin.git")
r = run("git fetch thin", 300)
ok("a thin pack: completed with our objects", "delta base(s) of ours added" in r and "main -> thin/main" in r, short(r))
r = run("git log --oneline -n 1 thin/main")
ok("... its commit", "Thin change" in r, short(r))
write(f"{w2}/tools/run.sh", "#!/bin/sh\necho run fast\n", 0o755)
hg("commit", "-q", "-am", "Host: run.sh", cwd=w2); hg("push", "-q", cwd=w2)
run("echo more notes >> notes.txt")
run("git commit -a -m SIEOS: notes")
r = run("git pull", 300)
ok("git pull: diverged, other files: a merge commit", "Merge made" in r and "fatal" not in r, short(r))
r = run("git push", 300)
ok("git push of the merge", "main -> main" in r and "fatal" not in r, short(r))
ok("host: the merge (2 parents, both changes)", len(hg("log", "-1", "--format=%P", "main", cwd=bare).split()) == 2
   and "run fast" in hg("show", "main:tools/run.sh", cwd=bare) and "more notes" in hg("show", "main:notes.txt", cwd=bare))
hg("pull", "-q", cwd=w2)
write(f"{w2}/README.md", "host version\n")
hg("commit", "-q", "-am", "Host: README", cwd=w2); hg("push", "-q", cwd=w2)
run("echo sieos version > README.md")
run("git commit -a -m SIEOS: README")
r = run("git pull", 300)
ok("git pull: the same file on both sides: refused", "CONFLICT: README.md changed on both sides" in r and "not merged" in r, short(r))
r = run("git push", 300)
ok("git push refused (not a fast-forward)", "not a fast-forward" in r, short(r))
r = run("git status -s")
ok("... and nothing was changed", len(r.strip().splitlines()) == 2, short(r))
run("echo scratch >> README.md")
run("git add README.md")
run("git reset README.md")
r = run("git status -s")
ok("git reset PATH: unstaged again", " M README.md" in r, short(r))
run("git checkout -- README.md")
r = run("git status -s")
ok("git checkout -- PATH: the file restored", len(r.strip().splitlines()) == 2, short(r))

r = run("git checkout -b topic")
r2 = run("git branch")
ok("git checkout -b", "Switched to branch 'topic'" in r and "* topic" in r2 and "  main" in r2, short(r2))
r = run("git checkout feature")
r2 = run("cat feature.txt")
ok("git checkout feature (origin's branch)", "set up to track 'origin/feature'" in r and "a feature" in r2, short(r))
r = run("git switch main")
r2 = run("ls")
ok("git switch main", "Switched to branch 'main'" in r and "feature.txt" not in r2, short(r2))
run("git branch -d topic")

# a repository made on SIEOS (checked by the host below)
run("cd /tmp/t")
r = run("git init fresh")
ok("git init", "Initialized empty Git repository in /tmp/t/fresh/.git/" in r, short(r))
run("cd fresh")
run("mkdir docs")
run("echo first line > a.txt")
run("echo inside > docs/b.txt")
run("echo binary > c.bin")
r = run("git status -s")
ok("git status -s (no commits yet)", "?? a.txt" in r and "?? docs/b.txt" in r, short(r))
run("git add -A")
r = run("git commit -m Made on SIEOS")
ok("first commit (root-commit)", "(root-commit)" in r, short(r))
run("echo second line >> a.txt")
run("git rm c.bin")
r = run("git commit -a -m Second on SIEOS")
r2 = run("git log --oneline")
ok("second commit, log", "Second on SIEOS" in r2 and "Made on SIEOS" in r2, short(r2))

# smart HTTP: clone, then a push that asks for the password, then one with it configured
run("cd /tmp/t")
hurl = f"http://10.0.2.2:{hport}/proj.git"
r = run(f"git clone {hurl} hproj", 300)
ok("git clone http://", "Cloning into 'hproj'" in r and "fatal" not in r, short(r))
run("cd hproj")
r = run("git log --oneline -n 1")
ok("... at the host's last commit", "Host: README" in r, short(r))
run("echo over http > http.txt")
run("git add http.txt")
run("git commit -m Over HTTP")
send("git push")
expect("Username for", 120); send("alice")
expect("Password for", 60); send("s3cret")
r = expect("$ ", 300)
ok("git push over HTTP, the password asked", "main -> main" in r and "fatal" not in r and asked, short(r))
run("git config credential.username alice")
run("git config credential.token s3cret")
run("echo again >> http.txt")
run("git commit -a -m Over HTTP with a token")
r = run("git push", 300)
ok("git push over HTTP, a token configured", "main -> main" in r and "Password" not in r, short(r))
fsck = hg("fsck", "--strict", cwd=bare, check=False)
ok("host: git fsck after the HTTP pushes", "error" not in fsck and "missing" not in fsck and
   hg("log", "-1", "--format=%s", "main", cwd=bare).strip() == "Over HTTP with a token", fsck.strip()[-300:])
run("sync")
send("exit")
time.sleep(1)
qemu.kill()
qemu.wait()

# the repositories SIEOS made, out of the disk, checked by the host's git
siefs = os.path.join(hosttools, "siefs")
def get_tree(src, dst):
    os.makedirs(dst, exist_ok=True)
    ls = subprocess.run([siefs, copy, "ls", src], capture_output=True, text=True).stdout
    for line in ls.splitlines():
        f = line.split(None, 5)
        if len(f) < 6 or f[5] in (".", ".."): continue
        if f[0].startswith("04"): get_tree(f"{src}/{f[5]}", os.path.join(dst, f[5]))
        else:
            subprocess.run([siefs, copy, "get", f"{src}/{f[5]}", os.path.join(dst, f[5])], check=True)
            if int(f[0], 8) & 0o100: os.chmod(os.path.join(dst, f[5]), 0o755)
for name in ("fresh", "proj"):
    d = os.path.join(tmp, "out-" + name)
    get_tree(f"/tmp/t/{name}", d)
    fsck = hg("fsck", "--strict", "--full", cwd=d, check=False)
    st = hg("status", "--porcelain", cwd=d, check=False)
    ok(f"host: git fsck of /tmp/t/{name} made by SIEOS", "error" not in fsck and "missing" not in fsck and "fatal" not in fsck, fsck.strip()[-300:])
    ok(f"host: git status reads its index: clean", st.strip() == "", st.strip()[-300:])
ok("host: SIEOS's commits in fresh", hg("log", "--format=%s", cwd=os.path.join(tmp, "out-fresh")).split("\n")[:2] == ["Second on SIEOS", "Made on SIEOS"])
packs = [f for f in os.listdir(os.path.join(tmp, "out-proj/.git/objects/pack")) if f.endswith(".idx")]
vp = "".join(hg("verify-pack", "-v", os.path.join(tmp, "out-proj/.git/objects/pack", p), check=False) for p in packs)
ok(f"host: git verify-pack of the {len(packs)} pack(s) SIEOS indexed", packs and "fatal" not in vp and "error" not in vp, vp.strip()[-200:])
os.remove(copy)
print("PASS" if not fails else f"FAIL: {fails} check(s)")
sys.exit(1 if fails else 0)
