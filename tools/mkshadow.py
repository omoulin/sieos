#!/usr/bin/env python3
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
"""mkshadow.py user:password... - print /etc/shadow lines using the SIEOS
password hash (must match user/lib/crypt.c):
    h = sha256(salt "$" password); repeat 5000x: h = sha256(h + password + salt)
    -> "$5a$<salt>$<hex>"
"""
import hashlib
import secrets
import string
import sys

ALPHABET = string.ascii_letters + string.digits + './'

def crypt(password, salt):
    h = hashlib.sha256(f'{salt}${password}'.encode()).digest()
    for _ in range(5000):
        h = hashlib.sha256(h + password.encode() + salt.encode()).digest()
    return f'$5a${salt}${h.hex()}'

for spec in sys.argv[1:]:
    user, password = spec.split(':', 1)
    if password in ('!', '*', ''):
        print(f'{user}:{password or "!"}:::::::')
        continue
    salt = ''.join(secrets.choice(ALPHABET) for _ in range(12))
    print(f'{user}:{crypt(password, salt)}:::::::')
