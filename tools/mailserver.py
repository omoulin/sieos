#!/usr/bin/env python3
"""A small mail server for the tests (tools/mailtest.py): IMAP4rev1, POP3
and SMTP submission, the subset SIEOS's mail program uses, over TLS (at
once, or after STARTTLS / STLS), with one test account.

  IMAP  CAPABILITY LOGIN LIST SELECT FETCH (UID FLAGS BODY.PEEK[HEADER.FIELDS])
        UID FETCH BODY[] (sets \\Seen) UID STORE +FLAGS EXPUNGE NOOP LOGOUT
  POP3  USER PASS STAT LIST UIDL RETR TOP DELE NOOP RSET QUIT
  SMTP  EHLO AUTH PLAIN/LOGIN MAIL RCPT DATA RSET NOOP QUIT (TLS and a login
        required before MAIL)

start(ctx, user, password, messages) -> a server: .port(kind, tls) makes a
listening port; .imap / .pop are the mailboxes; .sent what came by SMTP.
`python3 mailserver.py relay PORT` copies its stdin and stdout to
127.0.0.1:PORT (for QEMU's guestfwd=...-cmd:, one per connection).

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import base64, os, re, select, socket, sys, threading


class Conn:
    """Lines in and out of a socket that may become TLS on the way."""
    def __init__(self, sock, ctx, tls):
        self.s, self.ctx, self.buf = sock, ctx, b""
        if tls:
            self.starttls()

    def starttls(self):
        self.s = self.ctx.wrap_socket(self.s, server_side=True)
        self.tls = True

    tls = False

    def line(self):
        while b"\r\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise EOFError
            self.buf += d
        l, self.buf = self.buf.split(b"\r\n", 1)
        return l

    def send(self, data):
        self.s.sendall(data if isinstance(data, bytes) else data.encode())


class Server:
    def __init__(self, ctx, user, password, messages):
        self.ctx, self.user, self.password = ctx, user, password
        self.imap = {"INBOX": [], "Sent": [], "Trash": []}
        self.uidnext = 1
        for data, seen in messages:
            self.imap["INBOX"].append({"uid": self.uidnext, "flags": {"\\Seen"} if seen else set(), "data": data})
            self.uidnext += 1
        self.pop = [{"uidl": f"msg-{i + 1}.x", "data": d} for i, (d, _) in enumerate(messages)]
        self.sent, self.log, self.lock = [], [], threading.Lock()

    def port(self, kind, tls):
        ls = socket.socket()
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind(("127.0.0.1", 0))
        ls.listen(8)
        def loop():
            while True:
                c, _ = ls.accept()
                threading.Thread(target=self.serve, args=(kind, c, tls), daemon=True).start()
        threading.Thread(target=loop, daemon=True).start()
        return ls.getsockname()[1]

    def serve(self, kind, sock, tls):
        try:
            c = Conn(sock, self.ctx, tls)
            getattr(self, kind)(c)
        except (EOFError, OSError) as e:
            self.log.append(f"{kind}: {e!r}")
        finally:
            sock.close()

    # ---- IMAP
    def imapd(self, c):
        c.send("* OK IMAP4rev1 SIEOS test server ready\r\n")
        authed, box = False, None
        while True:
            l = c.line().decode(errors="replace")
            self.log.append("imap< " + (l if "LOGIN" not in l.upper() else l.split()[0] + " LOGIN ..."))
            tag, _, rest = l.partition(" ")
            cmd, _, args = rest.partition(" ")
            cmd = cmd.upper()
            uid = cmd == "UID"
            if uid:
                cmd, _, args = args.partition(" ")
                cmd = cmd.upper()
            ok = lambda t="done": c.send(f"{tag} OK {t}\r\n")
            if cmd == "CAPABILITY":
                c.send("* CAPABILITY IMAP4rev1" + ("" if c.tls else " STARTTLS") + "\r\n"); ok()
            elif cmd == "STARTTLS" and not c.tls:
                ok("begin TLS"); c.starttls()
            elif cmd == "LOGIN":
                if not c.tls:
                    c.send(f"{tag} NO TLS first\r\n"); continue
                a = re.findall(r'"((?:[^"\\]|\\.)*)"', args)
                a = [x.replace('\\"', '"').replace("\\\\", "\\") for x in a]
                if a == [self.user, self.password]:
                    authed = True; ok("logged in")
                else:
                    c.send(f"{tag} NO [AUTHENTICATIONFAILED] wrong password\r\n")
            elif cmd == "LOGOUT":
                c.send("* BYE\r\n"); ok(); return
            elif cmd == "NOOP":
                ok()
            elif not authed:
                c.send(f"{tag} NO log in first\r\n")
            elif cmd == "LIST":
                c.send('* LIST (\\Noselect \\HasChildren) "/" "[Archive]"\r\n')
                for name in self.imap:
                    c.send(f'* LIST (\\HasNoChildren) "/" "{name}"\r\n')
                ok()
            elif cmd == "SELECT":
                name = args.strip().strip('"')
                if name not in self.imap:
                    c.send(f"{tag} NO no such folder\r\n"); continue
                box = self.imap[name]
                c.send(f"* {len(box)} EXISTS\r\n* 0 RECENT\r\n* OK [UIDVALIDITY 7] ok\r\n* OK [UIDNEXT {self.uidnext}] ok\r\n")
                ok("[READ-WRITE] selected")
            elif box is None:
                c.send(f"{tag} NO select a folder\r\n")
            elif cmd == "FETCH":
                seqs, _, items = args.partition(" ")
                for i, m in self.pick(box, seqs, uid):
                    c.send(self.fetch(i, m, items))
                ok()
            elif cmd == "STORE":
                seqs, _, what = args.partition(" ")
                flags = set(re.findall(r"\\\w+", what))
                for i, m in self.pick(box, seqs, uid):
                    if what.upper().startswith("+FLAGS"): m["flags"] |= flags
                    elif what.upper().startswith("-FLAGS"): m["flags"] -= flags
                    else: m["flags"] = flags
                    if "SILENT" not in what.upper():
                        c.send(f"* {i} FETCH (FLAGS ({' '.join(sorted(m['flags']))}))\r\n")
                ok()
            elif cmd == "EXPUNGE":
                with self.lock:
                    i = 0
                    while i < len(box):
                        if "\\Deleted" in box[i]["flags"]:
                            del box[i]; c.send(f"* {i + 1} EXPUNGE\r\n")
                        else:
                            i += 1
                ok()
            else:
                c.send(f"{tag} BAD unknown command\r\n")

    @staticmethod
    def pick(box, seqs, uid):
        out = []
        for part in seqs.split(","):
            a, _, b = part.partition(":")
            top = max([m["uid"] for m in box] or [0]) if uid else len(box)
            lo = top if a == "*" else int(a)
            hi = lo if not b else top if b == "*" else int(b)
            lo, hi = min(lo, hi), max(lo, hi)
            for i, m in enumerate(box, 1):
                k = m["uid"] if uid else i
                if lo <= k <= hi and (i, m) not in out:
                    out.append((i, m))
        return out

    @staticmethod
    def fetch(i, m, items):
        u = items.upper()
        parts = []
        if "UID" in u:
            parts.append(f"UID {m['uid']}".encode())
        lit = b""
        h = re.search(r"BODY(\.PEEK)?\[HEADER\.FIELDS \(([^)]*)\)\]", items, re.I)
        if h:
            want = [w.lower() for w in h.group(2).split()]
            head = m["data"].split(b"\r\n\r\n", 1)[0].split(b"\r\n")
            keep, on = [], False
            for line in head:
                if line[:1] in (b" ", b"\t"):
                    if on: keep.append(line)
                    continue
                on = line.split(b":", 1)[0].decode().lower() in want
                if on: keep.append(line)
            data = b"\r\n".join(keep) + b"\r\n\r\n"
            parts.append(f"BODY[HEADER.FIELDS ({h.group(2).upper()})] {{{len(data)}}}\r\n".encode() + data)
        elif re.search(r"BODY\[\]", items, re.I):
            m["flags"].add("\\Seen")
            parts.append(f"BODY[] {{{len(m['data'])}}}\r\n".encode() + m["data"])
        if "FLAGS" in u:
            parts.append(f"FLAGS ({' '.join(sorted(m['flags']))})".encode())
        return f"* {i} FETCH (".encode() + b" ".join(parts) + b")\r\n"

    # ---- POP3
    def pop3d(self, c):
        c.send("+OK POP3 SIEOS test server ready\r\n")
        user, authed, dele = None, False, set()
        while True:
            l = c.line().decode(errors="replace")
            self.log.append("pop< " + (l if not l.upper().startswith("PASS") else "PASS ..."))
            cmd, _, arg = l.partition(" ")
            cmd = cmd.upper()
            box = self.pop
            live = [(i + 1, m) for i, m in enumerate(box) if i + 1 not in dele]
            if cmd == "CAPA":
                c.send("+OK\r\nUSER\r\nUIDL\r\nTOP\r\n" + ("" if c.tls else "STLS\r\n") + ".\r\n")
            elif cmd == "STLS" and not c.tls:
                c.send("+OK begin TLS\r\n"); c.starttls()
            elif cmd == "USER":
                user = arg; c.send("+OK\r\n")
            elif cmd == "PASS":
                if c.tls and user == self.user and arg == self.password:
                    authed = True; c.send("+OK logged in\r\n")
                else:
                    c.send("-ERR wrong password\r\n")
            elif cmd == "QUIT":
                with self.lock:
                    for k in sorted(dele, reverse=True):
                        del box[k - 1]
                c.send("+OK bye\r\n"); return
            elif not authed:
                c.send("-ERR log in first\r\n")
            elif cmd == "STAT":
                c.send(f"+OK {len(live)} {sum(len(m['data']) for _, m in live)}\r\n")
            elif cmd in ("LIST", "UIDL"):
                f = (lambda m: len(m["data"])) if cmd == "LIST" else (lambda m: m["uidl"])
                if arg:
                    k = int(arg)
                    if k in dele or not 1 <= k <= len(box): c.send("-ERR no such message\r\n")
                    else: c.send(f"+OK {k} {f(box[k - 1])}\r\n")
                else:
                    c.send("+OK\r\n" + "".join(f"{k} {f(m)}\r\n" for k, m in live) + ".\r\n")
            elif cmd in ("RETR", "TOP"):
                a = arg.split()
                k = int(a[0])
                if k in dele or not 1 <= k <= len(box):
                    c.send("-ERR no such message\r\n"); continue
                data = box[k - 1]["data"]
                if cmd == "TOP":
                    head, _, body = data.partition(b"\r\n\r\n")
                    data = head + b"\r\n\r\n" + b"\r\n".join(body.split(b"\r\n")[:int(a[1])])
                lines = [(b"." + x if x.startswith(b".") else x) for x in data.split(b"\r\n")]
                c.send(b"+OK\r\n" + b"\r\n".join(lines) + b"\r\n.\r\n")
            elif cmd == "DELE":
                k = int(arg)
                if k in dele or not 1 <= k <= len(box): c.send("-ERR no such message\r\n")
                else: dele.add(k); c.send("+OK deleted\r\n")
            elif cmd == "RSET":
                dele.clear(); c.send("+OK\r\n")
            elif cmd == "NOOP":
                c.send("+OK\r\n")
            else:
                c.send("-ERR unknown command\r\n")

    # ---- SMTP
    def smtpd(self, c):
        c.send("220 sieos-test ESMTP ready\r\n")
        authed, frm, rcpt = False, None, []
        while True:
            l = c.line().decode(errors="replace")
            self.log.append("smtp< " + (l if not l.upper().startswith("AUTH") else "AUTH ..."))
            cmd = l[:4].upper()
            if cmd in ("EHLO", "HELO"):
                c.send("250-sieos-test\r\n" + ("" if c.tls else "250-STARTTLS\r\n") + "250-AUTH PLAIN LOGIN\r\n250 8BITMIME\r\n")
            elif l.upper() == "STARTTLS" and not c.tls:
                c.send("220 go ahead\r\n"); c.starttls()
            elif l.upper().startswith("AUTH"):
                if not c.tls:
                    c.send("530 TLS first\r\n"); continue
                a = l.split()
                if a[1].upper() == "PLAIN":
                    raw = base64.b64decode(a[2] if len(a) > 2 else (c.send("334 \r\n"), c.line())[1])
                    _, u, p = raw.split(b"\0")
                    u, p = u.decode(), p.decode()
                else:
                    c.send("334 VXNlcm5hbWU6\r\n"); u = base64.b64decode(c.line()).decode()
                    c.send("334 UGFzc3dvcmQ6\r\n"); p = base64.b64decode(c.line()).decode()
                authed = u == self.user and p == self.password
                c.send("235 ok\r\n" if authed else "535 wrong password\r\n")
                self.log.append(f"smtp auth {a[1].upper()}: {'ok' if authed else 'refused'}")
            elif l.upper().startswith("MAIL FROM:"):
                if not (c.tls and authed):
                    c.send("530 log in first\r\n"); continue
                frm, rcpt = l[10:].strip(), []; c.send("250 ok\r\n")
            elif l.upper().startswith("RCPT TO:"):
                rcpt.append(l[8:].strip()); c.send("250 ok\r\n")
            elif l.upper() == "DATA":
                if not frm or not rcpt:
                    c.send("503 sender and recipients first\r\n"); continue
                c.send("354 end with a dot\r\n")
                raw = b""                            # (byte for byte: the CRLFs and dots are checked)
                while b"\r\n.\r\n" not in raw:
                    d = c.buf or c.s.recv(65536)
                    c.buf = b""
                    if not d: raise EOFError
                    raw += d
                data, _, c.buf = raw.partition(b"\r\n.\r\n")
                self.sent.append({"from": frm, "rcpt": rcpt, "raw": data + b"\r\n"})
                frm, rcpt = None, []
                c.send("250 queued\r\n")
            elif cmd == "RSET":
                frm, rcpt = None, []; c.send("250 ok\r\n")
            elif cmd == "NOOP":
                c.send("250 ok\r\n")
            elif cmd == "QUIT":
                c.send("221 bye\r\n"); return
            else:
                c.send("500 unknown command\r\n")


def start(ctx, user, password, messages):
    return Server(ctx, user, password, messages)


if __name__ == "__main__" and sys.argv[1:2] == ["relay"]:
    sys.stderr = open(sys.argv[3] if len(sys.argv) > 3 else "/dev/null", "a")   # (its stderr is the connection too)
    s = socket.create_connection(("127.0.0.1", int(sys.argv[2])))
    while True:
        r = select.select([0, s], [], [])[0]
        if 0 in r:
            d = os.read(0, 65536)
            if not d: break
            s.sendall(d)
        if s in r:
            d = s.recv(65536)
            if not d: break
            while d: d = d[os.write(1, d):]
