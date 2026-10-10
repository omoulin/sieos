#!/usr/bin/env python3
"""Make signature test vectors with the host's openssl (only a test tool):
ECDSA on P-256 and P-384, RSA PKCS #1 v1.5 and PSS, written one per line
for tests/net/crypto.c. Usage: gen.py OUTFILE

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import os, re, subprocess, sys, tempfile

def run(*a, inp=None):
    return subprocess.run(a, input=inp, capture_output=True, check=True).stdout

out = []
with tempfile.TemporaryDirectory() as d:
    k, msgf, sig = os.path.join(d, "k.pem"), os.path.join(d, "m"), os.path.join(d, "s")
    for i in range(6):
        msg = os.urandom(40 + i)
        open(msgf, "wb").write(msg)
        for curve, cname, hlen, h in (("prime256v1", "P256", 65, "sha256"), ("secp384r1", "P384", 97, "sha384"),
                                      ("prime256v1", "P256", 65, "sha384")):
            run("openssl", "ecparam", "-name", curve, "-genkey", "-noout", "-out", k)
            pub = run("openssl", "ec", "-in", k, "-pubout", "-outform", "DER")[-hlen:]
            run("openssl", "dgst", "-" + h, "-sign", k, "-out", sig, msgf)
            ints = re.findall(rb"INTEGER\s+:([0-9A-F]+)", run("openssl", "asn1parse", "-inform", "DER", "-in", sig))
            out.append(f"ecdsa {cname} {h} {pub.hex()} {msg.hex()} {ints[0].decode()} {ints[1].decode()}")
        bits = (2048, 3072, 4096)[i % 3]
        run("openssl", "genrsa", "-out", k, str(bits))
        n = run("openssl", "rsa", "-in", k, "-noout", "-modulus").decode().strip().split("=")[1]
        for h in ("sha256", "sha384", "sha512"):
            run("openssl", "dgst", "-" + h, "-sign", k, "-out", sig, msgf)
            out.append(f"rsa pkcs1 {h} {n} 010001 {msg.hex()} {open(sig, 'rb').read().hex()}")
            run("openssl", "dgst", "-" + h, "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:digest",
                "-sign", k, "-out", sig, msgf)
            out.append(f"rsa pss {h} {n} 010001 {msg.hex()} {open(sig, 'rb').read().hex()}")
open(sys.argv[1], "w").write("\n".join(out) + "\n")
