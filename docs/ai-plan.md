# The SIEOS AI model: plan

Status: partly done (marked below). The assistant as built: [sia.md](sia.md),
the engine: [llm.md](llm.md). Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

## Goal

A language model at the core of SIEOS, answering the desktop, the shell and
applications. It can run **locally** (on the NVIDIA GPU, or on the CPU
when there is none) or use an **online endpoint**. Hard budget: **4 GB of
memory for the model** (weights, cache and buffers together), on the
target laptop (32 GB RAM, RTX 5060 Laptop GPU with 8 GB, Raptor Lake HX).

## What fits in 4 GB

| Part | Size |
|---|---|
| Weights of a 3–4 billion parameter model, 4-bit quantized (~4.5 bits per weight) | ~1.8–2.4 GB |
| Attention cache (KV), 8-bit, about 50–60 KB per token for a 3B model with grouped heads | ~0.5 GB for 8,000 tokens |
| Work buffers, tokenizer, server | ~0.2 GB |
| **Total** | **~2.5–3.1 GB**, leaving room for 16,000 tokens of context |

An 8B model would need ~4.5 GB at 4 bits before any cache: over budget,
or a 3-bit version with a visible quality loss. **Target: a 3–4B model,
4-bit.** Choose one with a permissive licence when the time comes.

Expected speed (token generation is limited by memory bandwidth: every
token reads all the weights once):

| Backend | Bandwidth | Tokens per second, 2 GB of weights |
|---|---|---|
| RTX 5060 Laptop (GDDR7) | a few hundred GB/s | ~60–120 |
| CPU, DDR5 dual channel | ~70–90 GB/s | ~15–30 |

Reading a long prompt is far faster on the GPU (matrix-matrix work).

## Architecture

```
  desktop · shell · apps
        │  messages: SIA_OPEN, SIA_ASK, SIA_NEXT (the answer in pieces), SIA_STOP, SIA_INFO
        ▼
  sia server  ── limits: memory quota 4 GB, rights given by init/rooms
    │  engine (our C code): GGUF loader, tokenizer (BPE), transformer, sampling,
    │                       chat template, sessions with their KV cache
    ├── backend CPU    : AVX2 + AVX-VNNI kernels, threads on the P-cores
    ├── backend NVIDIA : nvgpu driver server (GSP firmware, memory, queues)
    │                    + ~15 GPU kernels written in PTX, compiled on the
    │                      development machine for Blackwell (sm_120)
    └── backend online : HTTPS client to an endpoint (needs network + TLS)
```

- **One protocol, any backend**: clients never know which one answered.
- **Memory**: with the GPU, the weights are read from SieFS straight into
  GPU memory in chunks, so RAM use stays small (no full copy kept in RAM).
  With the CPU, the weights stay in RAM (2 GB of the 32).
- **Quota**: the kernel gets a per-process memory limit (pages and DMA),
  set by init for the sia server: 4.5 GiB (siad), never more.
- **Rights**: the sia server is a server like the others; the kernel limits
  what it can read and whether it may use the network (online endpoint).
- **Model file**: GGUF (a documented format that carries the tokenizer),
  stored in `/models`, verified by its checksum at load.

## The engine (our C code)

1. GGUF reader; quantized formats: one 4-bit (k-quant style) and 8-bit.
2. BPE tokenizer from the GGUF metadata.
3. Transformer forward pass: RMSNorm, rotary embeddings, grouped-query
   attention with the KV cache, SwiGLU feed-forward, logits.
4. Sampling: temperature, top-k / top-p, repetition penalty; stop tokens.
5. Sessions: several clients, each with its own KV cache inside the quota.
6. Correctness test: compare logits with a reference run on the
   development machine for a few prompts.

GPU kernels needed (the same list for CPU code): quantized matrix-vector
and matrix-matrix product, RMSNorm, RoPE, attention (with softmax), SwiGLU,
add, copy/convert, argmax/top-k.

## What SIEOS needs first

| Need | For | Status |
|---|---|---|
| Floating point and vector registers saved on task switches (lazy) | every backend | done (x86-64, AArch64) |
| Per-thread storage | the engine | done; a fuller C library: not yet |
| Per-process memory quota in the kernel | the 4 GB limit | done (siad: 4.5 GiB) |
| SIEOS on the laptop: UEFI boot, NVMe (read), USB/touchpad | running there | not yet |
| IOMMU (VT-d): a device reaches only its own memory | the NVIDIA driver | not yet |
| Network: virtio (QEMU) done; Realtek driver for the laptop, TCP/IP, DNS, TLS | the online backend | done in QEMU; Realtek not yet |
| Scheduler aware of P-cores and E-cores | CPU inference speed | not yet |

## Order

1. Engine + CPU backend, in QEMU (model in `/models` on the SieFS disk). **Done** (also NEON on AArch64).
2. The `sia` protocol and the desktop's assistant (the prototypes' design). **Done.**
3. SIEOS on the laptop (USB boot into RAM; then NVMe). Not yet.
4. NVIDIA backend: develop with the GPU passed to a SIEOS virtual machine,
   then on the laptop itself. Not yet.
5. Online backend, once networking exists. **Done** (OpenAI-compatible, HTTP and HTTPS).
