#!/usr/bin/env python3
"""The mail test: SIEOS's Mail window and /bin/mail against a mail server
on the host (tools/mailserver.py), over TLS with a certificate from a test
CA made here with openssl (added to the test disk's /etc/ssl/roots only).

  first start on the screen -> Mail from the sia strip: the account form ->
  IMAP (993) + SMTP with STARTTLS (587): the folders and their unread
  counts (STATUS), INBOX's 4 seeded messages (quoted-printable; base64
  multipart with an attachment; an encoded-word subject; HTML only),
  newest first, unread in bold -> one read (marked \\Seen on the server),
  its attachment saved in ~/Downloads -> a reply (checked byte for byte
  on the host: CRLF, dot-stuffing, Date, Message-ID, In-Reply-To; its copy
  in Sent by APPEND), a reply all (Ctrl+Shift+R: the others in Cc, not
  us), a new message with a file attached (picked in Files), a forward
  with the attachment (Ctrl+L), each compared with the original bytes ->
  a mailto: link from the sia strip -> delete: to the Trash (\\Trash),
  then from the Trash for good -> a favorite folder (the star, saved in
  ~/.mail) -> the command line: list, read (the decoding), folders,
  mailto: -> a second account on another server with a self-signed
  certificate: refused, then accepted (the SHA-256 shown, pinned for the
  incoming and the outgoing server), its folders found by name, a message
  written from it (its copy in "Sent Messages"); a changed certificate is
  refused -> POP3 (995) in the window: list, read, delete (left on the
  server, then deleted there) -> the command line again: POP3 with STLS
  (110), IMAP with STARTTLS (143), SMTP on 465 -> ~/.mail is only alice's
  -> a server limited to TLS 1.2: IMAP, SMTP.

The servers are reached from SIEOS at 10.0.2.100 (the first) and
10.0.2.101 (the second, self-signed) on the usual ports (QEMU's guestfwd
runs `mailserver.py relay PORT` for each connection).

Usage: mailtest.py <cpus> <screenshots dir> <disk image> <host tools dir> <qemu command line...>

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import atexit, base64, email, json, os, re, select, shutil, socket, ssl, subprocess, sys, tempfile, time
from email import policy
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mailserver, qserial
SLOW = float(os.environ.get("SIEOS_SLOW", "1"))
T0 = time.time()
def say(s): print(f"{s} [{time.time() - T0:.0f} s]", flush=True)

cpus, shots, image, tools, cmdline = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
os.makedirs(shots, exist_ok=True)
copy = image + ".mail"
shutil.copyfile(image, copy)
cmdline = [a.replace(image, copy) if a.startswith("file=") else a for a in cmdline]
here = os.path.dirname(os.path.abspath(__file__))

# ---- the test CA, the server's certificate (for 10.0.2.100), the roots
tdir = tempfile.mkdtemp()
def ossl(*a): subprocess.run(["openssl", *a], cwd=tdir, check=True, capture_output=True)
ossl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "30",
     "-subj", "/CN=SIEOS Mail Test CA", "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign")
ossl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", "srv.key", "-out", "srv.csr", "-subj", "/CN=mail.test")
open(os.path.join(tdir, "srv.ext"), "w").write("subjectAltName=IP:10.0.2.100\n")
ossl("x509", "-req", "-in", "srv.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-extfile", "srv.ext",
     "-out", "srv.pem", "-days", "30")
ca_der = subprocess.run(["openssl", "x509", "-in", "ca.pem", "-outform", "DER"], cwd=tdir, capture_output=True, check=True).stdout
roots = subprocess.run([os.path.join(tools, "siefs"), copy, "cat", "/etc/ssl/roots"], capture_output=True, check=True).stdout
open(os.path.join(tdir, "roots"), "wb").write(roots + ca_der)
subprocess.run([os.path.join(tools, "siefs"), copy, "put", os.path.join(tdir, "roots"), "/etc/ssl/roots"], check=True)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(os.path.join(tdir, "srv.pem"), os.path.join(tdir, "srv.key"))
def selfsigned(name):
    """A self-signed certificate (no CA knows it) -> (a server context, its SHA-256 as openssl shows it)."""
    ossl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", name + ".key", "-out", name + ".pem", "-days", "30", "-subj", "/CN=home.test")
    c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    c.load_cert_chain(os.path.join(tdir, name + ".pem"), os.path.join(tdir, name + ".key"))
    fp = subprocess.run(["openssl", "x509", "-in", name + ".pem", "-noout", "-fingerprint", "-sha256"], cwd=tdir, capture_output=True, check=True)
    return c, fp.stdout.decode().strip().split("=", 1)[1]
ctx2, fp2 = selfsigned("home")
ctx3, fp3 = selfsigned("home2")

# ---- the seeded messages (CRLF lines)
def crlf(s): return s.replace("\n", "\r\n").encode()
QP = crlf("""From: Alice Martin <alice.m@example.org>
To: alice@example.org
Subject: Lunch on Friday
Date: Mon, 12 Oct 2026 09:15:00 +0000
Message-ID: <qp-1@example.org>
MIME-Version: 1.0
Content-Type: text/plain; charset=utf-8
Content-Transfer-Encoding: quoted-printable

Shall we meet at the caf=C3=A9 at noon? This line is long enough to be wr=
apped by a soft line break.
=3D the end =3D
""")
B64 = ("""From: Bob <bob@example.org>
To: alice@example.org
Subject: Quarterly report
Date: Wed, 14 Oct 2026 16:40:00 +0000
Message-ID: <b64-2@example.org>
MIME-Version: 1.0
Content-Type: multipart/mixed; boundary="outer"

--outer
Content-Type: multipart/alternative; boundary="inner"

--inner
Content-Type: text/plain; charset=utf-8
Content-Transfer-Encoding: base64

""" + base64.encodebytes("Hello team,\nthe report is attached: 12 % better.\n".encode()).decode() + """
--inner
Content-Type: text/html; charset=utf-8

<p>Hello team (HTML)</p>
--inner--
--outer
Content-Type: application/pdf; name="report.pdf"
Content-Disposition: attachment; filename="report.pdf"
Content-Transfer-Encoding: base64

""" + base64.encodebytes(b"%PDF-1.4 not really\n" * 40).decode() + """--outer--
""")
B64 = crlf(B64)
EW = crlf("""From: =?ISO-8859-1?Q?Ren=E9e?= <renee@example.org>
To: alice@example.org, carol@example.org
Cc: "Doe, John" <john@example.org>, bob@example.org
Subject: =?UTF-8?B?""" + base64.b64encode("Réunion à 10h ✓".encode()).decode() + """?=
 =?UTF-8?Q?_demain?=
Date: Thu, 15 Oct 2026 08:00:00 +0200
Message-ID: <ew-3@example.org>

See you tomorrow.
""")
HTML = crlf("""From: News <news@example.org>
To: alice@example.org
Subject: Newsletter
Date: Fri, 9 Oct 2026 07:00:00 +0000
Content-Type: text/html; charset=iso-8859-1

<html><head><style>p { color: red }</style></head><body><p>Big&nbsp;news &amp; more</p><br>Caf\xe9 au lait</body></html>
""".replace("\xe9", "\x00")).replace(b"\x00", b"\xe9")
HOME1 = crlf("""From: Mum <mum@example.net>
To: bob@home.test
Subject: Sunday?
Date: Tue, 13 Oct 2026 18:00:00 +0000

Are you coming on Sunday?
""")
HOME2 = crlf("""From: Shop <orders@example.com>
To: bob@home.test
Subject: Your order has shipped
Date: Sun, 11 Oct 2026 10:00:00 +0000

Order #4411 has shipped.
""")
USER, PW = "alice@example.org", "s3cret pass"
USER2, PW2 = "bob@home.test", "home pass"
srv = mailserver.start(ctx, USER, PW, [(QP, False), (B64, False), (EW, False), (HTML, True)])
srv2 = mailserver.start(ctx2, USER2, PW2, [(HOME1, False), (HOME2, True)], {"Sent Messages": "", "Deleted Messages": ""})
fwd = {993: srv.port("imapd", True), 143: srv.port("imapd", False), 995: srv.port("pop3d", True),
       110: srv.port("pop3d", False), 465: srv.port("smtpd", True), 587: srv.port("smtpd", False)}
fwd2 = {993: srv2.port("imapd", True), 465: srv2.port("smtpd", True)}
relay = os.path.join(here, "mailserver.py")
gf = "".join(f",guestfwd=tcp:10.0.2.{ip}:{p}-cmd:{sys.executable} {relay} relay {hp} {tdir}/relay.log"
             for ip, f in ((100, fwd), (101, fwd2)) for p, hp in f.items())
cmdline = [a.replace("user,id=n0", "user,id=n0" + gf) for a in cmdline]

# ---- QEMU, its serial port and its control socket (QMP)
sock_path = os.path.join(shots, "qmp-mail.sock")
if os.path.exists(sock_path): os.unlink(sock_path)
cmdline += ["-qmp", f"unix:{sock_path},server=on,wait=off"]
qemu = subprocess.Popen(cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
atexit.register(lambda: qemu.poll() is None and qemu.kill())
full, pos = b"", {"con": 0, "log": 0}

def fail(msg):
    qemu.kill()
    open(os.path.join(shots, "mail-serial.log"), "wb").write(full)
    sys.exit(f"FAIL: {msg}\n--- serial (last part) ---\n{full[-4000:].decode(errors='replace')}\n--- server ---\n" + "\n".join(srv.log[-30:]) + "\n--- relay ---\n" + (open(os.path.join(tdir, "relay.log")).read()[-2000:] if os.path.exists(os.path.join(tdir, "relay.log")) else ""))

def pull(t):
    global full
    if select.select([qemu.stdout], [], [], t)[0]:
        full += qemu.stdout.read1(65536)

def wait(text, who="log", timeout=30, line=True):
    """Wait for text after reader who's position -> the rest of its line."""
    end = time.time() + timeout * SLOW
    while text.encode() not in full[pos[who]:]:
        if time.time() > end or qemu.poll() is not None: fail(f"waiting for {text!r}")
        pull(0.2)
    i = full.index(text.encode(), pos[who]) + len(text)
    while line and b"\n" not in full[i:] and time.time() < end: pull(0.2)
    e = full.find(b"\n", i)
    pos[who] = i
    return full[i:e if e >= 0 else len(full)].decode(errors="replace")

def drain(t=0.4):
    end = time.time() + t
    while time.time() < end: pull(0.05)

def send(line): qserial.type_line(qemu, line)

def sh(cmd, timeout=60):
    """A command in alice's shell on the serial console -> its output."""
    drain(0.3)
    start = pos["con"] = len(full)
    send(cmd)
    wait("alice@sieos", "con", timeout, line=False)
    return full[start:pos["con"]].decode(errors="replace")

for _ in range(600):
    try:
        qmp = socket.socket(socket.AF_UNIX); qmp.connect(sock_path); break
    except OSError:
        time.sleep(0.05)
else:
    fail("no QMP socket")
qf = qmp.makefile("rw")
json.loads(qf.readline())
def q(cmd, **args):
    qf.write(json.dumps({"execute": cmd, "arguments": args}) + "\n"); qf.flush()
    while True:
        r = json.loads(qf.readline())
        if "return" in r: return r["return"]
        if "error" in r: fail(f"QMP {cmd}: {r['error']}")
q("qmp_capabilities")

KEYS = {" ": "spc", "\n": "ret", "\t": "tab", "\b": "backspace", ".": "dot", "/": "slash", "-": "minus",
        "=": "equal", ",": "comma", ";": "semicolon", "\x1b": "esc", "'": "apostrophe"}
SHIFTED = {"?": "slash", "_": "minus", "+": "equal", ">": "dot", "<": "comma", ":": "semicolon", '"': "apostrophe", "@": "2"}
def type_text(s):
    for c in s:
        keys_ = ["shift", c.lower()] if c.isalpha() and c.isupper() else ["shift", SHIFTED[c]] if c in SHIFTED else [KEYS.get(c, c)]
        q("send-key", keys=[{"type": "qcode", "data": k} for k in keys_])
        time.sleep(0.03 * SLOW)
def keys(*names):
    q("send-key", keys=[{"type": "qcode", "data": n} for n in names]); time.sleep(0.05)
W, H, Z = 1920, 1080, 0.75                  # the screen; screen pixels per pixel of the desktop's picture
def click(x, y):
    q("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": int((x * Z + 0.5) * 32768 / W)}},
                                  {"type": "abs", "data": {"axis": "y", "value": int((y * Z + 0.5) * 32768 / H)}}])
    time.sleep(0.05)
    for d in (True, False):
        q("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": "left"}}]); time.sleep(0.05)
def shot(name):
    time.sleep(0.5 * SLOW)
    q("screendump", filename=os.path.join(shots, name + ".png"), format="png")
def latest(prefix, name):
    """Where "atlas: dbg PREFIX NAME at X Y" was logged last -> (x, y)."""
    drain()
    k = full.rfind(f"atlas: dbg {prefix} {name} at ".encode())
    if k < 0: fail(f"no place logged for {prefix} {name}")
    x, y = full[k:].split(b" at ", 1)[1].split()[:2]
    return int(x), int(y)
def press(prefix, name): click(*latest(prefix, name))
def dpress(prefix, name):                   # a double click
    x, y = latest(prefix, name); click(x, y); click(x, y)
def sent_msg(m):
    """What came by SMTP, its dots undoubled, as an email message."""
    return email.message_from_bytes(re.sub(rb"(?m)^\.\.", b".", m["raw"]), policy=policy.default)
PDF = b"%PDF-1.4 not really\n" * 40
def rows():
    """The list's rows as drawn after the last list (atlas: dbg mailrow ID at X Y new|seen from=... subj=...)."""
    drain(1)
    text = full.decode(errors="replace")
    got = []
    for r in re.findall(r"atlas: dbg mailrow (\S+) at \d+ \d+ (new|seen) from=(.*?) subj=(.*)", text[text.rfind("atlas: mail: list "):]):
        if r[0] in [g[0] for g in got]: break                # (the next drawing)
        got.append(tuple(x.strip() for x in r))
    return got

# ---- first start on the screen, alice logged in there and on the serial console
wait("atlas: first start: accounts needed", timeout=60)
wait("atlas: greeter (")
time.sleep(0.5)
type_text("rootpass1\trootpass1\n"); wait("atlas: setup: step 2")
type_text("alice\tAlice Example\talicepass1\talicepass1\n"); wait("atlas: setup: step 3")
type_text("\n"); wait("atlas: login alice: ok", timeout=60)
wait("login: ", "con", 30, line=False)
send("alice"); wait("Password: ", "con", line=False); send("alicepass1"); wait("alice@sieos", "con", 30, line=False)
sh("echo 1 > /tmp/atlas.debug")
say("ok: first start; alice on the screen and the serial console")

# ---- Mail from the strip: the account form, IMAP + SMTP (STARTTLS)
keys("ctrl", "spc"); wait("atlas: strip")
type_text("mail"); time.sleep(0.5); type_text("\n")
wait("atlas: mail: account form")
shot("mail-01-account")
type_text("Alice Example\t" + USER + "\t10.0.2.100\t\t10.0.2.100\t\b\b\b587\t" + USER + "\t" + PW)
time.sleep(0.3)
type_text("\n")
wait("atlas: mail: account 0 saved (imap)")
wait("atlas: mail: helper started (pid ")
got = wait("atlas: mail: folders 0: ", timeout=60)
if got.strip() != "3, 3 unread": fail(f"folders: {got} (INBOX, Sent, Trash; [Archive] cannot be selected; 3 unread)")
got = wait("atlas: mail: list 0 INBOX: ", timeout=60)
if got.strip() != "4 messages, 3 unread": fail(f"INBOX: {got}")
r = rows()
want = [("3", "new", "Renee", "Reunion a 10h ? demain"), ("2", "new", "Bob", "Quarterly report"),
        ("1", "new", "Alice Martin", "Lunch on Friday"), ("4", "seen", "News", "Newsletter")]
if [tuple(x) for x in r] != want: fail(f"the list: {r}")
shot("mail-02-inbox")
say("ok: account form -> IMAP over TLS: 3 folders (3 unread), INBOX's 4 messages newest first, 3 unread, the encoded words decoded")

press("mailrow", "2")
got = wait("atlas: mail: read 2: ", timeout=60)
if not got.strip().endswith("1 attachments"): fail(f"read 2: {got}")
if "\\Seen" not in srv.imap["INBOX"][1]["flags"]: fail("message 2 not \\Seen on the server")
shot("mail-03-read")
say(f"ok: the multipart message read ({got.strip()}), \\Seen on the server")
press("mailatt", "1")
got = wait("atlas: mail: save 2: ", timeout=60)
if got.strip() != "Saved /home/alice/Downloads/report.pdf" or "\\Seen" not in srv.imap["INBOX"][1]["flags"]: fail(f"saving the attachment: {got}")
say("ok: the attachment saved in ~/Downloads")

# ---- a reply, then a new message
press("mailbtn", "Reply"); wait("atlas: mail: compose (reply)")
type_text("Thanks Bob.\n.a line starting with a dot\n")
shot("mail-04-reply")
press("mailbtn", "Send")
wait("atlas: mail: sent", timeout=60)
m = srv.sent[-1]
raw = m["raw"]
hdr, _, body = raw.partition(b"\r\n\r\n")
checks = [m["from"] == f"<{USER}>", m["rcpt"] == ["<bob@example.org>"], b"\n" not in raw.replace(b"\r\n", b""),
          b"From: \"Alice Example\" <alice@example.org>" in hdr, b"Subject: Re: Quarterly report" in hdr,
          b"In-Reply-To: <b64-2@example.org>" in hdr, re.search(rb"Date: \w{3}, \d\d \w{3} 20\d\d \d\d:\d\d:\d\d \+0000", hdr),
          re.search(rb"Message-ID: <[0-9a-f]{16}@example.org>", hdr), body.startswith(b"Thanks Bob.\r\n..a line starting with a dot\r\n"),
          b"> Hello team," in body]
if not all(checks): fail(f"the reply as received: {checks}\n{raw.decode(errors='replace')}")
cp = srv.imap["Sent"][-1]["data"] if srv.imap["Sent"] else b""
if not cp.startswith(b"From: ") or b"\r\n.a line starting with a dot\r\n" not in cp or "\\Seen" not in srv.imap["Sent"][-1]["flags"]:
    fail(f"no copy in Sent: {cp!r}")
say("ok: reply sent over SMTP with STARTTLS: CRLF lines, the dot doubled, Date, Message-ID, In-Reply-To, the text quoted; a copy in Sent")

press("mailrow", "3")
wait("atlas: mail: read 3: ", timeout=60)
keys("ctrl", "shift", "r"); wait("atlas: mail: compose (reply all)")
type_text("To all.")
keys("ctrl", "ret")
wait("atlas: mail: sent", timeout=60)
m = srv.sent[-1]
if m["rcpt"] != ["<renee@example.org>", "<carol@example.org>", "<john@example.org>", "<bob@example.org>"] or \
        b'Cc: carol@example.org, "Doe, John" <john@example.org>, bob@example.org\r\n' not in m["raw"]:
    fail(f"reply all: {m['rcpt']}\n{m['raw'][:600]!r}")
say("ok: reply all (Ctrl+Shift+R, Ctrl+Enter): the sender in To, the others in Cc (a name with a comma kept whole), not us")
auth = [l for l in srv.log if l.startswith("smtp auth")]
press("mailbtn", "New"); wait("atlas: mail: compose (new)")
type_text("bob@example.org, Carol <carol@example.org>\t\tHello from SIEOS\tLine one\n.\nLast line")
press("mailbtn", "Send")
wait("atlas: mail: sent", timeout=60)
m = srv.sent[-1]
if m["rcpt"] != ["<bob@example.org>", "<carol@example.org>"] or not m["raw"].split(b"\r\n\r\n", 1)[1].startswith(b"Line one\r\n..\r\nLast line\r\n") \
        or b"Subject: Hello from SIEOS\r\n" not in m["raw"]:
    fail(f"the new message: {m}")
say(f"ok: a new message to 2 addresses ({auth[-1] if auth else 'auth?'})")

keys("ctrl", "n"); wait("atlas: mail: compose (new)")
type_text("carol@example.org\t\tThe report\tHere it is.")
press("mailbtn", "Attach")
wait("atlas: files /home/alice: ")
dpress("file", "Downloads"); wait("atlas: files /home/alice/Downloads: ")
dpress("file", "report.pdf"); wait("atlas: mail: attached /home/alice/Downloads/report.pdf")
shot("mail-07-attach")
press("mailbtn", "Send")
wait("atlas: mail: sent", timeout=60)
e = sent_msg(srv.sent[-1])
a = list(e.iter_attachments())
if len(a) != 1 or a[0].get_filename() != "report.pdf" or a[0].get_content_type() != "application/pdf" or a[0].get_content() != PDF \
        or e.get_body(("plain",)).get_content().strip() != "Here it is.":
    fail(f"the message with a file attached: {srv.sent[-1]['raw'][:800]!r}")
press("mailrow", "2")
wait("atlas: mail: read 2: ", timeout=60)
keys("ctrl", "l"); wait("atlas: mail: compose (forward)")
wait("atlas: mail: save 2: for forwarding", timeout=60)
type_text("dave@example.org")
keys("ctrl", "ret")
wait("atlas: mail: sent", timeout=60)
e = sent_msg(srv.sent[-1])
a = list(e.iter_attachments())
if e["Subject"] != "Fwd: Quarterly report" or len(a) != 1 or a[0].get_content() != PDF or "Hello team," not in e.get_body(("plain",)).get_content():
    fail(f"the forward: {srv.sent[-1]['raw'][:800]!r}")
say("ok: a file attached (picked in Files: multipart/mixed, base64, the same bytes); a forward (Ctrl+L) with the attachment")

keys("ctrl", "spc"); wait("atlas: strip")
type_text("mailto:carol@example.org?subject=Strip"); time.sleep(0.3); type_text("\n")
wait("atlas: mail: mailto: to=carol@example.org subject=Strip", timeout=30)
keys("ctrl", "ret")
wait("atlas: mail: sent", timeout=60)
if srv.sent[-1]["rcpt"] != ["<carol@example.org>"] or b"Subject: Strip\r\n" not in srv.sent[-1]["raw"]: fail(f"mailto: {srv.sent[-1]}")
say("ok: a mailto: link from the sia strip")

# ---- delete one
press("mailrow", "3")
wait("atlas: mail: read 3: ", timeout=60)
press("mailbtn", "Delete")
wait("atlas: mail: delete 3: Moved to Trash", timeout=60)
got = wait("atlas: mail: list 0 INBOX: ", timeout=60)
if not got.startswith("3 messages") or [m["uid"] for m in srv.imap["INBOX"]] != [1, 2, 4] or len(srv.imap["Trash"]) != 1:
    fail(f"after delete: {got}, {srv.imap['INBOX']}, {srv.imap['Trash']}")
shot("mail-05-deleted")
press("mailfolder", "0/Trash")
wait("atlas: mail: list 0 Trash: 1 messages", timeout=60)
uid = rows()[0][0]
press("mailrow", uid); wait(f"atlas: mail: read {uid}: ", timeout=60)
keys("delete")
wait(f"atlas: mail: delete {uid}: Deleted", timeout=60)
if srv.imap["Trash"]: fail(f"still in the Trash: {srv.imap['Trash']}")
press("mailstar", "0/Trash")
wait("atlas: mail: favorites 0:\tTrash")
keys("f5")
got = wait("atlas: mail: folders 0: ", timeout=60)
latest("mailfolder", "0/Trash")
if not re.search(rb"atlas: dbg mailfolder 0/Trash at \d+ \d+ unseen=0 fav", full[-20000:]): fail("Trash is not a favorite")
say("ok: delete moves to the Trash (\\Trash), from the Trash for good (\\Deleted, EXPUNGE); Trash made a favorite; F5")

# ---- the command line, the same account (atlas quiet: its log shares the serial port)
sh("rm /tmp/atlas.debug")
out = sh("mail list")
if not re.search(r"2\t \t2026-10-14 16:40\tBob\tQuarterly report", out) or not re.search(r"1\t\*\t2026-10-12 09:15\tAlice Martin", out):
    fail(f"mail list: {out!r}")
out = sh("mail read 1")
if "Shall we meet at the café at noon? This line is long enough to be wrapped by a soft line break." not in out or "= the end =" not in out:
    fail(f"mail read 1 (quoted-printable): {out!r}")
out = sh("mail read 2")
if "Hello team,\nthe report is attached: 12 % better." not in out.replace("\r\n", "\n") or "Attachment: report.pdf (application/pdf" not in out or "(HTML)" in out:
    fail(f"mail read 2 (base64, multipart): {out!r}")
out = sh("mail read 4")
if "Big news & more" not in out or "Café au lait" not in out or "red" in out:
    fail(f"mail read 4 (HTML): {out!r}")
out = sh("mail folders")
if not all(f in out for f in ("INBOX\t0", "Sent\t0", "Trash\t0")) or "Archive" in out: fail(f"folders: {out!r}")
out = sh("ls -l .mail")
if "-rw-------" not in out: fail(f"~/.mail is not only alice's: {out!r}")
out = sh("cat .mail")
if "fav=Trash" not in out: fail(f"the favorite is not kept: {out!r}")
out = sh("cat Downloads/report.pdf")
if out.count("%PDF-1.4 not really") != 40: fail(f"~/Downloads/report.pdf: {out!r}")
drain(0.3)
pos["con"] = len(full)
send('mail "mailto:dave@example.org?subject=Hi%20there&body=First%20line%0D%0A"')
wait("(the text; a line with only . sends it)", "con", 30)
send("typed line"); time.sleep(0.3); send(".")
wait("alice@sieos", "con", 60, line=False)
m = sent_msg(srv.sent[-1])
if srv.sent[-1]["rcpt"] != ["<dave@example.org>"] or m["Subject"] != "Hi there" or m.get_content().replace("\r\n", "\n") != "First line\ntyped line\n":
    fail(f"mail mailto: {srv.sent[-1]}")
say("ok: mail list / read (quoted-printable, base64 multipart, HTML stripped; UTF-8 on the console) / folders with unread counts; "
    "~/.mail is -rw------- and keeps the favorite; the attachment saved whole; mail mailto: (the text typed)")

# ---- a second account, a self-signed server: refused, accepted (pinned), written from
sh("echo 1 > /tmp/atlas.debug")
press("mailbtn", "Account"); wait("atlas: mail: account form 0")
press("mailacc", "1"); wait("atlas: mail: account form 1")
type_text("Bob Home\t" + USER2 + "\t10.0.2.101\t\t10.0.2.101\t\t\t" + PW2 + "\n")
wait("atlas: mail: account 1 saved (imap)")
got = wait("atlas: mail: folders: error: ", timeout=60)
if not got.startswith("The server's certificate is not trusted"): fail(f"a self-signed certificate, not allowed: {got}")
press("mailbtn", "Account"); wait("atlas: mail: account form 1")
press("mailopt", "4"); time.sleep(0.3)
press("mailbtn", "Save"); wait("atlas: mail: account 1 saved (imap)")
got = wait("atlas: mail: folders: error: ", timeout=60)
if got.strip() != "Untrusted certificate, SHA-256 " + fp2: fail(f"the fingerprint: {got} (openssl: {fp2})")
shot("mail-08-certificate")
press("mailbtn", "Accept"); wait("atlas: mail: certificate accepted (account 1, incoming)")
got = wait("atlas: mail: folders 1: ", timeout=60)
if got.strip() != "3, 1 unread": fail(f"the second account's folders: {got}")
got = wait("atlas: mail: list 1 INBOX: ", timeout=60)
if not got.startswith("2 messages, 1 unread"): fail(f"the second account's INBOX: {got}")
r = rows()
if [x[2:] for x in r] != [("Mum", "Sunday?"), ("Shop", "Your order has shipped")]: fail(f"the second account's rows: {r}")
shot("mail-09-accounts")
keys("ctrl", "n"); wait("atlas: mail: compose (new)")
type_text("alice@example.org\t\tFrom home\tHello from home.")
keys("ctrl", "ret")
got = wait("atlas: mail: send: error: ", timeout=60)
if got.strip() != "Untrusted certificate, SHA-256 " + fp2: fail(f"the outgoing server's fingerprint: {got}")
press("mailbtn", "Accept"); wait("atlas: mail: certificate accepted (account 1, outgoing)")
got = wait("atlas: mail: sent", timeout=60)
m = srv2.sent[-1] if srv2.sent else {}
if "Sent Messages" not in got or b'From: "Bob Home" <bob@home.test>' not in m.get("raw", b"") or len(srv2.imap["Sent Messages"]) != 1:
    fail(f"writing from the second account: {got}, {m}")
sh("rm /tmp/atlas.debug")
out = sh("cat .mail")
if "[account]" not in out or "in_pin=" + fp2.replace(":", "") not in out or "out_pin=" + fp2.replace(":", "") not in out or "fav=Trash" not in out:
    fail(f"~/.mail with two accounts: {out!r}")
out = sh("mail -a 1 list")
if "Sunday?" not in out: fail(f"mail -a 1 list: {out!r}")
srv2.ctx = ctx3
out = sh("mail -a 1 folders")
if "error: the server's certificate changed: refused; SHA-256 now " + fp3 not in out: fail(f"a changed certificate: {out!r}")
out = sh("ls -l .mail")
if "-rw-------" not in out: fail(f"~/.mail is not only alice's: {out!r}")
say("ok: a second account (another server, self-signed): refused until allowed, its SHA-256 shown and pinned (in and out), "
    "its folders found by name, written from (a copy in \"Sent Messages\"); a changed certificate refused")

# ---- POP3 (995) in the window
sh("echo 1 > /tmp/atlas.debug")
press("mailbtn", "Account"); wait("atlas: mail: account form 1")
press("mailacc", "0"); wait("atlas: mail: account form 0")
press("mailopt", "0"); time.sleep(0.3)
press("mailbtn", "Save")
wait("atlas: mail: account 0 saved (pop3)")
got = wait("atlas: mail: list 0 INBOX: ", timeout=90)
if got.strip() != "4 messages, 4 unread" or len(srv.pop) != 4: fail(f"POP3 list: {got}, {len(srv.pop)} on the server")
r = rows()
if [x[0] for x in r] != ["msg-3.x", "msg-2.x", "msg-1.x", "msg-4.x"]: fail(f"POP3 rows: {r}")
shot("mail-06-pop3")
press("mailrow", "msg-2.x")
got = wait("atlas: mail: read msg-2.x: ", timeout=60)
if not got.strip().endswith("1 attachments"): fail(f"POP3 read: {got}")
press("mailbtn", "Delete")
wait("atlas: mail: delete msg-2.x: Deleted", timeout=60)
got = wait("atlas: mail: list 0 INBOX: ", timeout=90)
if not got.startswith("3 messages, 3 unread") or [m["uidl"] for m in srv.pop] != ["msg-1.x", "msg-3.x", "msg-4.x"]:
    fail(f"POP3 after delete: {got}, {[m['uidl'] for m in srv.pop]}")
retr = [l for l in srv.log if l.startswith("pop< RETR")]
if len(retr) != 4: fail(f"messages fetched twice? {retr}")
sh("rm /tmp/atlas.debug")
out = sh("ls Mail/INBOX")
if "msg-1.x" not in out or "msg-2.x" in out: fail(f"~/Mail/INBOX: {out!r}")
say("ok: POP3 over TLS: 4 messages copied (left on the server, none fetched twice), one read, one deleted here and there")

# ---- the command line: POP3 with STLS (110), IMAP with STARTTLS (143), SMTP on 465
def account(**kv):
    a = dict(name="Alice", address=USER, in_host="10.0.2.100", out_host="10.0.2.100", user=USER, **{"pass": PW})
    a.update(kv)
    first = True
    for k, v in a.items():
        sh(f"echo {k}={v} {'>' if first else '>>'} .mail"); first = False
account(**{"in": "pop3", "in_port": "110", "out_port": "465", "keep": "0"})
out = sh("mail list")
if "msg-1.x" not in out or "error" in out or srv.pop: fail(f"POP3 STLS, keep=0: {out!r}, {len(srv.pop)} left")
if not any(l == "pop< STLS" for l in srv.log): fail("no STLS")
sh("echo To: bob@example.org > /tmp/d.txt"); sh("echo Subject: from the shell >> /tmp/d.txt"); sh("echo >> /tmp/d.txt"); sh("echo hi >> /tmp/d.txt")
n = len(srv.sent)
out = sh("mail send /tmp/d.txt")
if "sent to 1 address" not in out or len(srv.sent) != n + 1 or b"Subject: from the shell" not in srv.sent[-1]["raw"]: fail(f"mail send on 465: {out!r}")
account(**{"in": "imap", "in_port": "143"})
out = sh("mail folders")
if "Trash" not in out or not any(l.endswith("STARTTLS") for l in srv.log if l.startswith("imap<")): fail(f"IMAP STARTTLS: {out!r}")
account(**{"in": "imap", "pass": "wrong"})
out = sh("mail list")
if "error:" not in out: fail(f"a wrong password: {out!r}")
say("ok: POP3 with STLS (not left on the server), SMTP on 465, IMAP with STARTTLS; a wrong password is an error")

# ---- a server that knows TLS 1.2 only (as many mail providers still do)
ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
account(**{"in": "imap", "out_port": "465"})
out = sh("mail folders")
if "Trash" not in out: fail(f"IMAP over TLS 1.2: {out!r}")
n = len(srv.sent)
out = sh("mail send /tmp/d.txt")
if "sent to 1 address" not in out or len(srv.sent) != n + 1: fail(f"SMTP over TLS 1.2: {out!r}")
say("ok: IMAP (993) and SMTP (465) with a server limited to TLS 1.2")

qemu.kill()
print(f"PASS: mail test (screenshots in {shots})")
