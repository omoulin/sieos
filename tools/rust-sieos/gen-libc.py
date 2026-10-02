#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""
gen-libc.py - the Rust libc crate's definitions, made SIEOS's.

  gen-libc.py DOC_JSON LIBC_SRC SYSROOT WORK OUT_CRATE

Rust's libc crate describes Linux with musl: its constants, type sizes and
structure layouts are Linux's.  SIEOS's C library is musl too, with SIEOS's
values (errno, open flags, signals, sockets, ioctls, termios...) and some
layouts of its own.  This compares the two: every integer constant, type
alias and structure the crate exports for x86_64-unknown-linux-musl (its
rustdoc JSON, DOC_JSON) is printed by a Rust program built against the crate
(LIBC_SRC) and by a C program built with SIEOS's headers (SYSROOT/usr/include);
both run on the build host (x86_64, as SIEOS).  OUT_CRATE is a copy of the
crate with what differs changed where the crate defines it (the item's span in
the JSON): constants and type aliases rewritten, structures laid out as SIEOS
has them (its member order, sizes and padding).  WORK/report.txt says what
changed, and what SIEOS's headers do not have (left as Linux's).
"""
import json
import os
import re
import subprocess
import sys

DOC, LIBC, SYSROOT, WORK, OUT = sys.argv[1:6]
os.makedirs(WORK, exist_ok=True)
CARGO = os.path.expanduser('~/.cargo/bin/cargo')

d = json.load(open(DOC))
idx = d['index']
item = lambda i: idx[str(i)]

PRIMS = {'i8': (1, True), 'u8': (1, False), 'i16': (2, True), 'u16': (2, False), 'i32': (4, True),
         'u32': (4, False), 'i64': (8, True), 'u64': (8, False), 'i128': (16, True), 'u128': (16, False),
         'isize': (8, True), 'usize': (8, False)}
PRIMS.update({'c_char': (1, True), 'c_schar': (1, True), 'c_uchar': (1, False), 'c_short': (2, True),
              'c_ushort': (2, False), 'c_int': (4, True), 'c_uint': (4, False), 'c_long': (8, True),
              'c_ulong': (8, False), 'c_longlong': (8, True), 'c_ulonglong': (8, False)})   # (core::ffi's)
INT_OF = {(1, True): 'i8', (1, False): 'u8', (2, True): 'i16', (2, False): 'u16', (4, True): 'i32',
          (4, False): 'u32', (8, True): 'i64', (8, False): 'u64'}

# the root's items (the crate's public names), by kind
aliases, consts, structs = {}, {}, {}
for k, v in idx.items():
    if v.get('crate_id') != 0 or v.get('visibility') != 'public' or not v.get('name'):
        continue
    inner = v['inner']
    if 'type_alias' in inner:
        aliases[v['name']] = inner['type_alias']['type']
    elif 'constant' in inner:
        consts[v['name']] = inner['constant']['type']
    elif 'struct' in inner and 'plain' in inner['struct']['kind']:
        packed = any('packed' in json.dumps(a) and '"packed": null' not in json.dumps(a) for a in v['attrs'])
        fields = []
        for f in inner['struct']['kind']['plain']['fields']:
            fi = item(f)
            if fi.get('visibility') == 'public':
                fields.append((fi['name'], fi['inner']['struct_field']))
        structs[v['name']] = (fields, packed)


def tname(t):
    """A JSON type as Rust source (the types libc's fields and constants use)."""
    if 'primitive' in t:
        return t['primitive']
    if 'resolved_path' in t:
        p = t['resolved_path']
        name = p['path'].split('::')[-1]
        args = p.get('args')
        if args and args.get('angle_bracketed', {}).get('args'):
            parts = [tname(a['type']) for a in args['angle_bracketed']['args'] if 'type' in a]
            return None if None in parts else '%s<%s>' % (name, ', '.join(parts))
        return name
    if 'array' in t:
        return '[%s; %s]' % (tname(t['array']['type']), t['array']['len'])
    if 'raw_pointer' in t:
        rp = t['raw_pointer']
        return '*%s %s' % ('mut' if rp['is_mutable'] else 'const', tname(rp['type']))
    if 'function_pointer' in t:
        return None
    if 'tuple' in t and not t['tuple']:
        return '()'
    return None


def int_info(t, depth=0):
    """(size, signed) of an integer type, through the aliases; None otherwise."""
    if depth > 8:
        return None
    if 'primitive' in t:
        return PRIMS.get(t['primitive'])
    if 'resolved_path' in t:
        name = t['resolved_path']['path'].split('::')[-1]
        if name in PRIMS:
            return PRIMS[name]
        if name in aliases:
            return int_info(aliases[name], depth + 1)
    return None


def build_iter(path, lines, compile_cmd, err_re, what):
    """Compile a probe, dropping the lines its compiler refuses, until it builds."""
    head, body, tail = lines
    body = list(body)
    for _ in range(12):
        src = '\n'.join(head + [b for b, _ in body] + tail) + '\n'
        open(path, 'w').write(src)
        r = subprocess.run(compile_cmd, capture_output=True, text=True)
        if r.returncode == 0:
            return [n for _, n in body]
        bad = set()
        for m in re.finditer(err_re, r.stderr):
            ln = int(m.group(1)) - 1 - len(head)
            if 0 <= ln < len(body):
                bad.add(ln)
        if not bad:
            sys.exit('%s probe: errors not tied to a line:\n%s' % (what, r.stderr[-3000:]))
        body = [b for i, b in enumerate(body) if i not in bad]
    sys.exit('%s probe: did not converge' % what)


# ---------------------------------------------------------------- the Rust side
rs_dir = os.path.join(WORK, 'rprobe')
os.makedirs(os.path.join(rs_dir, 'src'), exist_ok=True)
open(os.path.join(rs_dir, 'Cargo.toml'), 'w').write(
    '[package]\nname = "rprobe"\nversion = "0.1.0"\nedition = "2021"\n\n'
    '[dependencies]\nlibc = { path = "%s" }\n' % os.path.abspath(LIBC))
rbody = []
for n, t in sorted(consts.items()):
    if int_info(t):
        rbody.append(('    println!("C {} {}", "%s", libc::%s as i128);' % (n, n), n))
for n, t in sorted(aliases.items()):
    ii = int_info(t)
    if ii:
        rbody.append(('    println!("T {} {} {}", "%s", size_of::<libc::%s>(), (0 as libc::%s).wrapping_sub(1) < (0 as libc::%s));'
                      % (n, n, n, n), n))
for n, (fields, packed) in sorted(structs.items()):
    rbody.append(('    println!("S {} {} {}", "%s", size_of::<libc::%s>(), align_of::<libc::%s>());' % (n, n, n), n))
    if packed:
        continue
    for f, ft in fields:
        rbody.append(('    println!("F {} {} {} {}", "%s", "%s", offset_of!(libc::%s, %s), fsz(|s: &libc::%s| &s.%s));'
                      % (n, f, n, f, n, f), n))
rhead = ['#![allow(deprecated, unused_imports, invalid_value)]', 'use std::mem::{size_of, align_of, offset_of};',
         'fn fsz<T, F>(_: fn(&T) -> &F) -> usize { size_of::<F>() }', 'fn main() {']
rmain = os.path.join(rs_dir, 'src', 'main.rs')
build_iter(rmain, (rhead, rbody, ['}']),
           [CARGO, 'build', '-q', '--manifest-path', os.path.join(rs_dir, 'Cargo.toml'),
            '--target', 'x86_64-unknown-linux-musl', '--target-dir', os.path.join(WORK, 'rtarget')],
           r'src/main\.rs:(\d+):', 'Rust')
rout = subprocess.run([os.path.join(WORK, 'rtarget/x86_64-unknown-linux-musl/debug/rprobe')],
                      capture_output=True, text=True, check=True).stdout


def parse(out):
    c, t, s, f = {}, {}, {}, {}
    for line in out.splitlines():
        p = line.split()
        if p[0] == 'C':
            c[p[1]] = int(p[2])
        elif p[0] == 'T':
            t[p[1]] = (int(p[2]), p[3] in ('true', '1'))
        elif p[0] == 'S':
            s[p[1]] = (int(p[2]), int(p[3]))
        elif p[0] == 'F':
            f[(p[1], p[2])] = tuple(int(x) for x in p[3:])
    return c, t, s, f


rc, rt, rs, rf = parse(rout)

# ---------------------------------------------------------------- the C side
inc = os.path.join(SYSROOT, 'usr', 'include')
headers = []
for root, dirs, files in os.walk(inc):
    rel = os.path.relpath(root, inc)
    if rel.split(os.sep)[0] in ('sieos', 'sia', 'facet', 'c++', 'scsi', 'GL', 'EGL', 'vulkan', 'KHR', 'drm', 'libdrm'):
        continue
    if rel != '.' and rel.split(os.sep)[0] not in ('sys', 'net', 'netinet', 'arpa', 'netpacket'):
        continue
    for f in files:
        if f.endswith('.h') and f not in ('tgmath.h', 'complex.h', 'stdatomic.h'):
            headers.append(os.path.normpath(os.path.join(rel, f)))
def header_ok(h):
    """Does the header compile alone (some include Linux's, which SIEOS has not)?"""
    r = subprocess.run(['gcc', '-w', '-fsyntax-only', '-nostdinc', '-isystem', inc, '-D_GNU_SOURCE', '-x', 'c', '-'],
                       input='#include <%s>\n' % h, capture_output=True, text=True)
    return r.returncode == 0


headers = [h for h in headers if header_ok(h)]
cs_head = ['#define _GNU_SOURCE 1', '#include <stdio.h>', '#include <stddef.h>']
cs_head += ['#include <%s>' % h for h in sorted(headers) if h not in ('stdio.h', 'stddef.h')]
cs_head += ['int main(void) {']
P = lambda n: '    printf("C %%s %%lld\\n", "%s", (long long)(%s));' % (n, n)
T = lambda n: '    printf("T %%s %%zu %%d\\n", "%s", sizeof(%s), (%s)-1 < (%s)0);' % (n, n, n, n)
SS = lambda n, c: '    printf("S %%s %%zu %%zu\\n", "%s", sizeof(*(%s *)0), __alignof__(*(%s *)0));' % (n, c, c)
F = lambda n, c, f: ('    printf("F %%s %%s %%zu %%zu\\n", "%s", "%s", offsetof(%s, %s), sizeof(((%s *)0)->%s));'
                     % (n, f, c, f, c, f))
# (a structure: its struct tag, else a typedef; sizeof(*(X *)0) fails for a function, as stat)
# Rust's names for C members that are paths (Linux's stat has st_atime_nsec; SIEOS's st_atim.tv_nsec)
FIELD_C = {'st_atime_nsec': 'st_atim.tv_nsec', 'st_mtime_nsec': 'st_mtim.tv_nsec', 'st_ctime_nsec': 'st_ctim.tv_nsec'}
# siginfo_t's accessors (methods in the crate), at SIEOS's offsets
SI_ACC = {'si_addr': '*mut c_void', 'si_value': 'sigval', 'si_pid': 'pid_t', 'si_uid': 'uid_t',
          'si_status': 'c_int', 'si_utime': 'clock_t', 'si_stime': 'clock_t'}
F2 = lambda n, c, f, cf: ('    printf("F %%s %%s %%zu %%zu %%zu\\n", "%s", "%s", offsetof(%s, %s), sizeof(((%s *)0)->%s), '
                          '__alignof__(((%s *)0)->%s));' % (n, f, c, cf, c, cf, c, cf))
cbody = []
for a in SI_ACC:
    cbody.append((F2('siginfo_t', 'siginfo_t', '@' + a, a), 'siginfo_t'))
for n, t in sorted(consts.items()):
    if int_info(t) and n in rc:
        cbody.append((P(n), n))
for n in sorted(rt):
    cbody.append((T(n), n))
for n, (fields, packed) in sorted(structs.items()):
    if n not in rs:
        continue
    for cname in ('struct %s' % n, n):
        cbody.append((SS(n, cname), n))
        for fld, ft in fields:
            if (n, fld) in rf:
                cbody.append((F2(n, cname, fld, FIELD_C.get(fld, fld)), n))
cmain = os.path.join(WORK, 'cprobe.c')
cexe = os.path.join(WORK, 'cprobe')
build_iter(cmain, (cs_head, cbody, ['return 0; }']),
           ['gcc', '-w', '-nostdinc', '-isystem', inc, '-o', cexe, cmain],
           r'cprobe\.c:(\d+):\d+: error', 'C')
cout = subprocess.run([cexe], capture_output=True, text=True, check=True).stdout
cc, ct, cs, cf = parse(cout)

# ---------------------------------------------------------------- the differences, into a copy of the crate
import shutil
shutil.rmtree(OUT, ignore_errors=True)
shutil.copytree(LIBC, OUT, copy_function=shutil.copy)   # (new times: cargo must see the crate changed)
spans = {}
for k, v in idx.items():
    if v.get('crate_id') == 0 and v.get('visibility') == 'public' and v.get('name') and v.get('span'):
        kind = list(v['inner'])[0]
        spans[(kind, v['name'])] = v['span']
edits = {}                                             # file -> [(begin line, end line, function of the text)]


def edit(kind, name, fn):
    sp = spans[(kind, name)]
    edits.setdefault(sp['filename'], []).append((sp['begin'][0], sp['end'][0], fn))


def rtype(t):
    """A field's type as source inside the crate's modules (its own names through crate::)."""
    name = tname(t)
    if name is None:
        return None
    return re.sub(r'\b([A-Za-z_][A-Za-z0-9_]*)\b',
                  lambda m: 'crate::' + m.group(1) if (m.group(1) in aliases or m.group(1) in structs or
                                                      m.group(1).startswith('c_') or m.group(1) in ('size_t', 'ssize_t'))
                  else m.group(1), name)


rep = []
nconst = 0
literal = {v['name']: v['inner']['constant']['const'].get('is_literal', False)
           for v in idx.values() if v.get('crate_id') == 0 and 'constant' in v['inner']}
for n in sorted(rc):
    # (a constant defined from others, as SOCK_CLOEXEC from O_CLOEXEC, would follow their new values: pinned)
    if n in cc and (cc[n] != rc[n] or not literal.get(n, True)):
        v = cc[n]
        edit('constant', n, lambda text, n=n, v=v: re.sub(
            r'(\bpub const %s\s*:\s*([^=]+?)\s*=\s*)[^;]+;' % re.escape(n),
            lambda m: '%s%di128 as %s;' % (m.group(1), v, m.group(2)), text, count=1))
        nconst += 1
missing = sorted(n for n in rc if n not in cc)
rep.append('%d constants differ, %d are not in SIEOS\'s headers (left as Linux\'s)' % (nconst, len(missing)))
for n in sorted(rt):
    if n in ct and ct[n] != rt[n]:
        new = INT_OF[ct[n]]
        edit('type_alias', n, lambda text, n=n, new=new: re.sub(r'(\bpub type %s\s*=\s*)[^;]+;' % re.escape(n),
                                                              lambda m: m.group(1) + new + ';', text, count=1))
        rep.append('type %s: %s -> %s' % (n, rt[n], ct[n]))


def c_size_of_name(t):
    """SIEOS's size of a named type (an alias or a structure), or None."""
    name = tname(t)
    if name in ct:
        return ct[name][0]
    if name in cs:
        return cs[name][0]
    ii = int_info(t)
    return ii[0] if ii else None


def replace_struct(text, n, body, align):
    """The definition of struct n in text (a block of the s! macros, or plain) with body."""
    m = re.search(r'\bpub struct %s\s*\{' % re.escape(n), text)
    if not m:
        sys.exit('struct %s: no definition in its span' % n)
    depth, i = 0, m.end() - 1
    while True:
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        if depth == 0:
            break
        i += 1
    head = '#[repr(align(%d))]\n    ' % align if align else ''
    return text[:m.start()] + head + 'pub struct %s {\n%s\n    }' % (n, body) + text[i + 1:]


nstruct = 0
for n, (fields, packed) in sorted(structs.items()):
    if n not in cs or n not in rs or packed or rs[n][0] == 0:
        continue
    diff = cs[n] != rs[n] or any((n, f) in cf and cf[(n, f)][:2] != rf.get((n, f)) for f, _ in fields)
    if not diff:
        continue
    known = sorted((cf[(n, f)][0], cf[(n, f)][1], cf[(n, f)][2], f, ft) for f, ft in fields if (n, f) in cf)
    lost = [f for f, _ in fields if (n, f) not in cf]
    lines, off, pad, nat_align = [], 0, 0, 1
    for o, sz, al, f, ft in known:
        if o < off:
            continue                                   # (overlapping: a union in C; the first one kept)
        if o > (off + al - 1) // al * al:              # (more than repr(C)'s own padding)
            lines.append('        pub __sieos_pad%d: [u8; %d],' % (pad, o - off))
            pad += 1
        ty = rtype(ft)
        if rf[(n, f)][1] != sz and c_size_of_name(ft) != sz:
            ii = int_info(ft)
            if ii and (sz, ii[1]) in INT_OF:
                ty = INT_OF[(sz, ii[1])]
            elif 'array' in ft and int_info(ft['array']['type']):
                ty = '[%s; %d]' % (rtype(ft['array']['type']), sz // int_info(ft['array']['type'])[0])
            else:
                ty = None
        if ty is None:
            ty = '[u8; %d]' % sz
            al = 1
        lines.append('        pub %s: %s,' % (f, ty))
        off = o + sz
        nat_align = max(nat_align, al)
    size, align = cs[n]
    eff_align = max(nat_align, align)
    nat_size = (off + eff_align - 1) // eff_align * eff_align
    if nat_size < size:
        lines.append('        pub __sieos_pad%d: [u8; %d],' % (pad, size - off))
    body = '\n'.join(lines)
    edit('struct', n, lambda text, n=n, body=body, a=align if align > nat_align else 0: replace_struct(text, n, body, a))
    nstruct += 1
    rep.append('struct %s: Linux %s, SIEOS %s%s' % (n, rs[n], cs[n], ('; not in C: ' + ', '.join(lost)) if lost else ''))
rep.append('%d structures differ' % nstruct)

for fn, es in edits.items():
    path = os.path.join(OUT, fn)
    lines = open(path).read().split('\n')
    for b, e, f in sorted(es, key=lambda x: -x[0]):
        text = '\n'.join(lines[b - 1:e])
        lines[b - 1:e] = f(text).split('\n')
    open(path, 'w').write('\n'.join(lines))
open(os.path.join(WORK, 'report.txt'), 'w').write('\n'.join(rep) + '\n\nnot in SIEOS\'s headers:\n' +
                                                   '\n'.join(missing) + '\n')
print('\n'.join(rep[:3] + [rep[-1]]))
