#!/usr/bin/env python3
"""
nativebuild.py - build software on SIEOS itself, without interaction.

  nativebuild.py --iso ISO --root DIR --src TARBALL --script SCRIPT \
                 --out PATH_IN_GUEST=HOST_FILE ... [--mem 2G] [--smp 4] [--timeout 7200]

Some software can only be built where it runs: ksh93's build compiles and
runs hundreds of test programs.  This makes a disk from DIR (a SIEOS root with
the native toolchain), unpacks TARBALL in /root/build, and has /etc/rc run
SCRIPT there (as root, with PATH=/usr/gnu/bin:/usr/bin:/bin:/sbin), log to
/root/build.log and halt.  It boots the ISO in QEMU (KVM when /dev/kvm can be
used), waits for the halt, and copies the files named by --out out of the disk.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

RC_HOOK = '''
# nativebuild: run the build, then halt
if [ -f /root/autobuild ]; then
	PATH=/usr/gnu/bin:/usr/bin:/bin:/sbin; export PATH
	(cd /root/build && /bin/sh /root/autobuild) > /root/build.log 2>&1
	echo "nativebuild: exit $?" >> /root/build.log
	sync
	echo "nativebuild: done"
	/bin/halt
fi
'''

def debugfs(img, cmd):
    return subprocess.run(['debugfs', '-R', cmd, img], capture_output=True, text=True).stdout

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iso', required=True)
    ap.add_argument('--root', required=True)
    ap.add_argument('--src', required=True)
    ap.add_argument('--script', required=True)
    ap.add_argument('--out', action='append', default=[])
    ap.add_argument('--perms', default='build/all.perms')
    ap.add_argument('--mem', default='2G')
    ap.add_argument('--smp', default='4')
    ap.add_argument('--size', default='2048M')
    ap.add_argument('--timeout', type=int, default=7200)
    ap.add_argument('--keep', help='keep the build disk image here (for debugging)')
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix='nativebuild.', dir=os.path.dirname(os.path.abspath(a.iso)))
    try:
        root = os.path.join(work, 'root')
        shutil.copytree(a.root, root, symlinks=True)
        bdir = os.path.join(root, 'root', 'build')
        os.makedirs(bdir)
        subprocess.run(['tar', 'xf', a.src, '-C', bdir, '--strip-components=1'], check=True)
        shutil.copy(a.script, os.path.join(root, 'root', 'autobuild'))
        with open(os.path.join(root, 'etc', 'rc'), 'a') as f:
            f.write(RC_HOOK)
        img = os.path.join(work, 'disk.img')
        subprocess.run(['mkfs.ext4', '-q', '-F', '-b', '4096', '-L', 'sieos-root',
                        '-E', 'root_owner=0:0', '-d', root, img, a.size], check=True)
        perms = subprocess.run(['tools/mkperms.sh', root, a.perms], capture_output=True, text=True, check=True)
        pf = os.path.join(work, 'perms.debugfs')
        open(pf, 'w').write(perms.stdout)
        subprocess.run(['debugfs', '-w', '-f', pf, img], capture_output=True)
        shutil.rmtree(root)

        serial = os.path.join(work, 'serial.log')
        kvm = os.access('/dev/kvm', os.R_OK | os.W_OK)
        cmd = ['qemu-system-x86_64', '-smp', a.smp, '-m', a.mem, '-cdrom', a.iso, '-boot', 'd',
               '-drive', 'file=%s,format=raw,if=ide,index=0,media=disk' % img, '-nic', 'none',
               '-display', 'none', '-serial', 'file:' + serial, '-no-reboot']
        if kvm:
            cmd += ['-enable-kvm', '-cpu', 'host']
        else:
            print('nativebuild: no usable /dev/kvm, emulating (slow)', file=sys.stderr)
        print('nativebuild: building %s on SIEOS' % os.path.basename(a.src), file=sys.stderr)
        q = subprocess.Popen(cmd)
        t0 = time.time()
        done = False
        while time.time() - t0 < a.timeout:
            if q.poll() is not None:
                break
            if os.path.exists(serial) and 'nativebuild: done' in open(serial, errors='replace').read():
                done = True
                time.sleep(3)                          # let halt finish writing
                break
            time.sleep(5)
        if q.poll() is None:
            q.kill()
            q.wait()
        log = debugfs(img, 'cat /root/build.log')
        sys.stderr.write(log[-3000:])
        if 'nativebuild: exit 0' not in log:          # (SIEOS may power QEMU off before we see "done")
            sys.exit('nativebuild: the build failed or timed out')
        for o in a.out:
            guest, host = o.split('=', 1)
            os.makedirs(os.path.dirname(os.path.abspath(host)), exist_ok=True)
            subprocess.run(['debugfs', '-R', 'dump %s %s' % (guest, host), img], capture_output=True)
            if not os.path.exists(host) or os.path.getsize(host) == 0:
                sys.exit('nativebuild: %s was not made' % guest)
            os.chmod(host, 0o755)
    finally:
        if a.keep and os.path.exists(os.path.join(work, 'disk.img')):
            shutil.move(os.path.join(work, 'disk.img'), a.keep)
        shutil.rmtree(work, ignore_errors=True)

if __name__ == '__main__':
    main()
