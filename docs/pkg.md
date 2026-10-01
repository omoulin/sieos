# Packages: pkg

SIEOS's base system (everything `make` builds into the images) stays as it is.
Software added to it is managed as **packages**: installed under `/usr/pkg`
(never over a base file), updated and removed with `pkg`, from signed
repositories.

## SiPM

**SiPM** (SIEOS Package Manager, *SiPM (packages)* in the SIEOS menu) is pkg in a
window: the packages the repositories offer and the installed ones, filtered
(All, Installed, Updates) and searched; the selected one's details (versions,
size, what it needs) with Install, Upgrade or Remove; Refresh (`pkg update`)
and Upgrade all. When the session is not root's, a change asks for root's
password first. sia opens it too ("open SiPM").

## Using pkg (on SIEOS)

```sh
pkg update                  # fetch the repositories' signed indexes
pkg search [WORD]           # what they offer (name or summary)
pkg info NAME               # version, dependencies, size, installed or not
pkg install NAME...         # install, with the dependencies first
pkg upgrade [NAME...]       # newer versions of the installed packages
pkg remove [-f] NAME...     # remove (refused while another package needs it; -f forces)
pkg list                    # the installed packages
pkg files NAME              # an installed package's files
pkg query                   # every package, a tab-separated line each (for programs: SiPM)
```

Anyone can look (`search`, `info`, `list`, `files`, `query`). Changing the
system (`update`, `install`, `upgrade`, `remove`, `add`) needs root: pkg is
set-user-ID root and, for another user, asks for root's password (on the
terminal, or with `-P` the first line of standard input, which is how SiPM
hands it over), as the installer does. `create` runs with the caller's own
rights.

Programs land in `/usr/pkg/bin`, which is on every user's `PATH`; shared
libraries in `/usr/pkg/lib`, which the dynamic linker searches
(`/etc/ld-musl-x86_64.path`); headers in `/usr/pkg/include`, so that software
built on SIEOS can use them.

## Software built on SIEOS itself

Build it with the prefix `/usr/pkg`, install it into a staging directory, and
package that:

```sh
./configure --prefix=/usr/pkg && make
make install DESTDIR=/tmp/stage
pkg create -n NAME -v 1.0-1 -s "what it is" -d "DEPENDENCIES" -o NAME-1.0-1.spkg /tmp/stage
pkg add -u NAME-1.0-1.spkg  # -u: it is in no signed index
```

It is then managed like any other package (`pkg list`, `pkg files`, `pkg remove`).

## The packages

| Package | What |
|---------|------|
| `zlib` | the zlib compression library (shared) |
| `openssl` | OpenSSL 3.5 (LTS): libcrypto, libssl and the `openssl` command; trusts `/etc/ssl/certs.pem` |
| `curl` | `curl` and libcurl (on OpenSSL) |
| `openssh` | the OpenSSH client: `ssh`, `scp`, `sftp`, `ssh-keygen`, `ssh-agent`, `ssh-add`, `ssh-keyscan` (configuration in `/usr/pkg/etc/ssh`) |
| `rsync` | `rsync`, locally or over ssh |
| `git` | git (https, http, ssh and local remotes). SIEOS has no pager or editor: output is not paged, and `git commit` needs `-m` (or `core.editor` set) |
| `lua`, `pigz` | Lua 5.4; parallel gzip |
| `mir` | MiR (Make it Real): sia makes applications ([mir.md](mir.md)) |

## Building packages (on the build machine)

A package is a recipe, `ports/pkgs/NAME/recipe`:

```
name     = pigz
version  = 2.8-1                     # the software's version, then the package's revision
summary  = parallel gzip: compresses with every processor
source   = https://zlib.net/pigz/pigz-2.8.tar.gz
sha256   = eb872b4f0e1f0ebe59c9f7bd8c506c4204893ba6a8492de31df416f0d5170fd0
depends  = zlib                      # other packages, by name
build    = make -j$JOBS CC="$CC" CFLAGS="-O2 $CPPFLAGS" LDFLAGS="$LDFLAGS"
install  = mkdir -p $DESTDIR$PREFIX/bin
           cp pigz unpigz $DESTDIR$PREFIX/bin/
```

- `build` and `install` are shell commands run in the unpacked source (a value
  continues on lines that start with blanks). They have `CC`, `CXX`, `AR`,
  `RANLIB`, `STRIP` (the cross toolchain), `HOST` (`x86_64-pc-sieos`),
  `PREFIX` (`/usr/pkg`), `DESTDIR` (where to install), `JOBS`, `CONFIGURE`
  (`./configure --host=... --prefix=...`: for autoconf software,
  `build = $CONFIGURE && make -j$JOBS`, `install = make install
  DESTDIR=$DESTDIR`), and `CPPFLAGS`, `LDFLAGS` and `PKG_CONFIG_LIBDIR`
  pointing at the dependencies' files.
- SIEOS's own software, kept in this repository, says `source = tree:PATH` (a folder of
  the repository; no `sha256`), and its commands also have `TREE`, the repository's root
  (MiR: `ports/pkgs/mir`, `source = tree:user/mir`).
- `*.patch` files next to the recipe are applied first (`patch -p1`).
- The installed tree is stripped, its libtool archives and documentation
  (man, info, doc) dropped. Everything must be under `$PREFIX`.
- Downloaded sources are taught about SIEOS first: `config.sub` knows `x86_64-pc-sieos`,
  and libtool, in `configure` scripts, builds shared libraries for it as for Linux
  (ELF, sonames, `ld.so`). Without that, libtool would make static libraries only.

```sh
make pkgs                   # build every recipe (dependencies first): build/repo/NAME-VERSION.spkg
make repo                   # and write build/repo/INDEX, signed (INDEX.sig)
make repo-serve             # serve build/repo on port 8000 (SIEOS in QEMU: http://10.0.2.2:8000/)
```

To test a repository with SIEOS in QEMU, put `http://10.0.2.2:8000/` in
`/etc/pkg/repos` (the line is there, commented out).

## Publishing

The repository is **https://www.sieos.org/repo/** (the default in
`/etc/pkg/repos`): upload `build/repo`'s files there (`INDEX`, `INDEX.sig`,
the `.spkg` files). `INDEX` lists the two newest versions of each package.
SIEOS's TLS client speaks TLS 1.3, which the server must offer, with a
certificate from a public certificate authority.

## The signing key

`make` creates an ECDSA P-256 key the first time it needs one,
`~/.config/sieos/pkg-signing-key.pem` (`make PKG_KEY=path` for another). It
never goes in the source tree: whoever holds it can publish packages SIEOS
systems install. The images carry its public half, `/etc/pkg/keys/build.pub`:
a repository's index is used only if its signature matches a key there. Keep
the key (and a backup): systems built with it accept only indexes it signed.

## Formats

- **Package** (`.spkg`): a gzip-compressed ustar archive. Its first member,
  `+MANIFEST`, holds `name`, `version`, `summary`, `depends` and `size` (the
  installed bytes) as `key: value` lines; the others are regular files,
  directories and symbolic links, all under `usr/pkg/`.
- **Index** (`INDEX`): one record per package version, blank-line separated:
  the manifest's fields, `file` (the `.spkg` name) and `sha256` (the file's).
- **Signature** (`INDEX.sig`): the DER ECDSA P-256 signature of the index's
  SHA-256. Keys (`/etc/pkg/keys/*`) are the public point `04||X||Y` in
  hexadecimal.
- **Database**: `/var/lib/pkg/NAME/MANIFEST` and `FILES` (`f`, `l` or `d` and
  a path, a line each); indexes and downloads in `/var/cache/pkg`.

## What pkg checks

- An index is used only if its signature matches a key in `/etc/pkg/keys`; a
  downloaded package only if its SHA-256 is the index's.
- A package's files must be regular files, directories or symbolic links under
  `/usr/pkg`, with no `..`; nothing is written through a symbolic link (one
  already installed or one the package ships), so nothing lands outside
  `/usr/pkg`. These are checked for the whole package before a file is written.
- A file another package owns, or one no package owns, is not replaced
  (`-f` overrides).
- Files are written under a temporary name, then renamed over.
- An upgrade removes the files the new version no longer has; `remove` removes
  a package's files and its empty directories (`/usr/pkg` and its first level
  stay).
- `pkg` takes a lock (`/var/lib/pkg/.lock`): one change at a time.
- For a user other than root, a change needs root's password (a second's delay
  after a wrong one), and `PKG_ROOT` (installing into another tree, for tests)
  is ignored.
