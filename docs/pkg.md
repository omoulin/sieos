# Packages: pkg

SIEOS's base system (everything `make` builds into the images) stays as it is.
Software added to it is managed as **packages**: installed under `/usr/pkg`
(never over a base file), updated and removed with `pkg`, from signed
repositories.

## Using pkg (on SIEOS, as root)

```sh
pkg update                  # fetch the repositories' signed indexes
pkg search [WORD]           # what they offer (name or summary)
pkg info NAME               # version, dependencies, size, installed or not
pkg install NAME...         # install, with the dependencies first
pkg upgrade [NAME...]       # newer versions of the installed packages
pkg remove [-f] NAME...     # remove (refused while another package needs it; -f forces)
pkg list                    # the installed packages
pkg files NAME              # an installed package's files
```

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
- `*.patch` files next to the recipe are applied first (`patch -p1`).
- The installed tree is stripped, its libtool archives and documentation
  (man, info, doc) dropped. Everything must be under `$PREFIX`.

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
