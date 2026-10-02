#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""
gap.py CHROMIUM_SRC SYSROOT GAPDIR - what Chromium (built for Linux) uses that SIEOS has not.

The sources a Linux build of Chromium compiles (its own and its bundled third
parties; not Windows', macOS', Android's, ChromeOS', Fuchsia's, iOS', not the
tests) are scanned for:
  - system headers (#include <...>) SIEOS's sysroot has not;
  - system calls by number (__NR_x, SYS_x) SIEOS answers ENOSYS (GAPDIR/nosys.txt);
  - C library functions (glibc's exported names, called as functions) SIEOS's
    libc has not (GAPDIR/libc-symbols.txt);
  - /proc, /sys and /dev paths;
  - Linux constants of the kernel's interfaces (prctl, clone, madvise, sockets,
    ioctls, clocks, futexes, seccomp...) SIEOS's headers do not define.
GAPDIR/report.txt: each with how many files use it and where (a few).
"""
import collections
import os
import re
import subprocess
import sys

SRC, SYSROOT, GAP = sys.argv[1:4]
INC = os.path.join(SYSROOT, 'usr', 'include')

SKIP_DIRS = {'.git', 'test', 'tests', 'testing', 'web_tests', 'unittests', 'fuzzers', 'fuzzer', 'android',
             'win', 'mac', 'ios', 'fuchsia', 'chromeos', 'ash', 'lacros', 'cros', 'apple', 'aix', 'openbsd',
             'freebsd', 'nacl', 'node_modules', 'devtools-frontend', 'java', 'javatests', 'windows',
             'llvm-build', 'depot_tools', 'test_data', 'testdata', 'docs', 'examples', 'benchmarks',
             'perftests', 'tools', 'build_overrides', 'chromecast', 'android_webview', 'ios_internal'}
SKIP_PATTERNS = re.compile(r'_(win|mac|ios|android|fuchsia|chromeos|cros|ash|lacros|apple|aix|openbsd|freebsd|'
                           r'nacl|unittest|browsertest|test|fuzzer|perftest|mock|fake)(_[a-z0-9_]+)?\.(cc|c|cpp|h|mm)$')
TOP = ['base', 'build', 'cc', 'components', 'content', 'crypto', 'device', 'gin', 'gpu', 'ipc', 'media', 'mojo',
       'net', 'sandbox', 'services', 'skia', 'sql', 'storage', 'ui', 'url', 'v8', 'third_party', 'headless',
       'printing', 'pdf', 'dbus', 'courgette', 'cc', 'google_apis']

files = []
for top in TOP:
    root = os.path.join(SRC, top)
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d not in SKIP_DIRS and not d.startswith('.')]
        for f in fns:
            if f.endswith(('.c', '.cc', '.cpp', '.h', '.hpp', '.cxx')) and not SKIP_PATTERNS.search(f):
                files.append(os.path.join(dp, f))
print('%d files scanned' % len(files), file=sys.stderr)

# the tree's own headers (by path suffix): an <x/y.h> found there is not a system one
tree_headers = set()
for f in files:
    if f.endswith(('.h', '.hpp')):
        parts = os.path.relpath(f, SRC).split(os.sep)
        for i in range(len(parts)):
            tree_headers.add('/'.join(parts[i:]))
sys_headers = set(l.strip() for l in open(os.path.join(GAP, 'headers.txt')))
CXX_STD = set('algorithm any array atomic bit bitset cassert cctype cerrno cfloat charconv chrono cinttypes climits '
              'clocale cmath compare complex concepts condition_variable coroutine csetjmp csignal cstdarg cstddef '
              'cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype deque exception execution expected '
              'filesystem format forward_list fstream functional future initializer_list iomanip ios iosfwd iostream '
              'istream iterator latch limits list locale map memory memory_resource mutex new numbers numeric '
              'optional ostream queue random ranges ratio regex scoped_allocator semaphore set shared_mutex '
              'source_location span sstream stack stdexcept stop_token streambuf string string_view syncstream '
              'system_error thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility '
              'valarray variant vector version barrier print mdspan flat_map flat_set generator text_encoding '
              'stdfloat spanstream'.split())

nosys = set(l.strip() for l in open(os.path.join(GAP, 'nosys.txt')))
have = set(l.strip() for l in open(os.path.join(GAP, 'libc-symbols.txt')))

# glibc's functions (the build host's libc, libm, libresolv...): the names a call may be
glibc = set()
for lib in ['libc.so.6', 'libm.so.6', 'libresolv.so.2', 'libdl.so.2', 'libpthread.so.0', 'librt.so.1',
            'libutil.so.1']:
    for d in ['/lib/x86_64-linux-gnu', '/usr/lib/x86_64-linux-gnu']:
        p = os.path.join(d, lib)
        if os.path.exists(p):
            out = subprocess.run(['nm', '-D', '--defined-only', p], capture_output=True, text=True).stdout
            for line in out.splitlines():
                parts = line.split()
                if len(parts) >= 3 and parts[1] in 'TWiV':
                    glibc.add(parts[2].split('@')[0])
            break
glibc = {n for n in glibc if not n.startswith('_') and len(n) > 2}

# the kernel interfaces' constants worth knowing (their families)
CONST_RE = re.compile(r'\b((?:PR|CLONE|MADV|MAP|MFD|F_SEAL|SECCOMP|SECBIT|PTRACE|FUTEX|SO|SCM|SOL|AF|NETLINK|'
                      r'RTM|IFLA|IFA|TIOC|SIOC|RLIMIT|CLOCK|EPOLL|IN|TFD|EFD|O|AT|RENAME|STATX|POSIX_FADV|'
                      r'SCHED|MCL|MREMAP|PROT|SA|SIG|SI|SEGV|BUS|RUSAGE|IPPROTO|TCP|IP|IPV6|MSG|SHM|SEM|IPC|'
                      r'PERF|BPF|KCMP|MEMBARRIER|UFFD|NT|AUDIT|CAP|PERSONALITY|ADDR|RLIM|XATTR|FS|FIOC|FAN|'
                      r'SYS_SECCOMP|ELF|AT_[A-Z]+|LOCK|SPLICE|GRND|RWF|SFD|FICLONE|FIONREAD|FIONBIO)_[A-Z0-9_]+)\b')

defined = set()
for dp, dns, fns in os.walk(INC):
    for f in fns:
        if f.endswith('.h'):
            try:
                for m in re.finditer(r'^\s*#\s*define\s+(\w+)|\b(\w+)\s*=\s*[^=]|enum\s*\{([^}]*)\}',
                                     open(os.path.join(dp, f), errors='replace').read(), re.M):
                    if m.group(1):
                        defined.add(m.group(1))
                    elif m.group(2):
                        defined.add(m.group(2))
                    elif m.group(3):
                        for e in re.findall(r'\b([A-Z][A-Z0-9_]+)\b', m.group(3)):
                            defined.add(e)
            except OSError:
                pass

inc_use = collections.defaultdict(set)
sys_use = collections.defaultdict(set)
fn_use = collections.defaultdict(set)
path_use = collections.defaultdict(set)
const_use = collections.defaultdict(set)
INC_RE = re.compile(r'^\s*#\s*include\s*<([^>]+)>', re.M)
CALL_RE = re.compile(r'(?<![\w.>:])(?:::)?([a-z_][a-z0-9_]*)\s*\(')
PATH_RE = re.compile(r'"(/(?:proc|sys|dev)/[^"\s]*)"')
NR_RE = re.compile(r'\b(?:__NR_|SYS_)([a-z0-9_]+)\b')
COMMENT_RE = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)

for f in files:
    try:
        text = open(f, errors='replace').read()
    except OSError:
        continue
    rel = os.path.relpath(f, SRC)
    for h in INC_RE.findall(text):
        if h in CXX_STD or h in tree_headers or h in sys_headers:
            continue
        if h.endswith('.h') or '/' in h:
            inc_use[h].add(rel)
    code = COMMENT_RE.sub(' ', text)
    for n in NR_RE.findall(code):
        if n in nosys:
            sys_use[n].add(rel)
    for n in CALL_RE.findall(code):
        if n in glibc and n not in have:
            fn_use[n].add(rel)
    for p in PATH_RE.findall(code):
        path_use[re.sub(r'/\d+(/|$)', r'/N\1', p)].add(rel)
    for c in CONST_RE.findall(code):
        if c not in defined:
            const_use[c].add(rel)


def section(title, d, limit=400, min_files=1):
    out = ['', '=' * 78, title, '=' * 78]
    items = sorted(((len(v), k, sorted(v)) for k, v in d.items() if len(v) >= min_files), reverse=True)
    for n, k, v in items[:limit]:
        out.append('%5d  %-48s %s' % (n, k, ', '.join(v[:3])))
    out.append('(%d in all)' % len(items))
    return out


rep = ['Chromium sources scanned: %d files (a Linux build\'s, no tests)' % len(files)]
rep += section('System headers SIEOS has not', inc_use)
rep += section('System calls (by number) SIEOS answers ENOSYS', sys_use)
rep += section('C library functions (glibc\'s) SIEOS\'s libc has not', fn_use, min_files=1)
rep += section('/proc, /sys and /dev paths', path_use)
rep += section('Kernel interface constants SIEOS\'s headers do not define', const_use, limit=600)
open(os.path.join(GAP, 'report.txt'), 'w').write('\n'.join(rep) + '\n')
print('\n'.join(rep[:1]))
