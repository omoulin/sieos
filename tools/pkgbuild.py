#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""
pkgbuild.py RECIPE REPO WORK - cross-build a package for SIEOS's pkg.

RECIPE is a directory ports/pkgs/NAME with a file named recipe:

    name     = lua
    version  = 5.4.7-1                  (the software's version, then the package's revision)
    summary  = Lua, a small, fast scripting language
    source   = https://www.lua.org/ftp/lua-5.4.7.tar.gz
    sha256   = 9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
    depends  = zlib                      (other packages, by name; optional)
    build    = make -j$JOBS CC="$CC" ...  (shell commands, run in the source tree)
    install  = make install INSTALL_TOP=$DESTDIR$PREFIX

SIEOS's own software, kept in this repository, says "source = tree:PATH" (a
folder of the repository, copied; no sha256) and is built with TREE (the
repository's root) too.

Optionally *.patch files are applied (patch -p1) before building.  build and
install run with: HOST (x86_64-pc-sieos), CC, CXX, AR, RANLIB, STRIP, PREFIX
(/usr/pkg), DESTDIR (the staging root), JOBS, CONFIGURE (./configure with
--host and --prefix, and SIEOS's config.sub), and CPPFLAGS/LDFLAGS/
PKG_CONFIG_LIBDIR pointing at the dependencies' files (their packages,
unpacked in WORK/deps).  A value continues on lines that start with blanks.

The staged tree is stripped (programs and shared libraries), its libtool
archives and documentation dropped, and packed as REPO/NAME-VERSION.spkg:
a gzip-compressed ustar archive, +MANIFEST first, then usr/pkg/... (pkg's
format).  Archive members are sorted, owned by root, dated 0: a package
rebuilt from the same inputs is the same file.
"""
import gzip
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ports'))
from build import teach_config_sub, teach_libtool                 # noqa: E402

TARGET = 'x86_64-pc-sieos'
PREFIX = '/usr/pkg'
FIELDS = ('name', 'version', 'summary', 'source', 'sha256', 'depends', 'build', 'install')


def read_recipe(d):
    r, key = {}, None
    for line in open(os.path.join(d, 'recipe')):
        if line.startswith('#') or not line.strip():
            key = None if not line.strip() else key
            continue
        if line[0] in ' \t' and key:
            r[key] += '\n' + line.strip()
            continue
        m = re.match(r'(\w+)\s*=\s*(.*)$', line.rstrip('\n'))
        if not m or m.group(1) not in FIELDS:
            sys.exit('%s/recipe: bad line: %s' % (d, line.rstrip()))
        key = m.group(1)
        r[key] = m.group(2).strip()
    for f in ('name', 'version', 'summary', 'source', 'install') + (() if r.get('source', '').startswith('tree:') else ('sha256',)):
        if not r.get(f):
            sys.exit('%s/recipe: no %s' % (d, f))
    if not re.fullmatch(r'[a-z0-9][a-z0-9._+-]*', r['name']):
        sys.exit('%s/recipe: bad name %r' % (d, r['name']))
    r.setdefault('depends', '')
    r.setdefault('build', '')
    return r


def fetch(url, sha, dl):
    os.makedirs(dl, exist_ok=True)
    path = os.path.join(dl, os.path.basename(url))
    fetch_sh = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fetch.sh')
    subprocess.run([fetch_sh, url, path], check=True)
    got = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    if got != sha:
        os.rename(path, path + '.bad')
        sys.exit('%s: sha256 %s, the recipe says %s' % (path, got, sha))
    return path


def spkg_members(path):
    """The manifest (dict) and members of a package."""
    with tarfile.open(path, 'r:gz') as t:
        m = t.getmembers()
        man = t.extractfile(m[0]).read().decode()
    return dict(l.split(': ', 1) for l in man.splitlines() if ': ' in l), m


def unpack_deps(r, repo, deps_root):
    """The dependencies' packages (the newest of each in REPO), unpacked for building against."""
    shutil.rmtree(deps_root, ignore_errors=True)
    os.makedirs(deps_root)
    todo, seen = r['depends'].split(), set()
    while todo:
        n = todo.pop()
        if n in seen:
            continue
        seen.add(n)
        cands = [f for f in os.listdir(repo) if f.startswith(n + '-') and f.endswith('.spkg')]
        cands = [f for f in cands if spkg_members(os.path.join(repo, f))[0].get('name') == n]
        if not cands:
            sys.exit('%s: needs %s, which is not built yet (build it first)' % (r['name'], n))
        best = sorted(cands, key=lambda f: [int(x) if x.isdigit() else x
                                             for x in re.split(r'(\d+)', f)])[-1]
        man, _ = spkg_members(os.path.join(repo, best))
        todo += man.get('depends', '').split()
        with tarfile.open(os.path.join(repo, best), 'r:gz') as t:
            t.extractall(deps_root, members=[m for m in t.getmembers() if m.name != '+MANIFEST'],
                         filter='tar')


def strip_tree(root, strip):
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            if os.path.islink(p):
                continue
            if f.endswith('.la'):
                os.remove(p)
                continue
            with open(p, 'rb') as fh:
                if fh.read(4) != b'\x7fELF':
                    continue
            args = [strip, '--strip-unneeded', p] if '.so' in f or f.endswith('.a') else [strip, p]
            if f.endswith('.a'):
                args = [strip, '--strip-debug', p]
            subprocess.run(args, stderr=subprocess.DEVNULL)
    for doc in ('share/man', 'share/doc', 'share/info', 'share/gtk-doc'):
        shutil.rmtree(os.path.join(root, 'usr/pkg', doc), ignore_errors=True)


def pack(r, stage, out):
    members, size = [], 0
    for d, dirs, files in os.walk(os.path.join(stage, 'usr/pkg')):
        dirs.sort()
        rel = os.path.relpath(d, stage)
        members.append(rel)
        for f in sorted(files):
            members.append(os.path.join(rel, f))
    for m in members:
        p = os.path.join(stage, m)
        if not os.path.islink(p) and os.path.isfile(p):
            size += os.path.getsize(p)
    manifest = ('name: %s\nversion: %s\nsummary: %s\ndepends: %s\nsize: %d\n' %
                (r['name'], r['version'], r['summary'], ' '.join(r['depends'].split()), size)).encode()
    # (written through gzip as it goes: a package may be gigabytes, a model's)
    with open(out, 'wb') as fh, gzip.GzipFile(filename='', mode='wb', fileobj=fh, mtime=0, compresslevel=9) as gz, \
            tarfile.open(fileobj=gz, mode='w', format=tarfile.USTAR_FORMAT) as t:
        ti = tarfile.TarInfo('+MANIFEST')
        ti.size, ti.mode, ti.mtime, ti.uname, ti.gname = len(manifest), 0o644, 0, 'root', 'root'
        t.addfile(ti, io.BytesIO(manifest))
        for m in members:
            ti = t.gettarinfo(os.path.join(stage, m), arcname=m)
            if not (ti.isfile() or ti.isdir() or ti.issym()):
                sys.exit('%s: %s: only files, directories and symbolic links can be packaged' % (r['name'], m))
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = 'root'
            ti.mtime = 0
            ti.mode &= 0o7777
            ti.mode = ti.mode & ~0o022 if ti.isfile() or ti.isdir() else ti.mode
            if ti.isfile():
                with open(os.path.join(stage, m), 'rb') as fh:
                    t.addfile(ti, fh)
            else:
                t.addfile(ti)
    return size


def main():
    recipe, repo, work = (os.path.abspath(a) for a in sys.argv[1:4])
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    r = read_recipe(recipe)
    cross = os.path.join(root, 'build', 'cross', 'bin')
    os.makedirs(repo, exist_ok=True)
    wdir = os.path.join(work, r['name'])
    shutil.rmtree(wdir, ignore_errors=True)
    os.makedirs(wdir)
    src = os.path.join(wdir, 'src')
    if r['source'].startswith('tree:'):                  # SIEOS's own: a folder of this repository
        tree_dir = os.path.normpath(os.path.join(root, r['source'][5:]))
        if not tree_dir.startswith(root + os.sep) or not os.path.isdir(tree_dir):
            sys.exit('%s: no folder %s in the repository' % (r['name'], r['source'][5:]))
        shutil.copytree(tree_dir, src, symlinks=True)
    else:
        tarball = fetch(r['source'], r['sha256'], os.path.join(work, 'dl'))
        os.makedirs(src)
        subprocess.run(['tar', 'xf', tarball, '-C', src, '--strip-components=1'], check=True)
        teach_config_sub(src)
        teach_libtool(src)
    for p in sorted(f for f in os.listdir(recipe) if f.endswith('.patch')):
        subprocess.run(['patch', '-p1', '-s', '-d', src, '-i', os.path.join(recipe, p)], check=True)
    deps = os.path.join(wdir, 'deps')
    unpack_deps(r, repo, deps)
    stage = os.path.join(wdir, 'stage')
    os.makedirs(stage)
    dp = deps + PREFIX
    env = dict(os.environ, HOST=TARGET, PREFIX=PREFIX, DESTDIR=stage, JOBS=str(os.cpu_count()),
               CC=TARGET + '-gcc', CXX=TARGET + '-g++', AR=TARGET + '-ar', RANLIB=TARGET + '-ranlib',
               STRIP=TARGET + '-strip', PATH=cross + ':' + os.environ['PATH'],
               CFLAGS='-O2', CXXFLAGS='-O2',
               CPPFLAGS='-I%s/include' % dp, LDFLAGS='-L%s/lib -Wl,-rpath-link,%s/lib' % (dp, dp),
               PKG_CONFIG_LIBDIR='%s/lib/pkgconfig:%s/share/pkgconfig' % (dp, dp),
               PKG_CONFIG_SYSROOT_DIR=deps,
               CONFIGURE='./configure --host=%s --prefix=%s' % (TARGET, PREFIX), TREE=root)
    log = open(os.path.join(wdir, 'build.log'), 'w')
    for step in ('build', 'install'):
        if not r[step]:
            continue
        log.write('+ %s\n' % r[step])
        log.flush()
        rc = subprocess.run(['sh', '-ec', r[step]], cwd=src, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
        if rc:
            log.close()
            sys.stdout.write(open(os.path.join(wdir, 'build.log')).read()[-3000:])
            sys.exit('%s: %s failed (see %s)' % (r['name'], step, os.path.join(wdir, 'build.log')))
    if not os.path.isdir(os.path.join(stage, 'usr/pkg')):
        sys.exit('%s: nothing installed under %s (install with DESTDIR=$DESTDIR, prefix $PREFIX)' %
                 (r['name'], PREFIX))
    strays = [f for f in os.listdir(stage) if f != 'usr'] + \
             [f for f in os.listdir(os.path.join(stage, 'usr')) if f != 'pkg']
    if strays:
        sys.exit('%s: installed outside %s: %s' % (r['name'], PREFIX, ' '.join(strays)))
    strip_tree(stage, os.path.join(cross, TARGET + '-strip'))
    out = os.path.join(repo, '%s-%s.spkg' % (r['name'], r['version']))
    size = pack(r, stage, out)
    print('%s: %d KiB installed, %d KiB packaged' % (os.path.relpath(out, root), (size + 1023) // 1024,
                                                     (os.path.getsize(out) + 1023) // 1024))


if __name__ == '__main__':
    main()
