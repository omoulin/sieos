# git on SIEOS

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

`/bin/git` is git written for SIEOS (`user/git`, about 2,500 lines): not a
port of git, which would need a large POSIX layer, but the same formats,
so a repository made on SIEOS is an ordinary git repository for the host's
git (`git fsck --strict` passes), and one cloned from GitHub or made on the
host works here. The desktop has a Git window on top of it
([desktop.md](desktop.md#git)).

## Commands

```
git init [DIR]                    git clone URL [DIR]
git status [-s] [-b]              git add PATH... | -A        git rm [--cached] PATH...
git reset [PATH...]               git restore [--staged] PATH...
git commit [-a] -m MESSAGE        git log [--oneline] [-n N] [REV]      git show [REV]
git diff [--cached] [PATH...]     git branch [-a | -d NAME | NAME [REV]]
git checkout [-b NEW] BRANCH      git checkout -- PATH...     git switch [-c NEW] BRANCH
git fetch [REMOTE]                git pull [REMOTE [BRANCH]]  git push [-u] [REMOTE [BRANCH]]
git remote [-v]                   git remote add NAME URL     git config [--global] KEY [VALUE]
git -C DIR COMMAND ...
```

- **The message.** The shell splits words and drops quotes, so everything
  after `-m` is the message: `git commit -m Fix the parser` (the same for
  `git config user.name Alice Example`).
- **Who.** `user.name` and `user.email` from `.git/config` or
  `~/.gitconfig` (`git config --global ...`); without them, the account's
  full name and `name@hostname`. Times are UTC (+0000), taken from the
  file server's clock.
- **Revisions.** `HEAD`, a branch, a tag (annotated ones peeled), a
  remote's branch (`origin/main`), a hash or its first 4+ digits, then
  `~N` and `^` / `^2`.
- **Ignored files.** The top folder's `.gitignore` and
  `.git/info/exclude`: globs with `*`, `?`, `**`; a trailing `/` for
  folders; a pattern with a `/` is matched from the top (no `!`).
- **Checkout** (and switch) needs no uncommitted change to tracked files,
  and refuses to overwrite an untracked file. `git checkout NAME` with
  only `origin/NAME` makes a branch that follows it.
- **Pull** fetches, then: nothing to do; a fast-forward; or, when both
  sides have new commits, a merge commit if no file changed on both sides
  (whole files are taken from the side that changed them). If a file
  changed on both, it stops before changing anything and lists them
  ("CONFLICT: x changed on both sides"): merge by hand (or on the host).
- **Push** only fast-forwards (`pull` first otherwise); `-u` remembers
  the branch's upstream.
- `reset` only resets the index (`--hard` and `--soft` are refused);
  `diff` compares the index with the files, `--cached` HEAD with the index.

## Transports and credentials

- `git://host[:port]/path`: TCP, port 9418 (`git daemon`).
- `http://` and `https://` (smart HTTP, `git http-backend`, GitHub,
  GitLab...) over SIEOS's HTTP client and TLS 1.3 ([net.md](net.md)), with
  the server's certificate checked.
- Both speak protocol v0: the server lists its refs, the client sends
  `want` lines (`ofs-delta thin-pack`), up to 256 `have` lines (our
  newest commits) and `done`; the pack comes raw (no side-band).
  Push: `old new ref` with `report-status`, then a pack.
- HTTP credentials (Basic), asked only when the server answers 401: from
  the URL (`https://user:token@host/...`), else `credential.username` and
  `credential.token` in the configuration, else asked on the terminal
  (the password without echo). The Git window cannot answer questions:
  configure a token for it.

## The repository's files

Exactly git's (version 2 of everything):

| File | What |
|---|---|
| `.git/objects/xx/yyyy...` | loose objects: `type size\0content`, zlib-compressed, named by SHA-1 |
| `.git/objects/pack/pack-*.pack`, `.idx` | packs: entries with OFS and REF deltas; index v2 (fan-out, names, CRC-32, offsets) |
| `.git/refs/...`, `.git/packed-refs`, `.git/HEAD` | branches, tags, remotes' branches; HEAD names the branch |
| `.git/index` | the next commit: "DIRC" v2 (v3 read too), with each file's size and time |
| `.git/config`, `~/.gitconfig` | `[section "sub"]` and `key = value` lines |

A fetched pack is kept as it came (and indexed: every object's name
computed, deltas resolved against the pack or our objects). A **thin**
pack, whose deltas use objects only we have, is completed first: those
objects are appended and the pack's checksum recomputed, as `git
index-pack --fix-thin` does. Packs pushed carry whole objects (no deltas).
Loose objects and pushed packs use SIEOS's deflate (`lib/zlib.c`): fixed
Huffman codes and LZ77 matches with a one-entry hash (the whole file,
inflate and CRC-32 included, is 260 lines); SIEOS's own C sources shrink to
46% of their size with it (zlib -9: 32%).
Inflate reads all of zlib's output. Both, and SHA-1 (`lib/sha1.c`), are
checked by `make crypto-test`: FIPS 180's SHA-1 examples, CRC-32's check
value, streams made by the reference zlib (stored, fixed, dynamic
blocks), and the host's zlib both ways on several inputs.

## Sizes

| | text |
|---|---|
| `/bin/git` | 131 KB, of which git itself ~63 KB (git.c 32, odb.c 17, proto.c 11, diff.c 4) and SHA-1 + zlib 6.5 KB; the rest is the network, HTTP and TLS library |
| the Git window in atlas (`repo.c`) | 8.7 KB |

## Not done

Merges of changes inside one file (conflict markers), rebase, stash,
tags made here (read only), submodules (left out of checkouts), shallow
clones, SSH, protocol v2, the dumb HTTP protocol, `diff` between two
commits, SHA-256 repositories, delta compression of what we push. Packs
are read whole into memory (and a fetch's pack too, while it is indexed):
fine for projects of a few tens of MB, not for very large ones. Untested
yet against the big hosting services (GitHub, GitLab) over HTTPS; the
HTTP client says `User-Agent: SIEOS/1`, not `git/...`.

## Test

`make git-test` (tools/gittest.py), on both architectures: the host makes
a bare repository with history (repacked, so the clone gets deltas) and
serves it with `git daemon` (pushes allowed) and with `git http-backend`
behind a small Python HTTP server that wants a password for pushes. In
SIEOS: clone, log, status, the executable bit, `show` (a unified diff),
diff, add, `diff --cached`, commit, push; the host runs `git fsck
--strict` and finds the commit; a host commit pulled (fast-forward); a
thin pack (from host git's `pack-objects --thin` behind a minimal
upload-pack in the test) completed; changes on both sides merged, the
merge pushed; the same file on both sides refused, nothing changed; push
refused when behind; reset and `checkout --`; branches and switch;
`git init` and two commits; clone over HTTP, a push that asks for the
password, one with a configured token. Then the repositories SIEOS made
are copied out of the disk image and checked by the host: `git fsck
--strict --full`, `git status` (it reads our index: clean), `git log`,
`git verify-pack` of the packs SIEOS indexed.
