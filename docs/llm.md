# The SIEOS language-model engine (`llm/`)

SIEOS's own engine for running transformer language models on the CPU,
written from scratch in C: it reads GGUF model files, runs Llama-family
models (Llama, SmolLM2, Mistral, Qwen2), and holds conversations. It is the
CPU backend of the future **sia** assistant (docs/ai-plan.md).

Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only

```sh
make llm                  # host tools: build/llm/llm-run, build/llm/llm-info
make llm-test             # kernels, fuzzing, reference comparison, tokenizer, chats
make llm-neon-test        # AArch64 NEON kernels vs plain C (emulated)
make llm-bench            # speed of every model in models/
make llm-freestanding     # the engine built as SIEOS will: no C library at all

build/llm/llm-run -m models/smollm2-1.7b-instruct-q4_k_m.gguf -t 8 -p "Why is the sky blue?"
build/llm/llm-run -m models/smollm2-1.7b-instruct-q4_k_m.gguf -t 8 -i      # chat
```

## Files

| File | What it does |
|---|---|
| `llm/llm.h` | the interface: open a model, sessions, ask, next piece of the answer |
| `llm/gguf.c` | reads and checks a GGUF file: metadata, tensor list, data |
| `llm/quant.c` | number formats, activation quantization, dot-product kernels (C and AVX2), kernel choice |
| `llm/neon.c` | AArch64 NEON kernels (Raspberry Pi 4: no dot-product instructions) |
| `llm/neon_dot.c` | the same with `sdot` (Raspberry Pi 5), built with `-march=armv8.2-a+dotprod` |
| `llm/model.c` | the forward pass, threading of the matrix products |
| `llm/tok.c` | tokenizers: byte-level BPE and SentencePiece |
| `llm/chat.c` | chat templates, sessions and their cache, sampling |
| `llm/math.c` | exp, log, sin/cos, sqrt without a C library |
| `tools/llm/` | host tools (`llm-run`, `llm-info`) and their environment (`host.c`, ISO C) |
| `tests/llm/` | tests: `kernels.c`, `fuzz.c`, `ref.py`, `compare.py`, `tokcheck.py`, `run.sh`, `bench.sh`; AArch64: `neon_test.c`, `neon.sh`, `a64run.c` |

The engine uses no C library: only `memcpy`, `memset`, `memmove`, `memcmp`,
`strlen`, `strcmp`, `strchr` and `strlcpy` (from `lib/string.c`). Everything
else comes from its caller in an `llm_env_t`: memory, reading the model
file, running work on threads, a clock. Freestanding, it is 43 KB of code.

## How a token is computed

```
 token id ──► embedding row (dequantized) = x                    [n_embd floats]
 for each of the n_layer layers:
     h = RMSNorm(x) · attn_norm
     q, k, v = Wq·h, Wk·h, Wv·h                ← one threaded job for the three
     rotate q, k by position (RoPE)            ← pairs (2i,2i+1); Qwen2: (i, i+d/2)
     k, v ──► the session's cache [layer][position]  (half precision)
     per head: scores = q·k_p/√d for every cached p, softmax, sum of p·v_p
     x += Wo · (attention output)
     h = RMSNorm(x) · ffn_norm
     g, u = Wgate·h, Wup·h                     ← one threaded job for the two
     x += Wdown · (silu(g) ⊙ u)
 logits = Wout · RMSNorm(x)                   (Wout = the embedding if tied)
```

Grouped-query attention: several query heads share one key/value head
(`n_head / n_kv_head`), which keeps the cache small.

**Reading a prompt**: up to 32 tokens go through each layer together, so
each weight row, once loaded from memory, serves all of them.

## Number formats

Weights are stored in blocks that share a scale. The engine reads:

| Format | Values per block | Bytes | Value |
|---|---|---|---|
| F32, F16, BF16 | 1 | 4, 2, 2 | as is |
| Q8_0 | 32 | 34 | d·q, q 8 bits |
| Q4_0 / Q4_1 | 32 | 18 / 20 | d·(q−8) / d·q+m, q 4 bits |
| Q5_0 / Q5_1 | 32 | 22 / 24 | d·(q−16) / d·q+m, q 5 bits |
| Q4_K | 256 | 144 | d·s·q − dmin·m; 8 sub-blocks of 32, 6-bit scale s and min m each |
| Q5_K | 256 | 176 | the same with 5-bit q |
| Q6_K | 256 | 210 | d·s·(q−32); 16 sub-blocks of 16, 8-bit scale s each |

To multiply a matrix by a vector, the vector is quantized to 8 bits too, in
blocks of the same size (32, with the block sum; or 256, with 16 sums of 16
values for the K formats). Each block is then an integer dot product (AVX2:
`maddubs`/`madd`) times two scales. A plain C kernel exists for each format;
the AVX2 ones are chosen at load when the processor has AVX2, FMA and F16C
(AVX-VNNI is used for Q8_0, Q4_0, Q5_0).

*Lesson learned*: inside AVX code, never call a plain SSE function (even a
tiny one, like a half-precision conversion): each switch between the two
instruction encodings costs dozens of cycles. The first version did it once
per block and ran 30× slower than it should.

## Threads

A matrix product is split into chunks of rows (about 16 KiB of weights per
chunk); threads take chunks as they go, so fast and slow cores (this
laptop's performance and efficiency cores) finish together. Attention is
split by heads. The engine only asks its environment to "run this function
on N threads and wait"; on SIEOS that will be the server's own threads.

## Memory

| Part | Size |
|---|---|
| Weights | the tensors, read once into one block of memory |
| Cache per session | 2 · layers · context · kv_heads · head_dim · 2 bytes (half precision) |
| Scratch | buffers for 32 tokens, the attention scores, the quantized vectors |

The engine refuses to open a model, or to start a session, beyond its budget
(default 4 GiB): `llm_open` and `llm_session_new` return `LLM_EBUDGET`. The
default context is 4,096 tokens (the model's own if smaller).

| Model | Weights | Cache per session (4,096 tokens, reserved) | Measured peak RAM (`llm-run`, short chat) |
|---|---|---|---|
| SmolLM2-135M Q4_K_M | 101 MiB | 90 MiB | 194 MiB |
| SmolLM2-135M Q8_0 | 136 MiB | 90 MiB | 232 MiB |
| SmolLM2-360M Q8_0 | 367 MiB | 160 MiB | 501 MiB |
| SmolLM2-1.7B Q4_K_M (default) | 1,005 MiB | 768 MiB | 1,110 MiB |

The cache is reserved for the whole context but only the part in use is
touched, so short conversations stay well below the reserved size; the
budget check counts the full reservation.

## Tokenizers and chats

- **Byte-level BPE** (SmolLM2, Llama 3, Qwen2): the text is cut into pieces
  (`'s 't …`, a word with its leading space, a number, punctuation, spaces;
  SmolLM2 also makes every digit a piece of its own), bytes are mapped to
  printable characters, then the pair with the best rank in the file's merge
  list is merged until none applies. Outside ASCII, letters are recognised
  by excluding the common punctuation and symbol blocks: an approximation
  (exact for the test texts, including Greek, Cyrillic, CJK and emoji).
- **SentencePiece** (Llama 2, Mistral): spaces become U+2581, the pair whose
  merge scores best is merged first, missing characters become byte tokens.
  (Implemented; not yet tested against a real model of that family.)
- **Chat formats**: ChatML (`<|im_start|>`, SmolLM2, Qwen2), Llama 3
  (`<|start_header_id|>`), and `[INST]`, chosen from the file's template. The
  fixed parts are read as special tokens; **the user's text is always read
  as plain text**, so it cannot inject control tokens.
- **Sessions** keep their cache: a new turn reads only its own new tokens.
  `llm_next` reads exactly one token per call (the one chosen before), so a
  session's scores are always its own, even when several sessions alternate.
  When the context is full, the session starts again with the current turn.
- **Sampling**: greedy, temperature, top-k, top-p, min-p, repetition penalty,
  a seeded generator. Answers are returned in pieces, never cutting a UTF-8
  character in two.

## Tests (`make llm-test`)

1. **Kernels** (`kernels.c`): every format, random blocks; the plain C kernel,
   the AVX2 kernel and a float computation agree to ~1e-8; half-precision
   conversions exact both ways (and identical to the processor's own on
   22 million floats).
2. **Fuzzing** (`fuzz.c`, with the address and undefined-behaviour checkers):
   200 damaged or truncated model files are refused or, if the damage is
   harmless, run; never a crash or a bad memory access.
3. **Reference** (`ref.py`, numpy, double precision, written independently
   from the format descriptions) on SmolLM2-135M Q8_0, 135M Q4_K_M (mixes
   Q8_0, Q5_0, Q4_K, Q6_K) and 360M Q8_0:
   - exact mode (every product in floats): within 0.1–0.2 % of the reference;
   - fast kernels (8-bit activations): correlation ≥ 0.9988, same top token;
   - greedy decoding, 8 tokens: identical.
   On the 1.7B model (checked by hand: the reference needs ~30 s per step),
   exact mode picks the reference's token even in a three-way near-tie.
4. **Tokenizer** vs the model's published tokenizer (the `tokenizers`
   library, used only as a test reference): 22/22 texts identical (spaces,
   newlines, tabs, numbers, contractions, code, URLs, accents, Greek,
   Cyrillic, CJK, emoji, a 300-character word).
5. **Chats**: the model remembers across turns; a model over the budget is
   refused; a full context starts again and keeps answering.

The reference checks need numpy; `run.sh` uses a private Python in
`build/llm-conda` (installed from the Miniforge installer, `pip install numpy
tokenizers`); without it they are skipped.

## Speed (development machine: Raptor Lake HX, 8 P + 12 E cores, DDR5)

Measured while other builds were running (load ~15), so ±10 %. Memory read
bandwidth of this machine: 24 GB/s on one thread, 55 GB/s on 8, 79 GB/s on 16.

See `build/llm/bench.txt` after `make llm-bench`; summary:

| Model | Threads | Prompt tok/s | Generation tok/s |
|---|---|---|---|
| SmolLM2-135M Q8_0 | 1 / 16 | 206 / 580–820 | 97 / 195–280 |
| SmolLM2-135M Q4_K_M | 1 / 16 | 148 / 713 | 99 / 300 |
| SmolLM2-360M Q8_0 | 1 / 16 | 79 / 273 | 40 / 97 |
| SmolLM2-1.7B Q4_K_M (default) | 1 | 15 | 11.6 |
| SmolLM2-1.7B Q4_K_M | 8 | 110 | 37–45 |
| SmolLM2-1.7B Q4_K_M | 16 | 100–153 | 37–55 |
| SmolLM2-1.7B Q4_K_M, plain C kernels | 16 | 4.4 | 3.2 |

Kernels made the difference: plain C 3.2 tok/s, first AVX2 version 21,
F16C conversions inside AVX code (no SSE calls) and vector-unpacked scales
38, chunked work sharing across fast and slow cores up to 55. Reading a
prompt: the several-vectors kernels doubled it (70 → 153 tok/s).

Generation reads all the weights once per token: 43–55 tokens/s is
~46–59 GB/s, 60–75 % of what 16 threads can read; the rest goes into the
shorter matrix products (thread hand-off costs more than the work),
attention and the serial steps between products.

## AArch64: Raspberry Pi 4 and 5

`llm/neon.c` holds the NEON kernels, written with `<arm_neon.h>` and built
by sicc only (docs/cc.md, "Vectors and NEON"). It is compiled twice:

- **plain NEON** (`neon.o`, Pi 4's Cortex-A72, ARMv8.0): an 8-bit dot product
  of 16 pairs is `smull` + `smlal2` (16-bit products) and `sadalp` (pairwise
  add into 32-bit sums);
- **`sdot`** (`neon_dot.o`, Pi 5's Cortex-A76, ARMv8.2): one `sdot` does the
  same 16 products and sums. `neon_dot.c` is `neon.c` with `NEON_SDOT`.

Kernels: Q8_0, Q4_0, Q4_K, Q6_K, F16, F32 (one vector), Q4_K and Q6_K for
several vectors at once (prompts), and the attention's half-precision
helpers (dot, `y += a·x`, float → half with `fcvtn`). Q5_0, Q4_1, Q5_1,
Q5_K and BF16 use the plain C kernels.

**Choice at run time** (`pick_kernels`, `llm/quant.c`): `LLM_K_DOT` when
`llm_env_t.cpu_dotprod` says the processor has the dot-product instructions,
else `LLM_K_NEON`; `LLM_K_SCALAR` forces plain C. A program cannot read the
processor's feature registers itself (`ID_AA64ISAR0_EL1` is privileged), so
the kernel tells it: `SYS_INFO`'s `hwcap` has `HWCAP_DOTPROD` when the
processor has the dot product, and siad sets `cpu_dotprod` from it. Checked
emulated: `-cpu cortex-a72` (the Pi 4's) gives "neon kernels", `-cpu max`
"neon+dot kernels" (`make ARCH=arm64 sia-test CPU64=max`). Both object files are in the arm64
build of siad. `llm-run --kernel neon|dot|scalar` forces one.

**Tests** (`make llm-neon-test`, emulated with `qemu-aarch64 -cpu max`):
every format, both variants vs the plain C kernels on random blocks, 40
rounds each — F32/F16 within 3e-8, Q8_0 6e-9, Q4_0 7e-10, Q4_K and Q6_K
exact (integer sums), several-vector kernels identical to one-at-a-time;
the half-precision helpers identical. End to end on SmolLM2-135M
(`tests/llm/a64run.c`, the engine for AArch64 under the emulator): greedy
decoding of 8 tokens identical for the host's plain C, AArch64 plain C,
NEON and NEON+sdot; logits vs the host's plain C: AArch64 plain C
identical, NEON correlation 0.99919 (Q8_0) / 0.99920 (Q4_K_M), sdot
0.99936 / 0.99917, same top token (8-bit activations, as on x86-64).

**Speed under emulation** (135M Q8_0, one thread, the development machine
emulating AArch64: only the ratios mean something, and the emulator runs
NEON slowly):

| Kernels | Prompt tok/s | Generation tok/s |
|---|---|---|
| plain C | 1.30 | 0.99 |
| NEON | 1.63 | 1.24 |
| NEON + sdot | 2.39 | 1.80 |

**Estimates for the boards.** Generation reads every weight once per token,
so its ceiling is memory bandwidth / model size. Assumed read bandwidth
with 4 cores: Pi 4 (LPDDR4-3200) 4–5 GB/s, Pi 5 (LPDDR4X-4267) 10–17 GB/s
(published measurements, not ours: no board measured yet).

| Model (bytes read per token) | Pi 4 ceiling | Pi 4 expected | Pi 5 ceiling | Pi 5 expected |
|---|---|---|---|---|
| 135M Q4_K_M (105 MB) | 38–47 | 15–25 | 95–160 | 40–70 |
| 135M Q8_0 (145 MB) | 28–34 | 15–22 | 70–117 | 35–60 |
| 360M Q8_0 (386 MB) | 10–13 | 6–9 | 26–44 | 15–25 |
| 1.7B Q4_K_M (1.06 GB) | 3.8–4.7 | 2–3 | 9–16 | 5–9 |

"Expected" assumes reaching 50–70 % of the ceiling, as on x86-64 (60–75 %),
a bit less because the Pi 4 without `sdot` is closer to compute-bound
(4 × A72 at 1.8 GHz) and sicc keeps vectors in memory (a load and a store
around each NEON instruction). Prompts are compute-bound: expect roughly
2–4× the generation rate with the several-vectors kernels. The 1.7B model
fits the Pi's RAM only on 4 GB boards and up (1.06 GB of weights + cache).

## What SIEOS needs to run it

All done; the engine runs in siad (`user/sia`, [sia.md](sia.md)):
37-55 tokens/s for the 1.7B model inside SIEOS with 4-8 CPUs.

- **Floating point and vector registers saved on every thread switch**
  (FXSAVE/XSAVE, with XCR0 enabling AVX): the engine uses SSE and AVX2.
  The kernel also reports AVX as enabled (CPUID OSXSAVE + XGETBV), which
  the engine checks. Done: `kernel/arch/x86_64/fpu.c`.
- **Threads** in one process (SIEOS has them) and a way to run a function on
  N threads and wait: siad keeps a pool (`user/sia/local.c`), whose threads
  sleep in `ipc_recv` between answers.
- **A memory quota** per process (init gives siad 4.5 GiB: `SPAWN_QUOTA`),
  and enough virtual memory for the weights (the heap may grow to 64 GiB).
- **Reading big files** from SieFS in large pieces (`read` callback; 64 MiB at a
  time); later, mapping the file directly would avoid the copy.
- A clock (`now_ns`) for the statistics; optional.

## The interface, for the sia server

```c
llm_env_t env = { .ctx = srv, .alloc = …, .free = …, .read = read_model_file,
                  .parallel = run_on_pool, .threads = 8, .now_ns = clock_ns,
                  .budget = 4ULL << 30, .ctx_len = 4096 };
llm_t *m;  llm_open(&m, &env, file_size, err, sizeof err);
llm_session_t *s = llm_session_new(m, &e);                // one per sia session
llm_sampler_t p;  llm_default_sampler(&p);
llm_ask(s, system_prompt_or_NULL, user_text);             // SIA_ASK
while ((n = llm_next(s, &p, piece, sizeof piece)) >= 0)   // SIA_NEXT, one call each
    send(piece, n);
llm_stop(s);  llm_session_reset(s);  llm_session_free(s); // SIA_STOP, SIA_CLOSE
llm_info(m, &info);  llm_stats(s, &st);                   // SIA_INFO
```

`llm_next` reads one token per call and returns at most one token's bytes,
so a server can interleave sessions and stop between any two tokens.

## Models

Downloaded into `models/` (not part of the source tree; `models/SHA256SUMS`
holds their published checksums, all verified):

| File | Model | Licence | Size |
|---|---|---|---|
| `SmolLM2-135M-Instruct-Q8_0.gguf` | SmolLM2 135M instruct, 8-bit | Apache-2.0 | 145 MB |
| `SmolLM2-135M-Instruct-Q4_K_M.gguf` | the same, 4-bit mixed (tests every format) | Apache-2.0 | 105 MB |
| `smollm2-360m-instruct-q8_0.gguf` | SmolLM2 360M instruct, 8-bit | Apache-2.0 | 386 MB |
| `smollm2-1.7b-instruct-q4_k_m.gguf` | SmolLM2 1.7B instruct, 4-bit: **the default** | Apache-2.0 | 1,056 MB |

Sources: Hugging Face, `HuggingFaceTB/SmolLM2-*-Instruct-GGUF` and
`bartowski/SmolLM2-135M-Instruct-GGUF`.

## Limits

- Architectures: Llama-family only (llama, mistral, qwen2); no mixture of
  experts, no sliding-window attention.
- Formats: no Q2_K, Q3_K, IQ formats.
- AVX2 kernels for Q8_0, Q4_0, Q5_0, Q4_K, Q6_K, F16, F32; Q4_1, Q5_1, Q5_K,
  BF16 use the plain C kernels (correct, slower).
- No AVX-512 path (the target laptop has none).
- AArch64: NEON kernels for Q8_0, Q4_0, Q4_K, Q6_K, F16, F32 only; `sdot`
  used when the kernel reports it (`HWCAP_DOTPROD`: Pi 5 yes, Pi 4 no);
  speeds measured only under emulation.
- The cache is half precision; no 8-bit cache yet (it would halve its size).
- Long-context handling: when full, a session starts again with the current
  turn (no sliding window).
- Non-ASCII letter detection in the tokenizer is approximate (see above).
- Several-vectors kernels (prompts) exist for Q4_K and Q6_K only; other
  formats read prompts one vector at a time per row (correct, slower).
