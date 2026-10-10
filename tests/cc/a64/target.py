"""Where the sicc fuzzers build and run their programs: on the development
machine (x86-64, linked by the host), or, when QEMU names the user-mode
emulator, for AArch64: sicc --target=aarch64, linked by sicc with the test C
library of this directory, run under the emulator. The reference is then
built by the host compiler with SIEOS's AArch64 types (char unsigned, long
double = double) and the same printf (fmt.c).
Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only"""
import os, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))

class Target:
    def __init__(self, sicc, out):
        self.sicc, self.out, self.qemu = sicc, out, os.environ.get("QEMU")
        if not self.qemu: return
        rt = os.path.join(out, "rt")
        os.makedirs(rt, exist_ok=True)
        a = [sicc, "--target=aarch64"]
        for cmd in (a + ["-c", "-o", f"{rt}/crt0.o", f"{HERE}/crt0.S"],
                    a + ["-O2", "-c", "-o", f"{rt}/libc.o", f"{HERE}/libc.c"],
                    a + ["-O2", "-c", "-o", f"{rt}/fmt.o", f"{HERE}/fmt.c"],
                    a + ["-O2", "-c", "-o", f"{rt}/rt.o", f"{HERE}/../../../cc/rt/rt.c"],
                    ["gcc", "-O2", "-fno-builtin", "-mlong-double-64", "-c", "-o", f"{rt}/fmt_host.o", f"{HERE}/fmt.c"],
                    ["gcc", "-O2", "-c", "-o", f"{rt}/host.o", f"{HERE}/host.c"]):
            subprocess.run(cmd, check=True)
        self.rt = rt

    def ref(self, src, exe, flags=()):
        """the host compiler's program (raises on failure)"""
        extra = ["-fno-builtin", "-mlong-double-64", "-funsigned-char", f"{self.rt}/fmt_host.o", f"{self.rt}/host.o"] if self.qemu else []
        subprocess.run(["gcc", "-w", *flags, "-o", exe, src, *extra, "-lm"], check=True)

    def build(self, src, exe, opts=()):
        """sicc's program: (ok, error text)"""
        obj = exe + ".o"
        if self.qemu:
            r = subprocess.run([self.sicc, "--target=aarch64", *opts, "-c", "-o", obj, src], capture_output=True, text=True)
            if r.returncode: return False, r.stderr
            rt = self.rt
            r = subprocess.run([self.sicc, "--target=aarch64", "-nostdlib", "-T", f"{HERE}/prog.ld", "-o", exe, f"{rt}/crt0.o", obj,
                                f"{rt}/libc.o", f"{rt}/fmt.o", f"{rt}/rt.o"], capture_output=True, text=True)
            if r.returncode: return False, r.stderr
            os.chmod(exe, 0o755)
            return True, ""
        r = subprocess.run([self.sicc, *opts, "-c", "-o", obj, src], capture_output=True, text=True)
        if r.returncode: return False, r.stderr
        r = subprocess.run(["gcc", "-no-pie", "-o", exe, obj], capture_output=True, text=True)
        return r.returncode == 0, r.stderr

    def run(self, exe, sicc_built=True):
        """the program's output"""
        cmd = [self.qemu, exe] if self.qemu and sicc_built else [exe]
        return subprocess.run(cmd, capture_output=True, text=True).stdout
