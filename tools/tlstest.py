#!/usr/bin/env python3
"""Compare user/tls primitives with Python's hashlib/hmac/cryptography on random inputs."""
import hashlib, hmac, os, subprocess, sys
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.asymmetric import x25519, rsa, padding
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

BIN = sys.argv[1]
fails = 0
def run(*args):
    return subprocess.run([BIN, *args], capture_output=True, text=True, check=True).stdout.strip()
def check(name, got, want):
    global fails
    if got != want:
        fails += 1
        print(f"FAIL {name}: got {got[:80]} want {want[:80]}")
H = {'sha256': hashlib.sha256, 'sha384': hashlib.sha384, 'sha512': hashlib.sha512}
HC = {'sha256': hashes.SHA256, 'sha384': hashes.SHA384, 'sha512': hashes.SHA512}
for alg in H:
    for n in (0, 1, 55, 56, 63, 64, 111, 112, 127, 128, 129, 1000, 5000):
        m = os.urandom(n)
        check(f"{alg}/{n}", run('hash', alg, m.hex()), H[alg](m).hexdigest())
    for kn in (0, 16, 64, 200):
        k, m = os.urandom(kn), os.urandom(77)
        check(f"hmac-{alg}/{kn}", run('hmac', alg, k.hex(), m.hex()), hmac.new(k, m, H[alg]).hexdigest())
    for L in (16, 32, 42, 100):
        salt, ikm, info = os.urandom(13), os.urandom(22), os.urandom(10)
        want = HKDF(algorithm=HC[alg](), length=L, salt=salt, info=info).derive(ikm).hex()
        check(f"hkdf-{alg}/{L}", run('hkdf', alg, salt.hex(), ikm.hex(), info.hex(), str(L)), want)
for kl in (16, 32):
    for n in (0, 1, 15, 16, 17, 100, 1000):
        k, iv, aad, pt = os.urandom(kl), os.urandom(12), os.urandom(13), os.urandom(n)
        check(f"gcm{kl*8}/{n}", run('gcm', k.hex(), iv.hex(), aad.hex(), pt.hex()), AESGCM(k).encrypt(iv, pt, aad).hex())
for i in range(8):
    a, b = x25519.X25519PrivateKey.generate(), x25519.X25519PrivateKey.generate()
    from cryptography.hazmat.primitives import serialization as s
    araw = a.private_bytes(s.Encoding.Raw, s.PrivateFormat.Raw, s.NoEncryption())
    bpub = b.public_key().public_bytes(s.Encoding.Raw, s.PublicFormat.Raw)
    check(f"x25519/{i}", run('x25519', araw.hex(), bpub.hex()), a.exchange(b.public_key()).hex())
# RFC 7748 vector
check("x25519/rfc", run('x25519', 'a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4',
      'e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c'),
      'c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552')
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization as s2
for i in range(10):
    a, b = ec.generate_private_key(ec.SECP256R1()), ec.generate_private_key(ec.SECP256R1())
    d = a.private_numbers().private_value.to_bytes(32, 'big').hex()
    apub = a.public_key().public_bytes(s2.Encoding.X962, s2.PublicFormat.UncompressedPoint).hex()
    bpub = b.public_key().public_bytes(s2.Encoding.X962, s2.PublicFormat.UncompressedPoint).hex()
    check(f"p256pub/{i}", run('p256pub', d), apub)
    check(f"p256ecdh/{i}", run('p256ecdh', d, bpub), a.exchange(ec.ECDH(), b.public_key()).hex())
check("p256ecdh/offcurve", run('p256ecdh', '01'*32, '04' + '11'*64), 'FAIL')
for bits in (1024, 2048, 3072, 4096):
    key = rsa.generate_private_key(public_exponent=65537, key_size=bits)
    pn = key.public_key().public_numbers()
    n = pn.n.to_bytes((bits + 7) // 8, 'big').hex()
    e = pn.e.to_bytes(3, 'big').hex()
    for alg in ('sha256', 'sha384', 'sha512'):
        msg = os.urandom(50)
        dg = H[alg](msg).hexdigest()
        sig = key.sign(msg, padding.PKCS1v15(), HC[alg]())
        check(f"pkcs1-{bits}-{alg}", run('pkcs1', alg, n, e, dg, sig.hex()), 'ok')
        bad = bytearray(sig); bad[-1] ^= 1
        check(f"pkcs1-{bits}-{alg}-bad", run('pkcs1', alg, n, e, dg, bytes(bad).hex()), 'bad')
        if bits >= 2048 or alg != 'sha512':
            sig = key.sign(msg, padding.PSS(mgf=padding.MGF1(HC[alg]()), salt_length=H[alg]().digest_size), HC[alg]())
            check(f"pss-{bits}-{alg}", run('pss', alg, n, e, dg, sig.hex()), 'ok')
            check(f"pss-{bits}-{alg}-wrongdigest", run('pss', alg, n, e, H[alg](b'x').hexdigest(), sig.hex()), 'bad')
print("all primitive tests passed" if not fails else f"{fails} failures")
sys.exit(1 if fails else 0)
