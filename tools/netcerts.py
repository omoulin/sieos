#!/usr/bin/env python3
"""Make /etc/ssl/roots: the trusted root certificates, one DER after another
(the binary form; user/lib/x509.c reads it), from a PEM bundle such as the
Mozilla list published at https://curl.se/ca/cacert.pem.

Usage: netcerts.py cacert.pem rootfs/etc/ssl/roots

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
"""
import base64, re, sys

pem = open(sys.argv[1]).read()
ders = [base64.b64decode("".join(b.split())) for b in
        re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", pem, re.S)]
open(sys.argv[2], "wb").write(b"".join(ders))
print(f"{len(ders)} root certificates, {sum(map(len, ders))} bytes")
