# Plan: sia on the GPUs

What must be built so sia's local model runs on the GPUs of SIEOS's
machines: the target laptop's **NVIDIA** GPU and its **Intel** iGPU, and
the Raspberry Pis' **VideoCore** (V3D). In what order, and how we know each
step works. The goal of [ai-plan.md](ai-plan.md)'s step 4, made concrete.
Showing the picture is a separate job (the laptop's Intel display driver,
[desktop.md](desktop.md); the Pis' screens, part 3 below).
Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

## Why, and how much faster

Generation reads every weight once per token, so it is limited by memory
bandwidth; reading a prompt is a stack of matrix products, limited by
arithmetic. A GPU with its own memory wins at both. A GPU that shares the
CPU's memory (Intel's iGPU, the Pis' V3D) cannot generate much faster than
the cores, but it can read prompts faster where the cores are short of
arithmetic, and it leaves the cores free while it works.

| Machine | GPU | Memory | Peak arithmetic (to be measured) |
|---|---|---|---|
| Laptop | **GeForce RTX 5060 Laptop** (GB206, Blackwell, `10de:2d19`) | 8 GiB GDDR7, ~380 GB/s, its own | tensor cores: tens of TOPS (8-bit) |
| Laptop | **Intel UHD Graphics** (Raptor Lake-S, Xe-LP, `8086:a788`, 32 EUs at up to 1.6 GHz) | shared DDR5 (the cores reach 55-79 GB/s) | ~0.8 TFLOPS fp32, ~3.3 TOPS 8-bit (`dp4a`) |
| Pi 4 | **V3D 4.2** (VideoCore VI, 8 QPUs at 500 MHz) | shared LPDDR4, 4-5 GB/s | ~32 GFLOPS fp32, no 8-bit dot product |
| Pi 5 | **V3D 7.1** (VideoCore VII, 960 MHz) | shared LPDDR4X, 10-17 GB/s | roughly 2-3x the Pi 4's |

The laptop's NVIDIA GPU: BAR0 64 MiB of registers, **BAR1 8 GiB**
(resizable BAR: all of the memory is visible to the CPU), BAR3 32 MiB.

| SmolLM2-1.7B Q4_K_M | CPU today (in SIEOS) | NVIDIA (expected) | Intel iGPU (expected) | Pi V3D (expected) |
|---|---|---|---|---|
| Generation | 37-55 tok/s (16 threads) | 150-250 | 40-60: the same memory, the cores free | Pi 5: 5-9 (estimate), no faster than its cores |
| A 3-4B 4-bit model (~2 GB) | ~20-25 tok/s | 90-150 | ~20-30 | does not fit well |
| Reading a prompt | 110-153 tok/s | thousands | 300-500 (ceiling ~950: 3.4 G operations a token) | Pi 4: maybe 1-2x the cores; Pi 5: below its `sdot` cores |

Reading prompts is where sia waits most: the system text, the desktop's
file list (`SIA_CONTEXT`) and every resumed conversation are read again
before the first word of an answer ([sia.md](sia.md)). On the NVIDIA GPU that
drops from seconds to a fraction of one, and the 3-4B model that the AI
plan targets becomes fast enough to be the default. The **Intel iGPU**
matters when the NVIDIA GPU is off (on battery it is better powered down:
it draws several watts idle) or absent: prompts 2-4x faster than the
cores, and the cores stay free for the desktop. The **Pis' V3D** is the
weakest case: the numbers say it will not beat the cores at arithmetic
(the Pi 5's cores have `sdot`, the GPU has no 8-bit dot product). So the
Pi part starts with a measurement on Linux and is built only if that
measurement shows a gain (part 3).

QEMU has no GPU for computing short of a whole Vulkan stack (virtio-gpu
"venus"). Development in QEMU passes the real NVIDIA GPU through instead
(below). The Intel iGPU drives the laptop's own screen, so it cannot be
passed to a VM: it is developed on the laptop from the USB key. The Pis:
on the boards.

**One rule for all three.** The kernels are first written and checked on
Linux against the plain C kernels, with the vendor's own stack as test
equipment; only then does a SIEOS driver run them. The hard driver work
never debugs math at the same time. And sia never depends on a GPU: any
GPU error, or a model that does not fit, and **the CPU backend answers**.

## Phase 1: a GPU-shaped engine (CPU only, shared by all three)

1. `llm/model.c`'s forward pass calls the backend through a small table:
   buffers (`alloc`, `upload`, `download`), operations (`matvec`,
   `matmul`, `rmsnorm`, `rope`, `attn`, `silu_mul`, `add`, `embed`), and
   `sync`. The CPU backend is today's code behind it; no graph, no
   scheduler: one call per operation, as now.
2. The session's KV cache and the scratch buffers become backend buffers.

*Test:* `make llm-test`, `llm-neon-test` and `sia-test` give
bit-identical results. *Cost:* no measurable slowdown; a few hundred
bytes of code.

## Part 1: NVIDIA (the RTX 5060)

The largest gain, so the first to be built.

### What it takes

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

### Phases

Each phase is merged when its tests pass, as usual. Phase 2 runs on
Linux and validates every GPU kernel before any SIEOS driver exists.

#### Phase 2: the GPU kernels, on Linux

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

#### Phase 3: what the SIEOS kernel and QEMU need

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

#### Phase 4: nvgpu, up to "GSP running"

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

#### Phase 5: a channel and the first kernel

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

#### Phase 6: sia on the GPU

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

#### Phase 7: on the laptop itself

From the USB key ([usb.md](usb.md)), with the firmware on it. Hybrid
graphics: the UEFI firmware normally leaves the GPU powered (D0). If it
does not, powering it needs an ACPI method, a separate and bigger
project. *Test:* the phase 5-6 tests on the bare machine, and the speeds.

#### Phase 8: later

MSI-X (no more polling); the **IOMMU** (VT-d) so that the GPU's DMA
reaches only nvgpu's memory; letting the GPU idle at low power between
answers; an 8-bit KV cache; tuning the prompt kernels; the 3-4B model as
the default where a GPU is present.

### Size budget

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

### Risks

| Risk | What we do |
|---|---|
| Blackwell's boot (FSP, firmware layout) is the least documented part | three open drivers to compare; a trace of Linux's driver on this very GPU; the VM makes each try cheap |
| The RPC structures change with every firmware version | pin one version; generate the few structures needed from that version's open-gpu-kernel-modules |
| `ptxas` is NVIDIA's tool, under NVIDIA's licence | needed only to rebuild kernels on the development machine; its output is committed (check that the CUDA licence covers redistributing it). Writing machine code ourselves is possible later (Mesa's NAK documents the encoding) but not planned |
| A wedged GPU on the bare machine | only a reboot fixes it; that is one reason why the CPU fallback matters |
| Hybrid-graphics power (ACPI) | check early in phase 7; a laptop whose firmware powers the GPU down needs an ACPI interpreter first |
| No IOMMU in SIEOS until phase 8 | nvgpu is the only process with the device; the GPU's own page tables confine siad's channel |

## Part 2: Intel (the laptop's UHD Graphics)

### What it takes

The same chip runs the laptop's screen: the display driver (`igfx.c` in
Atlas, which sets the modes) is built separately and is the first user of
the device. Computing needs the other half of the chip, the **render
engine** and its EUs. On this generation (Gen12, Xe-LP) it needs **no
firmware**: the GuC and HuC are optional, and the host can submit work
itself through the **execlists** (`ELSP`), as Linux's i915 does on this
chip. Public references cover every step:

- Intel's **PRMs** for Tiger Lake / Alder Lake-S (volume 2, the command
  reference: `PIPELINE_SELECT`, `STATE_BASE_ADDRESS`,
  `MEDIA_INTERFACE_DESCRIPTOR_LOAD`, `GPGPU_WALKER`, `PIPE_CONTROL`, the
  logical ring context layout);
- Linux's **i915** (`gt/intel_execlists_submission.c`, `gt/intel_lrc.c`,
  `gt/gen8_ppgtt.c`, forcewake in `intel_uncore.c`);
- Intel's **compute-runtime** (NEO) and Mesa's **anv**, for how a compute
  walker and its state are laid out.

```
  siad  (llm/ + llm/igpu.c)
    │  writes batches (walkers) into its own ring buffer and asks igpu to
    │  submit; waits on a fence the GPU writes (PIPE_CONTROL)
    │  set-up: SIA_IGPU_OPEN → a context, its ring, its GPU address space
    ▼
  igpu  (new server: the render engine half of 00:02.0)
    │  forcewake; one logical ring context per client; execlist submission;
    │  per-client page tables (PPGTT, 4 levels); hangs: engine reset
    ▼
  GPU:  render engine (EUs, L3) · the GGTT, split with the display driver
```

**Two owners, one device, kept apart.** Atlas's display driver touches
only the display registers (0x40000 up) and the low part of the global
GTT (the frame buffers); igpu touches only the render engine's registers,
forcewake and the GGTT's upper half (its rings and context images). The
split is written down in both files and checked at start-up. Merging both
into one server would cost an IPC on every frame; this costs nothing.

**No copies.** The iGPU reads the same RAM as the cores, so the weights
are not uploaded anywhere: igpu maps siad's pages (the model, the KV
cache) straight into siad's GPU address space. That needs one kernel
mechanism: the physical addresses of a client's pinned pages, given to a
driver (`SYS_PIN`, the counterpart of NVIDIA's `SYS_GRANT`).

**Kernels.** Written in **OpenCL C** (`llm/igpu/*.cl`), compiled on the
development machine by Intel's offline compiler `ocloc` for Gen12LP, the
binary (zebin, an ELF) committed with its checksum, like the cubin: a
SIEOS build needs no Intel tool. The same operations as part 1's PTX:
`matvec` with `dp4a` (one subgroup per row), `matmul` for prompts in
8-bit with tiles in shared local memory, RMSNorm, RoPE, attention,
SiLU, add, embedding, top-k.

### Phases

#### Intel phase 1: the kernels, on Linux

1. The kernels above, built by `ocloc` (`make igpu-kernels`).
2. **`llm/igpu.c`**: the backend of phase 1's table, its lower edge the
   same five calls as `llm/gpu.c`.
3. **A host tool** (`llm-run --igpu`) runs them through OpenCL
   (`libOpenCL.so` opened at run time, `clCreateProgramWithBinary` on the
   same binary). Test equipment only.

*Tests:* every kernel against the plain C kernels on random blocks; end
to end on the four models: the same greedy tokens as the CPU. *Measures:*
prompt and generation speeds, alone and with the cores busy. **Gate:** if
prompts are not at least 2x the cores' 110-153 tok/s, stop here and
write down why.

*You do once (root):* install Intel's compute runtime and `ocloc`
(Debian: `intel-opencl-icd`, `intel-ocloc`).

#### Intel phase 2: igpu, up to "a batch runs"

1. Identify the chip (Gen12, `8086:a788` and its siblings); forcewake
   for the render domain.
2. The engine: reset, one logical ring context (the image laid out as
   the PRM says), a ring buffer, an execlist submission, and a batch of
   `MI_STORE_DATA_IMM` that writes a value to memory. Then the global
   GTT split with the display driver, and PPGTT for a client.
3. Hang detection: a batch that does not finish in 2 s gets the engine
   reset (`GDRST`) and an error to its client; igpu goes on.

*Test (on the laptop):* `igpu test` runs the store batch 10,000 times and an endless batch
that must be reset; the screen (Atlas) keeps working throughout.

#### Intel phase 3: compute and sia

1. Compute state: `PIPELINE_SELECT` GPGPU, `STATE_BASE_ADDRESS`,
   the interface descriptor (the kernel's start, its threads, its shared
   local memory, from the zebin's notes), `GPGPU_WALKER`, `PIPE_CONTROL`
   with a fence. Stateless (64-bit addresses) access only: no binding
   tables.
2. `SIA_IGPU_OPEN` and `SYS_PIN`; siad uses the iGPU when there is no
   usable NVIDIA GPU and `/etc/sia.conf` says `device=auto|igpu`.

*Tests:* `igpu test` runs every kernel against the CPU on random data;
`sia-test`'s answers the same as the CPU's (greedy); igpu killed mid-answer
→ the answer goes on on the CPU.

### Size budget

| Part | Lines | Code |
|---|---|---|
| `llm/igpu.c` | ~600 | ~8 KB in siad |
| Kernels (OpenCL C) | ~1,200 | binary ~100 KB |
| igpu | 2,000-3,000 | ~35 KB; no firmware |
| Kernel (`SYS_PIN`) | ~80 | < 1 KB |

### Risks

| Risk | What we do |
|---|---|
| The context image's layout must be exact, or the engine hangs | copy i915's register lists for Gen12 exactly; start with the smallest batch |
| Shared with the display: a render hang must not take the screen | engine reset only (never a full GPU reset); the display registers are not touched |
| `ocloc`'s output format changes | pin its version, as for the firmware elsewhere |
| No VM for the iGPU (it drives the screen) | develop on the bare laptop from the USB key; a hang there costs a reboot, so phase 2 tests the engine reset first |

## Part 3: Raspberry Pi (VideoCore VI and VII)

The Pis' GPU has two parts: the **display** (the HVS, which composes
planes, and the HDMI controllers) and **V3D** (3D and compute). Today the
firmware owns both: on the Pi 4 SIEOS asks it for a frame buffer through
the mailbox, on the Pi 5 it takes the fixed one the firmware left
(`screen.c`).

### The display (built: it pays for itself)

1. **Pi 5 at any size.** The Pi 5's frame buffer has the firmware's fixed
   size, so Settings cannot change it (the picture is centred at 1920 x
   1080). Programming the HVS directly (a display list: one plane, its
   scaling to the mode) gives the Pi 5 the Pi 4's choice of sizes, with
   the mode itself left as the firmware set it.
2. **Tear-free and a hardware cursor** on both: two frame buffers swapped
   at vertical blank, and the pointer as its own HVS plane, so moving it
   redraws nothing.
3. Clocks and power stay with the firmware (mailbox calls), as Linux's
   `vc4` driver does: no clock tree in SIEOS.

References: Linux `drivers/gpu/drm/vc4` (`vc4_hvs.c`, `vc4_plane.c`,
`vc4_crtc.c`; the BCM2712 differences are marked there), and the
BCM2711 peripherals document.

*Test:* QEMU's `raspi4b` has no HVS, so on the boards: Settings changes
the Pi 5's size and goes back; the pointer moves without trails;
`gui-test` unchanged in QEMU (the firmware path stays as the fallback).
*Size:* ~600-900 lines in Atlas (`screen.c` splits into a Pi file).

### V3D for sia (measured first)

1. **Measure on Linux** (Raspberry Pi OS on each board): an 8-bit and a
   16-bit matrix-vector product and a matrix product written as QPU code
   (as `py-videocore6` does on the Pi 4), against SIEOS's NEON kernels on
   the same board. **Gate:** built only if a prompt reads at least 1.5x
   faster than on the four cores. On the Pi 5 that is unlikely (its cores
   have `sdot`); on the Pi 4 it is possible.
2. If built: kernels in QPU assembly (`llm/v3d/*.qasm`), assembled at
   build time by a small assembler (`tools/qasm.c`, ~500 lines: no Mesa);
   a **v3dd** server: V3D powered and clocked through the mailbox, its
   own MMU (4 KiB pages, a 4 GiB space) mapping siad's pinned pages
   (`SYS_PIN`, no copies), jobs through the **compute shader dispatch**
   (CSD) registers, a hang reset. References: Linux `drivers/gpu/drm/v3d`,
   Mesa's `broadcom/` (the QPU instruction encoding), `py-videocore6`.
3. siad uses it under the same rules: `device=auto|v3d`, the CPU answers
   on any error.

*Size if built:* v3dd ~1,000-1,500 lines, kernels ~1,000 lines of QPU
assembly, `llm/v3d.c` ~400.

## Order across the GPUs

1. Phase 1 (the backend table): shared, first.
2. NVIDIA, phases 2-7: the largest gain.
3. Intel, phases 1-3: after NVIDIA's phase 5, which builds `SYS_PIN`'s
   sibling `SYS_GRANT` and proves the backend's five calls on a GPU.
4. The Pis' display: independent of all the above, any time.
5. The Pis' V3D: its Linux measurement any time; the rest only if it
   passes the gate.

## What you will be asked to do

- Install NVIDIA's open driver and the CUDA toolkit on the host (NVIDIA
  phase 2); Intel's compute runtime and `ocloc` (Intel phase 1).
- Hand the NVIDIA GPU to `vfio-pci` before each `make run-gpu` session,
  and back afterwards (phases 3-6); one-time permissions for `/dev/vfio/17`.
- Run the laptop from the USB key for NVIDIA phase 7 and Intel phases 2-3.
- Run Raspberry Pi OS on the Pis for the V3D measurement, and SIEOS on the
  boards for the display work.
