# SSH

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

`/bin/ssh` (the client), `/bin/ssh-keygen` (a link to it) and `sshd` (the
server): a compact SSH-2 written for SIEOS on its own crypto and network
library, interoperable with OpenSSH both ways. Porting OpenSSH or Dropbear
would have brought a POSIX layer and a second crypto library with it.

```
  ssh (user/ssh)                         sshd (user/sshd)           auth
    │                                      │                          │
    └──── user/lib/ssh.c: the transport ───┘                          │
          version lines, KEXINIT, curve25519-sha256,                  │
          ssh-ed25519 host key, then chacha20-poly1305@openssh.com    │
                                               password? ──AUTH_CHECK─┤
                                               shell/exec ─AUTH_LOGIN─┘ starts sh,
                                                                        our child
  sh ──CON_WRITE / CON_READ ("sshd3#17")──► sshd ──channel data──► client
```

## Using it

```
ssh [-p PORT] [-i KEY] [USER@]HOST [COMMAND]      a shell (no COMMAND) or one command
ssh-keygen [-t ed25519] [-f FILE] [-C COMMENT]    a new key: ~/.ssh/id_ed25519 and .pub
```

- **First connection** to a host: its key's fingerprint (`SHA256:...`, as
  OpenSSH shows it) and "Are you sure you want to continue connecting
  (yes/no)?"; yes adds it to `~/.ssh/known_hosts` (`host` for port 22,
  `[host]:port` otherwise, OpenSSH's format). A host whose key changed is
  refused, with its new fingerprint and the line to remove.
- **Authentication**: `~/.ssh/id_ed25519` (or `-i`) if the server takes it,
  else a password, read without echo (three tries). OpenSSH's key file
  format, unencrypted keys only (one with a passphrase is reported and
  skipped).
- **A shell** gets a terminal on the server (`pty-req`, 80 × 24: SIEOS's
  terminals do not report a size) and the console goes **raw**: every key
  goes to the server as typed (Enter as CR, Backspace as DEL, arrows as
  `ESC [ A`..`D`), and the server echoes. **`~.`** after Enter disconnects.
  A command gets no terminal and no input. The exit status is the remote
  command's (255: ssh's own errors).

### The server

`sshd` is in init's table, started at boot **if `/etc/ssh/sshd_config`
exists**; otherwise it ends at once and is not started again. It is off by
default: when on, it listens on port 22 and so starts the network at boot.
To turn it on (as root):

```
mkdir /etc/ssh
echo on > /etc/ssh/sshd_config
svc restart sshd
```

(Nothing in the file is read yet; it is the switch.) On its first start it
makes its host key, `/etc/ssh/ssh_host_ed25519_key` (mode 0600, root only)
and `.pub`, and logs its fingerprint on the console:
`sshd: listening on port 22, host key SHA256:...`.

- **Passwords** are checked by the accounts server (`AUTH_CHECK`), the same
  check login does (1 s of delay after a wrong one). The shell is then
  started by `AUTH_LOGIN` with the password again, which also **opens an
  encrypted home** ([accounts.md](accounts.md)): the password reaches auth
  twice, and sshd wipes it once the shell runs.
- **Keys**: `~/.ssh/authorized_keys`, OpenSSH's format (`ssh-ed25519 KEY
  comment` lines; options before the key are skipped, not applied). An
  encrypted home that is still locked cannot be read, so its keys count
  only while it is open (logged in elsewhere); a password works always.
- **root**: with a key only (OpenSSH's default, `prohibit-password`).
- 6 tries per connection, 2 minutes to log in, up to 8 connections.
- `exec` runs `sh -c COMMAND` at the user's home; `shell` runs `sh -l`.

## How it works

### The transport (user/lib/ssh.c)

One set of algorithms, what OpenSSH negotiates by default without its
post-quantum hybrids:

| | |
|---|---|
| Key exchange | `curve25519-sha256` (and its old name `curve25519-sha256@libssh.org`): X25519, SHA-256 (RFC 8731) |
| Host key | `ssh-ed25519` (RFC 8709): Ed25519 signatures, `lib/ed25519.c` |
| Cipher | `chacha20-poly1305@openssh.com` (its MAC is the tag; no other MAC) |
| Compression | none |
| Strict key exchange | `kex-strict-c/s-v00@openssh.com` (the Terrapin fix) |

- **chacha20-poly1305@openssh.com** is not RFC 8439's AEAD: 64 bytes of
  key make two ChaCha20 keys, one encrypting the 4-byte packet length and
  one the rest; the nonce is the packet's sequence number; the one-time
  Poly1305 key is block 0 of the second key, and the tag covers the whole
  encrypted packet. It uses the ChaCha20 keystream and Poly1305 from
  `lib/aead.c` (`chacha20_xor`, `poly1305`), not copies.
- **Strict key exchange**: when both sides offer it, sequence numbers start
  from 0 with the new keys; and SIEOS never accepts a message out of place
  during the exchange anyway, strict or not (no IGNORE, no DEBUG).
- **The exchange hash** H covers both version lines, both KEXINITs, the host
  key, both ephemeral keys and the shared secret; the server signs it, the
  client checks the signature, and the keys come from the secret and H
  (RFC 4253, 7.2). The first H is the session id, which a key login signs.
- **Packets**: one send lock per connection (several threads send), one
  receiving thread; up to 35000 bytes, 32 KiB of channel data each.
- **Key files**: OpenSSH's `openssh-key-v1` (base64, 70-column lines, the
  two check words, padding 1, 2, 3...), `.pub` lines, `known_hosts` and
  `authorized_keys` lines.

### Ed25519 (lib/ed25519.c)

RFC 8032, on the field of X25519, whose arithmetic it shares
(`lib/fe25519.h`: x25519.c's five 51-bit limbs, now exported, and one
exponentiation for both 1/z and the square root's (p-5)/8 power). Points in
extended coordinates with the one addition formula that also doubles;
scalar multiplication swaps in constant time whatever the scalar's bits;
reduction modulo the group order on 64 signed byte limbs. Signatures are
deterministic (no random numbers). Checked against RFC 8032's test vectors
(1, 2, 3 and SHA(abc)): public keys, signatures, damaged signatures and
S >= L refused (`make crypto-test`).

### The terminal: how a remote shell talks

SIEOS has no pty: a program's terminal is a port named in its identity
(`mk_ident_t.console`, "port#channel"), inherited by the programs it starts;
the console server and the desktop's terminals answer `CON_WRITE`,
`CON_READ` and `CON_ECHO` there. sshd does the same for its sessions:

- Each connection slot (8) has a port, `sshd0` .. `sshd7`, served by a
  thread of its own (so a client that stops reading holds up only its own
  session); the channel number is the session's number, so a program left
  from an earlier session in the slot gets `-EIO`, as does any user but
  the session's (and root).
- What the shell writes goes to the client as channel data (`\n` as `\r\n`
  when it asked for a terminal), within the client's window (the write
  waits for room).
- What the client sends goes through the console's own line editing
  (`user/lib/tty.c`: Backspace, Ctrl-C, Ctrl-D), echoed by sshd when there
  is a terminal; input typed ahead waits in a 4 KiB queue until the shell
  reads (the editing holds one finished line). After the client's EOF, a
  read gets what is left, then 0, then `-EIO` (sh ends).
- auth starts the shell as sshd's child, on that terminal; a thread learns
  of its end from the kernel (`SYS_CHILD_PORT`) and sends `exit-status`,
  EOF and CLOSE. A connection that ends first ends its shell.

On the client's side, **`CON_RAW`** (a third value for `CON_ECHO`, in
`tty.c`, so the console and the desktop's terminals both have it): keys
go to the reader one by one, unedited and unshown. Leaving raw mode answers
a read still waiting with nothing: that is how ssh's key-reading thread
ends when the connection does.

### Threads

- ssh: one thread receives (and shows), one reads keys (and sends).
- sshd: the main thread accepts; per slot, a connection thread (reads the
  client) and a terminal thread; one thread reaps shells. Slots and their
  threads are made at first use and kept (a thread's stack is never given
  back in SIEOS).

## Tests and measurements

`make ssh-test` (tools/sshtest.py; both architectures) boots a fresh disk
with sshd on, QEMU forwarding a host port to its port 22, and checks,
against the host's OpenSSH (10.0p2):

- host `ssh` → sshd: a password (from `SSH_ASKPASS`), the host key saved
  and then checked (its fingerprint the one sshd logged), a wrong password
  refused, root refused with a password, a command's exit status, a key
  in `authorized_keys` (made by the host's ssh-keygen, written there from
  the console) and one not there, an interactive session on a terminal
  (`-tt`: typed lines, echo, `exit`);
- SIEOS's `ssh-keygen`, whose public key the host's ssh-keygen reads, then
  SIEOS's `ssh` → an sshd run on the host as this user (its own host key
  and config, at 10.0.2.2): first sight asked, commands with SIEOS's key,
  an interactive shell on the raw console, and the same host with another
  key refused;
- SIEOS's `ssh` → SIEOS's own sshd (through the host's forwarded port:
  netd has no loopback) with a password, interactively; a refused port.

`make crypto-test`: Ed25519 (RFC 8032 7.1) and Poly1305 (RFC 8439 2.5.2).

| | |
|---|---|
| Code (x86-64) | ssh 54 KB, sshd 55 KB of text (each with the file and crypto libraries it uses); user/lib/ssh.o 13.7 KB, ed25519.o 3.4 KB |
| Code (arm64) | ssh 63 KB, sshd 64 KB |
| Memory | sshd 268 KiB with one slot used (a slot: 70 KiB of connection state, two thread stacks of 16 and 24 KiB) |
| A connection (host's ssh, key, `echo`: key exchange to exit status) | 91 ms (x86-64, KVM), 100 ms (arm64, emulated) |

## Limits

- One channel per connection (a shell or a command): no port forwarding,
  agent, X11, `scp` or sftp (an sftp subsystem would be a file server
  protocol of its own: not small). No re-keying: OpenSSH asks for new keys
  after 1 GiB each way, and the connection then ends.
- Ed25519 keys only (no RSA or ECDSA), unencrypted key files only; no
  `authorized_keys` options, no `sshd_config` settings yet.
- The remote terminal is 80 × 24 whatever the window; SIEOS has no signals,
  so Ctrl-C on SIEOS's sshd drops the line being typed but does not stop a
  running program; lines are at most 255 characters; input sent faster than
  the shell reads, beyond 4 KiB, is lost.
- No `SSH_ASKPASS`-like non-interactive password: the client asks on the
  console.
