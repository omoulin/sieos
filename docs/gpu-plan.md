# Plan: sia on the GPU

What must be built so sia's local model runs on the target laptop's
NVIDIA GPU, in what order, and how we know each step works. The goal of
[ai-plan.md](ai-plan.md)'s step 4, made concrete. Part of SIEOS.
(c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

## Why, and how much faster

The development machine is the target laptop: i7-14700HX (8 P + 12 E
cores), DDR5, and a **GeForce RTX 5060 Laptop** (GB206, Blackwell,
`10de:2d19`, 8 GiB GDDR7 on a 128-bit bus). Its BARs as the firmware
leaves them: BAR0 64 MiB of registers, **BAR1 8 GiB** (resizable BAR: all
of the memory is visible to the CPU), BAR3 32 MiB.

| | CPU today (8-16 threads, in SIEOS) | GPU (expected) |
|---|---|---|
| Memory bandwidth | 55-79 GB/s | ~380 GB/s (to be measured) |
| Generation, 1.7B Q4_K_M (1.06 GB a token) | 37-55 tok/s | ceiling ~360; expect 150-250 |
| Generation, a 3-4B 4-bit model (~2 GB) | ~20-25 tok/s | 90-150 |
| Reading a prompt, 1.7B | 110-153 tok/s | thousands (matrix products on the tensor cores) |

Reading prompts is where sia waits most: the system text, the desktop's
file list (`SIA_CONTEXT`) and every resumed conversation are read again
before the first word of an answer ([sia.md](sia.md)). On the GPU that
drops from seconds to a fraction of one, and the 3-4B model that the AI
plan targets becomes fast enough to be the default.

**Not planned:** the Intel iGPU and the Raspberry Pis' V3D share the
CPU's memory. Generation is limited by memory bandwidth, so they would not
be faster, and prompts only a little. QEMU has no GPU for computing short
of a whole Vulkan stack (virtio-gpu "venus"). Development in QEMU passes
the real GPU through instead (below).

## What it takes

Since Turing, an NVIDIA GPU is run by its own processor, the **GSP**,
which runs NVIDIA's signed firmware (the "resource manager", RM). The
host driver loads that firmware, then asks it for everything (memory,
channels, engines) through **RPC messages** in shared memory. Blackwell
can only be driven this way, and that is good news: the firmware does
the clocks, power, memory training and engine set-up, so the driver
stays small. Three open references describe every step:

- NVIDIA's **open-gpu-kernel-modules** (MIT/GPL): the RPC structures and
  the Blackwell boot sequence, the authority for the pinned firmware
  version;
- **nouveau**'s GSP code in Linux (`nvkm/subdev/gsp`, the r570 firmware,
  Blackwell support);
- **nova-core** (Linux, Rust): a new, smaller GSP driver, readable as a
  checklist of what is really needed.

```
  siad  (the engine: llm/ + llm/gpu.c)
    │  writes commands into its own channel (shared memory) and rings the
    │  doorbell; waits on a semaphore the GPU writes. No message per token.
    │
    │  set-up only: SIA_GPU_OPEN → a channel, its memory, the doorbell page
    ▼
  nvgpu  (new server, owns the PCI device)
    │  GSP firmware loaded and booted; RPCs: client, device, address space,
    │  memory, channel, compute and copy engines; page tables; errors, resets
    ▼
  GPU:  GSP (RM firmware) · FSP (secure boot) · compute (SMs) · copy engine · 8 GiB
```

**Why a separate server.** siad reads prompts and replies from the
network. Giving it the device (registers, DMA) would put the whole
machine within reach of a bug in a parser. nvgpu holds the device; siad
holds only a channel, and the GPU's own page tables, which nvgpu writes,
confine that channel to the memory nvgpu gave it. The submission path
stays fast: the channel's command ring is memory shared between the
two, and the doorbell is a page of BAR0 designed to be handed to a
process (NVIDIA's "usermode" region).

## Phases

Each phase is merged when its tests pass, as usual. Phase 2 runs on
Linux and validates every GPU kernel before any SIEOS driver exists.
That way the hard driver work (phases 4-5) never has to debug math at
the same time.

### Phase 1: a GPU-shaped engine (CPU only)

1. `llm/model.c`'s forward pass calls the backend through a small table:
   buffers (`alloc`, `upload`, `download`), operations (`matvec`,
   `matmul`, `rmsnorm`, `rope`, `attn`, `silu_mul`, `add`, `embed`), and
   `sync`. The CPU backend is today's code behind it; no graph, no
   scheduler: one call per operation, as now.
2. The session's KV cache and the scratch buffers become backend buffers.

*Test:* `make llm-test`, `llm-neon-test` and `sia-test` give
bit-identical results. *Cost:* no measurable slowdown; a few hundred
bytes of code.

### Phase 2: the GPU kernels, on Linux

1. **Kernels in PTX** (`llm/gpu/*.ptx`, about 15): `matvec` for Q4_K,
   Q6_K, Q8_0 and F16 (generation: memory-bound, one warp per row,
   `dp4a`); `matmul` for prompts (8-bit tensor-core `mma`, then tuned);
   activation quantization to 8 bits; RMSNorm, RoPE (both pairings), the
   attention for one token (split over the cache, "flash decoding") and
   for a prompt; SiLU ⊙ up, add, embedding rows, and the argmax/top-k
   of the logits (so that only a few values come back to the CPU).
2. **Built on the development machine** by `ptxas` (CUDA toolkit) for
   `sm_120`. The PTX and the resulting **cubin** (ELF) go into git with
   its checksum, so a SIEOS build needs no NVIDIA tool. Rebuilding a
   cubin is only needed when a kernel changes (`make gpu-kernels`).
3. **`llm/gpu.c`**: the GPU backend of phase 1's table: it lays out the
   weights in GPU memory, keeps the KV cache there, and launches the
   kernels. Its lower edge is five calls (`mem_alloc`, `copy`, `launch`,
   `fence`, `wait`) from `llm_env_t`, like the CPU's `parallel`.
4. **A host tool** (`llm-run --gpu`) implements those five with the CUDA
   *driver* API (`libcuda.so`, opened at run time: `cuModuleLoadData` on
   the same cubin, `cuLaunchKernel`). It is test equipment, like
   `llm-run` itself.

*Tests:* every kernel against the plain C kernels on random blocks, as in
`llm-neon-test` (the integer sums exact, floats within 1e-6 relative);
end to end on the four models: the same greedy tokens as the CPU, logits'
correlation ≥ 0.999. *Measures:* tokens per second for generation and
prompts on this GPU: the numbers the SIEOS driver must then reach.

*You do once (root):* install NVIDIA's open driver (≥ the pinned
firmware's version) and the CUDA toolkit on Debian. Phases 4-6 need the
GPU back from that driver (below).

### Phase 3: what the SIEOS kernel and QEMU need

1. **Mapping BARs above 4 GiB**: BAR1 is at 384 GiB here. Check that
   `SYS_MAP_PHYS` and the kernel's page tables take a 64-bit address and
   an 8 GiB size, write-combined (`MAP_WC`) for BAR1.
2. **Sharing pages with another process**: a driver-only `SYS_GRANT`
   (pid, address, size, flags) that maps its DMA memory or a device page
   into another process. This is the one new kernel mechanism. It is
   undone when either process ends.
3. **Interrupts: none at first.** SIEOS has no MSI yet, and the GPU's legacy
   line may not be routed. nvgpu polls the GSP's message queue (with
   `SYS_SLEEP` between looks), siad polls its semaphore. MSI-X comes
   later (phase 8).
4. **PCIe extended configuration space** (ECAM from ACPI's MCFG) only if
   the driver needs capabilities beyond 256 bytes. The firmware has
   already sized the BARs, so it probably does not.
5. **`make run-gpu`**: QEMU `q35` with `-device vfio-pci,host=01:00.0`
   (its audio function too: both are IOMMU group 17, alone),
   and `pci-hole64-size` large enough for the 8 GiB BAR. A GPU that hangs takes down only the VM, and QEMU resets it
   for the next run.

*Test:* in `run-gpu`, `lspci`-like output from SIEOS shows the GPU and its
BARs; the existing tests still pass without it.

*You do (root), each time the GPU moves to the VM:* unbind it from the
host driver and bind `vfio-pci`; once, give your user `/dev/vfio/17` and
a locked-memory limit as large as the VM.

### Phase 4: nvgpu, up to "GSP running"

1. Identify the chip (`NV_PMC_BOOT_0`/`BOOT_42`); refuse anything but
   GB20x (one chip family, one firmware).
2. **Firmware**: one pinned version of NVIDIA's GSP firmware (the
   GB20x files from linux-firmware, or `gsp_ga10x.bin` from NVIDIA's
   driver package of the same version), fetched with its SHA-256 like
   MicroPython (`.hosttools`, not in git) and installed in
   `/lib/firmware/nvidia/`. It is tens of MB, and only laptop images
   carry it.
3. Lay out what the firmware expects: its image and the page tables that
   describe it ("radix3"), the boot arguments, the shared command and
   message queues. Then the **boot through the FSP** (Blackwell's secure
   boot), and wait for the RM's "init done" event.
4. The RPCs of start-up: the system information it asks for (PCI ids,
   BAR addresses, memory), the registry, the log buffers (whose content
   nvgpu prints to the console log: the firmware's own messages are the
   main debugging tool).

*Test (`run-gpu`):* `gpu info` prints the chip, the memory size and the
firmware's version, and a second boot of the VM works again (a clean
reset). *Debugging aid:* on Linux, nouveau's or NVIDIA's driver traced
with `mmiotrace` and their GSP RPC logs show the exact sequence to copy.

### Phase 5: a channel and the first kernel

1. RPCs for the RM objects: the client, device and subdevice, an
   address space, VRAM and system-memory allocations, a channel group,
   a **GPFIFO channel**, the Blackwell **compute** and **copy** engine
   objects, the usermode doorbell. The class numbers and method layouts
   come from NVIDIA's published class headers.
2. The GPU's page tables for the address space (written by nvgpu, as
   nouveau does), and a simple allocator of GPU memory (8 GiB, big
   blocks: weights, cache, scratch).
3. Launching a kernel: the cubin parsed (code, register count, shared
   memory, parameter offsets from `.nv.info`), a **QMD** (the launch
   descriptor) written, and its address sent to the compute engine; a
   semaphore released after it.
4. **Copy engine**: system memory to VRAM, in 64 MiB pieces. Writing
   through BAR1 directly (write-combined) may be simpler and as fast,
   because the whole memory is visible. Measure both and keep one.
5. `SIA_GPU_OPEN`: a channel set up in a client's name and its ring, its
   semaphores and a VRAM window granted to it (`SYS_GRANT`).

*Test (`run-gpu`):* `gpu test` copies 1 GiB in and out (checked, with its
bandwidth), adds two vectors, and runs every phase 2 kernel against the
CPU on random data. A kernel that faults reports an error and nvgpu
recovers the channel without a reboot.

### Phase 6: sia on the GPU

1. siad opens nvgpu when it exists and the model fits the GPU's memory;
   otherwise, or after any GPU error, **the CPU backend answers**, as
   now. sia never stops working because of the GPU.
2. The weights go from SieFS to the GPU in 64 MiB pieces (no second copy
   in RAM: siad's RAM use drops by the model's size). The KV cache lives
   in VRAM; sampling stays on the CPU, from the top-k values.
3. A token's work is written into the channel as one batch (24 layers ×
   about 10 launches for the 1.7B model) and waited for once. No message
   to nvgpu while answering.
4. `sia info` shows the backend ("gpu: RTX 5060 Laptop, 8 GiB") and the
   speeds; `/etc/sia.conf` gets `device=auto|cpu|gpu`.

*Tests:* `make sia-test` unchanged (no GPU in plain QEMU: the CPU path);
`make gpu-sia-test` in `run-gpu`: the same answers as the CPU (greedy),
speed close to phase 2's on Linux, the GPU taken away mid-answer (nvgpu
killed) → the answer goes on on the CPU.

### Phase 7: on the laptop itself

From the USB key ([usb.md](usb.md)), with the firmware on it. Hybrid
graphics: the UEFI firmware normally leaves the GPU powered (D0). If it
does not, powering it needs an ACPI method, a separate and bigger
project. *Test:* the phase 5-6 tests on the bare machine, and the speeds.

### Phase 8: later

MSI-X (no more polling); the **IOMMU** (VT-d) so that the GPU's DMA
reaches only nvgpu's memory; letting the GPU idle at low power between
answers; an 8-bit KV cache; tuning the prompt kernels; the 3-4B model as
the default where a GPU is present.

## Size budget

| Part | Lines (C or PTX) | Code |
|---|---|---|
| Phase 1 table | ~200 | + < 1 KB |
| `llm/gpu.c` | ~1,000 | ~15 KB in siad |
| Kernels (PTX) | ~2,500 | cubin ~200 KB, loaded into the GPU |
| nvgpu | 6,000-8,000 | ~100 KB; the RPC header generated for the pinned firmware only |
| Kernel (`SYS_GRANT`, BAR checks) | ~150 | < 1 KB |

nouveau's GSP path is several times larger because it serves every chip
since Turing and every firmware it supports. nvgpu serves one chip family
and one firmware version, and only computes (no display, no video, no
3D).

## Risks

| Risk | What we do |
|---|---|
| Blackwell's boot (FSP, firmware layout) is the least documented part | three open drivers to compare; a trace of Linux's driver on this very GPU; the VM makes each try cheap |
| The RPC structures change with every firmware version | pin one version; generate the few structures needed from that version's open-gpu-kernel-modules |
| `ptxas` is NVIDIA's tool, under NVIDIA's licence | needed only to rebuild kernels on the development machine; its output is committed (check that the CUDA licence covers redistributing it). Writing machine code ourselves is possible later (Mesa's NAK documents the encoding) but not planned |
| A wedged GPU on the bare machine | only a reboot fixes it; that is one reason why the CPU fallback matters |
| Hybrid-graphics power (ACPI) | check early in phase 7; a laptop whose firmware powers the GPU down needs an ACPI interpreter first |
| No IOMMU in SIEOS until phase 8 | nvgpu is the only process with the device; the GPU's own page tables confine siad's channel |

## What you will be asked to do

- Install NVIDIA's open driver and the CUDA toolkit on the host (phase 2).
- Hand the GPU to `vfio-pci` before each `make run-gpu` session, and back
  afterwards (phases 3-6); one-time permissions for `/dev/vfio/17`.
- Run the laptop from the USB key for phase 7.
