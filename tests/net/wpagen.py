# An access point's side of a WPA2-PSK 4-way handshake, written apart from
# user/wifi/wpa.c (hashlib + openssl AES-ECB for the key wrap), to make test vectors.
import hashlib, hmac, struct, subprocess

def aes(k, b):
    return subprocess.run(["openssl", "enc", "-aes-128-ecb", "-nopad", "-K", k.hex()], input=b, capture_output=True, check=True).stdout

def wrap(kek, p):
    n = len(p) // 8; a = b"\xa6" * 8; r = [p[8*i:8*i+8] for i in range(n)]
    for j in range(6):
        for i in range(n):
            b = aes(kek, a + r[i]); t = n * j + i + 1
            a = (int.from_bytes(b[:8], "big") ^ t).to_bytes(8, "big"); r[i] = b[8:]
    return a + b"".join(r)

pmk = hashlib.pbkdf2_hmac("sha1", b"correct horse", b"SIEOS-test", 4096, 32)
aa = bytes.fromhex("020000000001"); spa = bytes.fromhex("021122334455")
anonce = bytes(range(32)); snonce = bytes(range(32, 64))
d = min(aa, spa) + max(aa, spa) + min(anonce, snonce) + max(anonce, snonce)
ptk = b"".join(hmac.new(pmk, b"Pairwise key expansion\0" + d + bytes([i]), "sha1").digest() for i in range(3))[:48]
kck, kek = ptk[:16], ptk[16:32]

def key(info, replay, nonce, data, kck=None):
    body = struct.pack(">BHH", 2, info, 0) + replay + nonce + bytes(16) + bytes(8) + bytes(8) + bytes(16) + struct.pack(">H", len(data)) + data
    f = bytearray(struct.pack(">BBH", 2, 3, len(body)) + body)
    if kck:
        f[81:97] = hmac.new(kck, bytes(f), "sha1").digest()[:16]
    return bytes(f)

rsn_ap = bytes.fromhex("30140100000fac040100000fac040100000fac020000")
gtk = bytes(range(0xa0, 0xb0))
kd = rsn_ap + bytes([0xdd, 22]) + bytes.fromhex("000fac01") + bytes([1, 0]) + gtk
kd += b"\xdd" + bytes((-len(kd) - 1) % 8)          # padding to 8 bytes
m1 = key(0x008a, (1).to_bytes(8, "big"), anonce, b"")
m3 = key(0x13ca, (2).to_bytes(8, "big"), anonce, wrap(kek, kd), kck)
m2 = key(0x010a, (1).to_bytes(8, "big"), snonce, rsn_ap, kck)
m4 = key(0x030a, (2).to_bytes(8, "big"), bytes(32), b"", kck)
gkd = bytes([0xdd, 22]) + bytes.fromhex("000fac01") + bytes([2, 0]) + gtk[::-1]
gkd += b"\xdd" + bytes((-len(gkd) - 1) % 8)
g1 = key(0x1382, (3).to_bytes(8, "big"), bytes(32), wrap(kek, gkd), kck)
g2 = key(0x0302, (3).to_bytes(8, "big"), bytes(32), b"", kck)
for n, v in [("g1", g1), ("g2", g2), ("pmk", pmk), ("ptk", ptk), ("m1", m1), ("m2", m2), ("m3", m3), ("m4", m4), ("wrap", wrap(bytes(range(16)), bytes.fromhex("00112233445566778899aabbccddeeff")))]:
    print(n, v.hex())
