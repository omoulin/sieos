#!/usr/bin/env python3
"""ref.py - An independent reference for the SIEOS engine: reads a GGUF file,
unpacks every quantized tensor with numpy, and runs the transformer in double
precision. Slow, simple, and written from the format descriptions, so its
results can judge the C engine's.

  ref.py model.gguf info                 tensor types
  ref.py model.gguf logits out.bin ids…  logits of the last token of ids (float32)

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
"""
import struct, sys
import numpy as np

def read_gguf(path):
    f = open(path, 'rb'); buf = f.read(); f.close()
    pos = 0
    def rd(fmt):
        nonlocal pos
        v = struct.unpack_from('<' + fmt, buf, pos); pos += struct.calcsize('<' + fmt); return v[0]
    def rstr():
        nonlocal pos
        n = rd('Q'); s = buf[pos:pos + n]; pos += n; return s.decode('utf-8', 'replace')
    scal = {0: 'B', 1: 'b', 2: 'H', 3: 'h', 4: 'I', 5: 'i', 6: 'f', 7: '?', 10: 'Q', 11: 'q', 12: 'd'}
    def rval(t):
        if t == 8: return rstr()
        if t == 9:
            et = rd('I'); n = rd('Q')
            return [rval(et) for _ in range(n)]
        return rd(scal[t])
    assert rd('I') == 0x46554747
    ver = rd('I'); nt = rd('Q'); nkv = rd('Q')
    kv = {}
    for _ in range(nkv):
        k = rstr(); t = rd('I'); kv[k] = rval(t)
    tens = {}
    for _ in range(nt):
        name = rstr(); nd = rd('I'); ne = [rd('Q') for _ in range(nd)]; t = rd('I'); off = rd('Q')
        tens[name] = (t, ne, off)
    align = kv.get('general.alignment', 32)
    data = (pos + align - 1) // align * align
    return buf, kv, tens, data

BS = {0: (1, 4), 1: (1, 2), 2: (32, 18), 3: (32, 20), 6: (32, 22), 7: (32, 24), 8: (32, 34), 12: (256, 144), 13: (256, 176), 14: (256, 210)}

def f16(a): return a.view(np.float16).astype(np.float64)

def scale_min_k4(sc):          # sc: (nb, 12) uint8 -> scales, mins (nb, 8)
    s = np.zeros((sc.shape[0], 8), np.int64); m = np.zeros_like(s)
    for j in range(8):
        if j < 4:
            s[:, j] = sc[:, j] & 63; m[:, j] = sc[:, j + 4] & 63
        else:
            s[:, j] = (sc[:, j + 4] & 0xF) | ((sc[:, j - 4] >> 6) << 4)
            m[:, j] = (sc[:, j + 4] >> 4) | ((sc[:, j] >> 6) << 4)
    return s, m

def dequant(buf, data, t, ne, off):
    n = int(np.prod(ne)); bs, bb = BS[t]
    raw = np.frombuffer(buf, np.uint8, n // bs * bb, data + off)
    if t == 0: return raw.view(np.float32).astype(np.float64)
    if t == 1: return f16(raw.view(np.uint16))
    b = raw.reshape(-1, bb)
    if t in (2, 3):
        d = f16(b[:, 0:2].copy().view(np.uint16))[:, 0]
        o = 4 if t == 3 else 2
        qs = b[:, o:o + 16]
        q = np.concatenate([qs & 15, qs >> 4], 1).astype(np.float64)
        if t == 2: return (d[:, None] * (q - 8)).ravel()
        m = f16(b[:, 2:4].copy().view(np.uint16))[:, 0]
        return (d[:, None] * q + m[:, None]).ravel()
    if t in (6, 7):
        d = f16(b[:, 0:2].copy().view(np.uint16))[:, 0]
        o = 4 if t == 7 else 2
        qh = b[:, o:o + 4].copy().view(np.uint32)[:, 0]
        qs = b[:, o + 4:o + 20]
        bits = ((qh[:, None] >> np.arange(32, dtype=np.uint32)) & 1).astype(np.int64)
        q = np.concatenate([qs & 15, qs >> 4], 1).astype(np.int64) | (bits << 4)
        if t == 6: return (d[:, None] * (q - 16)).ravel()
        m = f16(b[:, 2:4].copy().view(np.uint16))[:, 0]
        return (d[:, None] * q + m[:, None]).ravel()
    if t == 8:
        d = f16(b[:, 0:2].copy().view(np.uint16))[:, 0]
        return (d[:, None] * b[:, 2:34].view(np.int8).astype(np.float64)).ravel()
    if t in (12, 13):
        d = f16(b[:, 0:2].copy().view(np.uint16))[:, 0]; dmin = f16(b[:, 2:4].copy().view(np.uint16))[:, 0]
        s, m = scale_min_k4(b[:, 4:16])
        qh = b[:, 16:48] if t == 13 else None
        qs = b[:, 48:176] if t == 13 else b[:, 16:144]
        out = np.zeros((b.shape[0], 256))
        for j in range(8):                       # sub-block j: 32 values
            chunk = qs[:, (j // 2) * 32:(j // 2) * 32 + 32]
            q = (chunk & 15 if j % 2 == 0 else chunk >> 4).astype(np.int64)
            if t == 13: q = q | (((qh >> j) & 1).astype(np.int64) << 4)
            out[:, j * 32:(j + 1) * 32] = d[:, None] * s[:, j:j + 1] * q - dmin[:, None] * m[:, j:j + 1]
        return out.ravel()
    if t == 14:
        ql = b[:, 0:128].astype(np.int64); qh = b[:, 128:192].astype(np.int64)
        sc = b[:, 192:208].view(np.int8).astype(np.int64); d = f16(b[:, 208:210].copy().view(np.uint16))[:, 0]
        out = np.zeros((b.shape[0], 256))
        for h in range(2):
            L = ql[:, h * 64:h * 64 + 64]; Hh = qh[:, h * 32:h * 32 + 32]; S = sc[:, h * 8:h * 8 + 8]
            q = [(L[:, :32] & 15) | ((Hh & 3) << 4), (L[:, 32:] & 15) | (((Hh >> 2) & 3) << 4),
                 (L[:, :32] >> 4) | (((Hh >> 4) & 3) << 4), (L[:, 32:] >> 4) | (((Hh >> 6) & 3) << 4)]
            for k in range(4):
                for half in range(2):
                    idx = slice(h * 128 + k * 32 + half * 16, h * 128 + k * 32 + half * 16 + 16)
                    out[:, idx] = d[:, None] * S[:, 2 * k + half:2 * k + half + 1] * (q[k][:, half * 16:half * 16 + 16] - 32)
        return out.ravel()
    raise SystemExit('type %d not supported by the reference' % t)

def main():
    path, cmd = sys.argv[1], sys.argv[2]
    buf, kv, tens, data = read_gguf(path)
    if cmd == 'info':
        types = {}
        for n, (t, ne, off) in tens.items(): types[t] = types.get(t, 0) + 1
        print('types', types); return
    arch = kv['general.architecture']
    E = kv[arch + '.embedding_length']; L = kv[arch + '.block_count']; H = kv[arch + '.attention.head_count']
    KVH = kv.get(arch + '.attention.head_count_kv', H); hd = E // H
    eps = kv.get(arch + '.attention.layer_norm_rms_epsilon', 1e-5); theta = kv.get(arch + '.rope.freq_base', 10000.0)
    def W(name):
        t, ne, off = tens[name]
        return dequant(buf, data, t, ne, off).reshape(list(reversed(ne)))
    out_path = sys.argv[3]; ids = [int(x) for x in sys.argv[4:]]
    emb = W('token_embd.weight')
    out = W('output.weight') if 'output.weight' in tens else emb
    x = emb[ids]                                   # (T, E)
    T = len(ids)
    inv = theta ** (-np.arange(0, hd, 2) / hd)
    ang = np.arange(T)[:, None] * inv[None, :]
    cos, sin = np.cos(ang), np.sin(ang)
    neox = arch == 'qwen2'
    def rope(v, nh):                               # v: (T, nh*hd)
        v = v.reshape(T, nh, hd).copy()
        if neox: a, b = v[..., :hd // 2], v[..., hd // 2:]
        else: a, b = v[..., 0::2], v[..., 1::2]
        a2 = a * cos[:, None] - b * sin[:, None]; b2 = a * sin[:, None] + b * cos[:, None]
        if neox: v[..., :hd // 2], v[..., hd // 2:] = a2, b2
        else: v[..., 0::2], v[..., 1::2] = a2, b2
        return v.reshape(T, nh * hd)
    def norm(v, w): return v / np.sqrt((v * v).mean(-1, keepdims=True) + eps) * w
    mask = np.triu(np.full((T, T), -np.inf), 1)
    for l in range(L):
        p = 'blk.%d.' % l
        h = norm(x, W(p + 'attn_norm.weight'))
        q = h @ W(p + 'attn_q.weight').T; k = h @ W(p + 'attn_k.weight').T; v = h @ W(p + 'attn_v.weight').T
        if p + 'attn_q.bias' in tens:
            q += W(p + 'attn_q.bias'); k += W(p + 'attn_k.bias'); v += W(p + 'attn_v.bias')
        q = rope(q, H).reshape(T, H, hd); k = rope(k, KVH).reshape(T, KVH, hd); v = v.reshape(T, KVH, hd)
        o = np.zeros((T, H, hd))
        for hh in range(H):
            kh = hh // (H // KVH)
            s = q[:, hh] @ k[:, kh].T / np.sqrt(hd) + mask
            s = np.exp(s - s.max(-1, keepdims=True)); s /= s.sum(-1, keepdims=True)
            o[:, hh] = s @ v[:, kh]
        x = x + o.reshape(T, E) @ W(p + 'attn_output.weight').T
        h = norm(x, W(p + 'ffn_norm.weight'))
        g = h @ W(p + 'ffn_gate.weight').T; u = h @ W(p + 'ffn_up.weight').T
        x = x + (g / (1 + np.exp(-g)) * u) @ W(p + 'ffn_down.weight').T
    lg = norm(x[-1:], W('output_norm.weight')) @ out.T
    lg[0].astype(np.float32).tofile(out_path)

main()
