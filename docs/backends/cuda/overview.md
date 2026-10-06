<!-- markdownlint-disable MD013 MD060 -->

# CUDA Backend

The CUDA backend runs VMAF's feature extractors directly on an NVIDIA GPU
(compute capability 8.0 or newer), keeping frames on the device across the
pipeline to avoid PCIe round-trips. Build with `-Denable_cuda=true`, run with
`--backend cuda`.

## Overview

- **Coverage.** CUDA twins exist for `vif`, `adm`, `motion`, `motion_v2`,
  `psnr`, `psnr_hvs`, `ssim`, `float_ssim`, `float_ms_ssim`, `float_psnr`,
  `float_moment`, `float_motion`, `float_vif`, `float_adm`, `cambi`, `ciede`,
  `speed_chroma`, `speed_temporal` and `ssimulacra2`. The per-extractor
  matrix is in [feature metrics](../../metrics/features.md).
- **Agreement with the CPU.** 24 of the 25 features of the cross-backend
  parity gate are bit-identical to the CPU extractor at `--precision max`;
  `ciede` is bounded at 1e-9. See [Numerical agreement](#numerical-agreement).
- **Driver API only.** The backend does not link `libcuda`; it loads the driver
  at run time, so applications that already load CUDA through FFmpeg share the
  same primary context.
- **Per-twin detail.** What each twin computes, its options and how to check
  it: [CUDA twin notes](twin-notes.md).

## Requirements

### Toolchain

Building needs the CUDA toolkit (`nvcc`, driver API headers). The release
pin is CUDA 13.4.2 (`CUDA_VERSION` in `build-config.env`; apt package
`cuda-toolkit-13-4`). The build uses the driver API only, through `ffnvcodec`
dynlink wrappers.

### GPU architecture coverage

**Minimum compute capability: 8.0 (Ampere).** The fork ships cubins for
every supported Nvidia generation from Ampere through Blackwell whenever
the host `nvcc` supports them, plus a `compute_80` PTX as an
unconditional JIT fallback:

| Generation | Arch     | Emitted as      | Host `nvcc` gate |
| ---------- | -------- | --------------- | ---------------- |
| Ampere     | `sm_80`  | cubin + PTX     | always           |
| Ampere     | `sm_86`  | cubin           | always           |
| Ada        | `sm_89`  | cubin           | always           |
| Hopper     | `sm_90`  | cubin           | `nvcc` > 11.8    |
| Blackwell  | `sm_100` | cubin           | `nvcc` > 12.8    |
| Blackwell  | `sm_120` | cubin + PTX     | `nvcc` > 12.8    |

The `compute_80` PTX is emitted unconditionally so any `sm_80`+ GPU
that lacks a matching cubin (future minor revisions, headless Tegra
variants) can still JIT a compatible kernel at driver-load time. This
diverges from upstream Netflix's meson.build, which ships cubins only
at Txx major boundaries; see
[ADR-0122](../../adr/0122-cuda-gencode-coverage-and-init-hardening.md).

!!! warning "Turing (`sm_75`) and older are not supported"
    [ADR-1223](../../adr/1223-cuda-ampere-architecture-floor.md) raised the
    floor to compute capability 8.0 and removed the `sm_75` cubin and the
    `compute_50` PTX that older toolkits emitted. CUDA 13.x had already
    dropped Maxwell (`sm_50`), Pascal (`sm_60`) and Volta (`sm_70`); this
    drops Turing (RTX 20xx, GTX 16xx, Tesla T4) as well.

`vmaf_cuda_state_init()` queries the device compute capability and
    returns `-ENOTSUP` with an explicit message rather than letting
    `cuModuleLoadData` fail with `CUDA_ERROR_NO_BINARY_FOR_GPU` (222)
    inside whichever feature extractor loaded first.

The error message is:

```text
CUDA: device "NVIDIA GeForce RTX 2080 Ti" has compute capability 7.5,
      below the minimum 8.0 (Ampere).
      libvmaf ships no cubin or PTX below sm_80, so no kernel can be
      loaded on this GPU.
      Turing (sm_75) and older were dropped in ADR-1223. Use an Ampere
      or newer GPU, or build with -Denable_cuda=false and run on the
      CPU backend.
```

### Runtime requirements

The CUDA backend is compiled against `nv-codec-headers` but **does not
link** against `libcuda` — instead it `dlopen`s the driver library at
runtime through the `cuda_load_functions()` helper from
`ffnvcodec/dynlink_loader.h`. This keeps libvmaf linkable in
environments where the GPU driver may not be present at build time
(CI images, cross-compilation), but it means two things must be true
at run time on any host that actually dispatches the backend:

1. **`libcuda.so.1` exists and is reachable by the dynamic loader.**
   On Linux the driver stub is typically installed by the Nvidia
   driver package at `/usr/lib/x86_64-linux-gnu/libcuda.so.1` (Debian/
   Ubuntu), `/usr/lib64/libcuda.so.1` (RHEL/Fedora), or under the
   distribution-specific Nvidia path. Check:

   ```bash
   ldconfig -p | grep -iE 'libcuda|libnvcuvid'
   ```

   If the line is missing, the backend will fail to initialise with a
   multi-line error message pointing at this section.

2. **The driver userspace matches the kernel module.** A fresh
   driver install that hasn't been followed by a reboot (or a
   `modprobe -r nvidia && modprobe nvidia`) commonly reports
   `cuInit(0)` returning a non-zero code even though `libcuda.so.1`
   loaded successfully. The log message for that case names
   `cuInit(0)` and the return code so the failure mode is
   distinguishable from the dlopen case above.

3. **The driver is new enough for the toolkit that built libvmaf.** A
   build made with CUDA 13.x needs driver R580 or newer (NVIDIA's minor
   version compatibility table in the
   [CUDA Toolkit release notes](https://docs.nvidia.com/cuda/cuda-toolkit-release-notes/)).
   The kernels are stored compressed (`compress_device_code`, on by default,
   see [Build](#build)): `nvcc --compress-mode` output needs a driver from CUDA
   12.4 (R550) or later, which the R580 floor already covers. An older driver
   fails `cuModuleLoadData()` in the first extractor that loads a kernel.

Statically-linked consumers (for example, ffmpeg binaries built with
`--enable-libvmaf` in static mode) are **not** exempt: the driver
library is loaded through `dlopen`, which bypasses `DT_NEEDED` and
therefore does not show up in `ldd <binary>`. An otherwise
self-contained static ffmpeg will still fail on the first frame if
`libcuda.so.1` is not on the loader path.

## Build

Run from the repository root:

```bash
meson setup build core -Denable_cuda=true
ninja -C build
```

Meson options:

| Option | Default | Effect |
|--------|---------|--------|
| `enable_cuda` | `false` | compile the CUDA backend and kernels |
| `enable_nvcc` | `true` | build kernel objects with `nvcc` (when `false`, the clang CUDA driver is used; experimental). Only effective with `enable_cuda=true` |
| `enable_nvtx` | `false` | instrument kernels with NVTX ranges; requires `enable_cuda=true` (see [NVTX profiling](../nvtx/profiling.md)) |
| `nvcc_threads` | `4` | parallel `nvcc` threads per kernel compile (1 to 32); see [build flags](../../development/build-flags.md) |
| `compress_device_code` | `true` | store every fatbin entry (cubins and PTX) compressed with `nvcc --compress-mode=size`; `false` stores them raw. Requires `enable_nvcc=true`: the clang CUDA path cannot compress, so it needs `false`. See [`compress_device_code`](../../development/build-flags.md#compress_device_code) |

## Run

When the binary is built with CUDA, the backend is auto-selected on GPU-capable
hosts. CLI controls:

```bash
./build/tools/vmaf ...                 # CUDA used automatically
./build/tools/vmaf --backend cuda ...  # exit 100 if CUDA cannot initialise
./build/tools/vmaf --no_cuda ...       # force CPU path
```

Automatic selection falls back to the CPU when CUDA cannot initialise, and
reports it only on stderr (`problem during vmaf_cuda_state_init, using CPU`);
the run still prints a score and exits 0. `--backend cuda` refuses to fall
back and exits with code `100` (see
[explicit-backend semantics](../index.md#explicit-backend-semantics-backend-name)).

In a container, automatic selection falls back to CPU without `--gpus all`:
the NVIDIA Container Toolkit then injects no GPU and no `libcuda.so.1`. With
`--gpus all`, the published `v1.0.0-rc.1-cuda13` image on an RTX 4090 scores
the Netflix `src01` pair with `vmaf_v0.6.1` at 76.6678303 on CUDA against
76.6678309 on the CPU.

### FFmpeg

The FFmpeg filter name is `libvmaf_cuda`; see
[usage/ffmpeg.md](../../usage/ffmpeg.md)
for a hwaccel pipeline that keeps decoded frames on the GPU. For
software-decoded input the regular `libvmaf` filter accepts a fork-added
`cuda=1` AVOption (per
[ADR-0408](../../adr/0408-ffmpeg-libvmaf-cuda-backend-selector.md)); build
FFmpeg with `--enable-libvmaf-cuda` to enable it.

### Importing frames through the VMAFx API

A program that already holds frames on the GPU (a decoder's surfaces, a
compositor's GL textures) hands them to a CUDA device of the VMAFx API without
a copy through the host: device pointers are read where they are, NV12 / P010
/ P016 are planarised on the device, and CUDA events order the producer's
writes and its reuse of the memory against the library's reads on the device.
See [CUDA devices](../../api/vmafx/index.md#cuda-devices) for the calls, the
layouts a plane must have, and the fences
([ADR-2023](../../adr/2023-vmafx-cuda-device-frames.md)).

Every CUDA twin reads a picture with that picture's own row pitch, so frames
whose planes have the producer's pitch (rather than the engine's
texture-aligned one) score as their host-uploaded copies; `integer_vif` read
both inputs with the engine's pitch until RC4 work package 3
(`T-CUDA-VIF-PICTURE-PITCH-2026-10-06` in [docs/state.md](../../state.md)).

### Dispatch knob (`VMAF_CUDA_DISPATCH`)

`VMAF_CUDA_DISPATCH` selects how a CUDA extractor submits work. libvmaf reads
it when the extractor initialises; each token is `extractor:strategy`, where
`extractor` is the CUDA extractor's registered name (`vif_cuda`,
`float_ssim_cuda`, ...). Two strategy names are accepted:

| Value | Behaviour |
|---|---|
| `direct` | Per-extractor submit and collect. **Default** for every extractor. |
| `graph` | CUDA graph capture. Not implemented: libvmaf logs `CUDA graph dispatch requested for '<extractor>' but graph capture is not implemented; falling back to direct` and runs `direct`. |

```bash
VMAF_CUDA_DISPATCH=vif_cuda:graph ./build/tools/vmaf --backend cuda ...
```

See [ADR-0483](../../adr/0483-gpu-dispatch-parse-dedup.md) for the parse
grammar and the [env-var reference](../../usage/env-vars.md#cuda-dispatch).

## Numerical agreement

The CUDA twins target the CPU extractors' bits, not a tolerance. At
`--precision max` 24 of the 25 gate features are bit-identical to
`--backend cpu` (one `scripts/ci/exact_twins.d/<feature>.cuda` fragment each),
and the gate compares them with tolerance 0. `ciede` differs only by the math
library and is bounded at 1e-9 (`LIBM_TWINS` in
`scripts/ci/cross_backend_calibration.py`). See also the
[cross-backend exact twins](../../development/cross-backend-exact-twins.md)
table and the [cross-backend gate](../../development/cross-backend-gate.md).

Default `%.6f` output hides sub-microscore deltas entirely; `--precision max`
exposes them
([ADR-0119](../../adr/0119-cli-precision-default-revert.md)).

### Floating-point model: no FMA contraction (ADR-1403)

Every CUDA kernel is compiled without contraction: `a * b + c` is a rounded
multiply followed by a rounded add, as in the CPU build and in the SYCL twins
([ADR-1367](../../adr/1367-sycl-strict-fp-every-feature-tu.md)). nvcc's
default would fuse it into one FMA, which rounds once.

- The build passes one flag list to every kernel
  (`cuda_device_strict_fp_args` in `core/src/meson.build`: `--fmad=false`
  under nvcc, `-ffp-contract=off` under clang's CUDA driver), and
  `core/test/test_strict_fp_compiler_args.py` fails if a kernel gets its own.
- Division and square root are IEEE as well (nvcc's `-prec-div` and
  `-prec-sqrt` defaults; the build never passes `--use_fast_math`).
- Where the CPU reference itself fuses (the MS-SSIM decimation, ssimulacra2's
  colour matrix, SpEED's exact products), the kernel writes the fused
  operation explicitly.

What this guarantees is the rounding of fp32 `+ - * /` and `sqrt`. A twin
still differs from the CPU where it calls a device math function (`log2f`,
`pow`, `atan2`), sums in another order, or uses another formula; the twins
below avoid each of those by construction.

### Agreement per twin

Measured against `--backend cpu` at `--precision max` on an RTX 4090
([Research-1403](../../research/1403-cuda-strict-fp-every-kernel.md) and the
ADRs below), on the Netflix 576x324 pair, both 1080p checkerboard pairs and
BBB 3840x2160. Per-twin detail, options and checks are in the
[twin notes](twin-notes.md).

| Twin | Agreement with the CPU | Since |
|---|---|---|
| `vif` | bit-identical (reads the CPU's log2 table; needs 16 pixels per side) | [ADR-1462](../../adr/1462-cuda-vif-reads-host-log2-table.md) |
| `adm` | bit-identical | [ADR-1416](../../adr/1416-cuda-adm-cpu-row-rounding.md) |
| `motion`, `motion_v2` | bit-identical | [ADR-1372](../../adr/1372-cuda-motion-diff-first-pipeline.md), [ADR-1457](../../adr/1457-cuda-exact-twins-declared.md) |
| `psnr` | bit-identical | [ADR-1457](../../adr/1457-cuda-exact-twins-declared.md) |
| `psnr_hvs` | bit-identical | [ADR-1397](../../adr/1397-psnr-hvs-twins-cpu-float-sum.md) |
| `ssim` | bit-identical | [ADR-1424](../../adr/1424-cuda-ssim-cpu-frame-sum.md) |
| `float_ssim` | bit-identical on every input | [ADR-1399](../../adr/1399-cuda-float-ssim-device-decimation.md), [ADR-1464](../../adr/1464-cuda-float-ssim-raster-order-sum.md) |
| `float_ms_ssim` | bit-identical on every input, `enable_lcs` outputs too | [ADR-1403](../../adr/1403-cuda-strict-fp-every-kernel.md), [ADR-1465](../../adr/1465-cuda-float-ms-ssim-raster-order-sum.md) |
| `float_psnr` | bit-identical | [ADR-1455](../../adr/1455-cuda-float-psnr-exact-block-sums.md) |
| `float_moment` | bit-identical on every frame | [ADR-1453](../../adr/1453-cuda-float-moment-cpu-float-squares.md), [ADR-1497](../../adr/1497-float-moment-twins-cpu-sum-past-2-53.md) |
| `float_motion` | bit-identical | [ADR-1409](../../adr/1409-float-motion-twins-cpu-float-sum.md) |
| `float_vif` | bit-identical | [ADR-1412](../../adr/1412-cuda-float-vif-cpu-arithmetic.md) |
| `float_adm` | bit-identical | [ADR-1420](../../adr/1420-cuda-float-adm-cpu-arithmetic.md) |
| `cambi` | bit-identical | [ADR-1379](../../adr/1379-cuda-cambi-device-resident-pipeline.md), [ADR-1457](../../adr/1457-cuda-exact-twins-declared.md) |
| `speed_chroma`, `speed_temporal` | bit-identical | [ADR-1380](../../adr/1380-cuda-speed-device-resident-pipeline.md), [ADR-1477](../../adr/1477-speed-upstream-double-math.md) |
| `ssimulacra2` | bit-identical | [ADR-1433](../../adr/1433-cuda-ssimulacra2-cpu-sum-order.md) |
| `ciede` | 127 of 180 frames identical, the rest within 5.2e-12 (bound 1e-9); the remainder is glibc's `powf` against CUDA's | [ADR-1426](../../adr/1426-cuda-ciede-cpu-arithmetic.md), [ADR-1467](../../adr/1467-ciede-squares-as-products.md) |

### Golden gate and snapshots

The Netflix golden-data gate is CPU-only — the three reference
pairs in `python/test/` (1 normal + 2 checkerboard) are hardcoded
`assertAlmostEqual` values that only the CPU scalar + fixed-point
path is required to match exactly. See
[docs/principles.md §3.1](../../principles.md#31-netflix-golden-data-gate).

GPU regression is caught by fork-added per-backend snapshot tests
(`testdata/scores_cpu_*.json` + `testdata/netflix_benchmark_results.json`),
which record what each backend produces today at a small ULP
tolerance. Regenerate intentionally via
[`/regen-snapshots`](../../../.claude/skills/regen-snapshots/SKILL.md)
with a commit-message justification; use
[`/cross-backend-diff`](../../../.claude/skills/cross-backend-diff/SKILL.md)
to surface an unexpected delta.

## Known gaps

Open items only. Closed gaps are in the [twin notes](twin-notes.md#history).

| Gap | Status | Detail |
|-----|--------|--------|
| CUDA graph capture dispatch | open | `VMAF_CUDA_DISPATCH=<extractor>:graph` logs `libvmaf: CUDA graph dispatch requested for '<extractor>' but graph capture is not implemented; falling back to direct` and runs `direct`. Graph capture needs static pitch allocation and graph instance lifecycle management across frames (deferred; `docs/state.md`). |
| Zero-copy picture import | open | `core/src/cuda/picture_cuda.c` uploads and downloads pictures with `cuMemcpy2DAsync` (host pictures, usually pinned, to the device pool and back). A CUDA frame from NVDEC reaches the pool through one device-to-device copy in the FFmpeg `libvmaf_cuda` filter ([ADR-1685](../../adr/1685-post-1-0-embedding-zero-copy-milestone.md)); no host copy, but not zero-copy. Linux `dmabuf` / external-memory import (`cuImportExternalMemory`, `cuExternalMemoryGetMappedBuffer`) is not implemented, unlike the SYCL backend (`dmabuf_import.cpp`). The CUDA driver API import needs Vulkan or EGL external handle negotiation (deferred; `docs/state.md`). |
| `float_motion` extra options | open | `motion_add_scale1`, `motion_add_uv` and `motion_filter_size` keep `float_motion` on the CPU. `motion_add_uv=true` is not wired through to the CUDA backend; the `picture_copy()` call in `integer_ms_ssim_cuda.c` passes `0` for the `channel` argument (Y plane only). UV-plane motion on GPU is tracked in [docs/state.md](../../state.md). |
| `psnr_hvs_cuda` throughput | open (tuning) | Bit-exactness costs a 256-byte readback per block and a sequential host sum: 12.2 ms per 3840x2160 frame on an RTX 4090 instead of 2.4 ms. Details on [the psnr_hvs page](../../metrics/psnr-hvs.md#gpu-twins). |
| `float_ssim` at `scale=1` on large pictures | open (cost) | The raster-order sum makes an explicit `scale=1` on 1080p or 4K slower (see the [twin notes](twin-notes.md#float_ssim-adds-its-frame-sums-in-the-cpus-order-adr-1464)). |

## Source layout

```text
core/src/cuda/                  # context, picture, dispatch strategy, drain batch, kernel template
core/src/feature/cuda/          # per-feature extractors and kernels
  <feature>_cuda.{c,h}             # extractor dispatch (host side)
  <feature>/                       # .cu kernels of that feature
```

Extractors with a directory: `float_adm`, `float_motion`, `float_psnr`,
`float_vif`, `integer_adm`, `integer_cambi` (T3-15a, ADR-0360), `integer_ciede`,
`integer_moment`, `integer_motion_v2` (the one motion SAD kernel,
`motion_v2_score.cu`), `integer_ms_ssim`, `integer_psnr`, `integer_psnr_hvs`,
`integer_ssim`, `integer_vif`, `speed`, `ssimulacra2`. Shared host helpers:
`integer_motion_sad_cuda.{c,h}` (motion SAD pipeline shared by `motion` and
`motion_v2`, ADR-1372), `speed_cuda_pipeline.{c,h}` (shared by the two SpEED
twins, ADR-1380), `ssim_cuda.{c,h}`.

Adding a new CUDA extractor: see
[`/add-feature-extractor`](../../../.claude/skills/add-feature-extractor/SKILL.md)
and the [kernel scaffolding template](../kernel-scaffolding.md).

## Design notes

- **Driver API only.** libvmaf links against `cuda.h` via `ffnvcodec` and do not
  depend on the CUDA Runtime API. This keeps libvmaf linkable against FFmpeg
  builds that already load CUDA dynamically.
- **Pinned host staging.** Input pictures are uploaded from
  `cuMemHostAlloc`-pinned buffers. See
  [picture_cuda.c](../../../core/src/cuda/picture_cuda.c).
- **Non-default streams per extractor.** Each feature extractor owns its own
  stream so submit/collect for different features can overlap.
- **Overlapped submit.** Frame N+1 starts uploading while frame N is still
  on the device. The former `ring_buffer.c` is gone; the overlap now comes
  from the per-stream dispatch strategy and event-drain machinery, see
  [`dispatch_strategy.c`](../../../core/src/cuda/dispatch_strategy.c)
  and [`drain_batch.c`](../../../core/src/cuda/drain_batch.c).
- **Shared primary context.** libvmaf retains the device's primary context with
  `cuDevicePrimaryCtxRetain` so FFmpeg and VMAF share one GPU context rather
  than fighting over time-sliced contexts.
- **Engine-scope fence batching (T-GPU-OPT).**
  - Each feature extractor owns a private non-blocking stream and a
      `finished` event for its DtoH readback.
  - The engine collects every frame's pending events in a single
      thread-local drain batch
      ([`src/cuda/drain_batch.c`](../../../core/src/cuda/drain_batch.c))
      and waits on them in one `cuStreamSynchronize(drain_str)` between the
      submit and collect phases.
  - A frame's per-extractor `collect()` calls then become host-side buffer
      reads only: the per-stream sync is short-circuited via
      `vmaf_cuda_kernel_collect_wait`'s `lc->drained` fast path.
  - Participating extractors at time of writing: `psnr_cuda`, `adm_cuda`,
      `vif_cuda`, `ssimulacra2_cuda`, `integer_ms_ssim_cuda` and
      `integer_psnr_hvs_cuda`.
  - MS-SSIM's 5-scale pyramid allocates per-scale partials buffers so all
      DtoH copies can enqueue back-to-back on the same stream
      ([ADR-0271](../../adr/0271-cuda-drain-batch-ms-ssim.md)); PSNR-HVS
      follows the same submit-side readback and `lc.finished` registration
      pattern for its one term buffer (64 floats per block, ADR-1397).
  - Bit-exactness is preserved: same kernels, same stream order, only the
      host wait point moves.
  - `motion_cuda` does not participate in the drain batch (ADR-0845). It
      uses its own per-extractor 8-frame SAD batching
      (`MOTION_BATCH_DEPTH=8`) that amortises `cuStreamSynchronize` over 8
      frames rather than 1.

### Error-path resource release (PR #1279)

Every CUDA feature extractor releases what it has already acquired when `init`
or `submit` fails part-way: pinned host buffers, device buffers, the module and
the stream, in the reverse order of acquisition and each one NULL-guarded.

- The `speed_chroma` / `speed_temporal` twins share one pipeline since
  ADR-1380, and `speed_cuda_pipeline_close()` is its only release path. It
  replaced the `release_cuda_module_and_stream()` helper both twins used to
  share.
- `integer_ms_ssim` and `integer_psnr_hvs` release their pinned buffers on the
  same labels.

There is no user-visible behaviour change: a failed init still returns the
same error code, it just no longer leaks (docs/state.md
`T-CUDA-INIT-SUBMIT-LEAKS-2026-06-19`).

## Profiling

See [nvtx/profiling.md](../nvtx/profiling.md) for Nsight Systems recipes that
rely on the backend's NVTX annotations.

## CUDA version notes

- **`__mul24` / `__umul24` / `__mul24hi`: absent from this codebase (safe).**
  NVIDIA confirmed a silent data-corruption bug in these intrinsics, present
  from CUDA 11.1 and fixed only in CUDA 13.3: `__mul24(val, CONSTANT)` where
  one operand is a compile-time constant may produce incorrect results on
  PTX/SASS generated by CUDA 11.1 to 13.2 (surfaced in the PR #64
  impact-assessment digest, Research-0734).
  - The 2026-05-28 audit of all 78 files under `core/src/feature/cuda/` and
      `core/src/cuda/` found zero uses of these intrinsics. No scores are
      affected.
  - Future kernel authors must not introduce them; see the invariant note in
      `core/src/feature/cuda/AGENTS.md`.

## Licensing of the CUDA kernels (ADR-1250)

A CUDA kernel implementing an upstream Netflix metric carries that metric's
upstream code and keeps its terms (BSD-2-Clause-Patent) plus its copyright
notice; the files ported from the NVIDIA-contributed upstream CUDA extractors
also carry NVIDIA's. Fork-original CUDA code — the runtime, the dispatch layer,
the kernels for fork-only metrics — is EUPL-1.2. The per-file tag is
authoritative; see [ADR-1250](../../adr/1250-eupl-fork-relicense.md).

## References

- [CUDA C++ Best Practices
  Guide](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/)
- [CUDA Driver API Reference](https://docs.nvidia.com/cuda/cuda-driver-api/)
- [CUDA Runtime API Reference](https://docs.nvidia.com/cuda/cuda-runtime-api/)
  (informational — libvmaf itself uses the Driver API)

## See also

- [CUDA twin notes](twin-notes.md): per-twin behaviour, options, checks and
  change history.
- [Kernel scaffolding templates](../kernel-scaffolding.md).
- [NVTX profiling](../nvtx/profiling.md).
- [Backends overview](../index.md).

## Moved sections

Per-twin sections moved to the [twin notes](twin-notes.md). Older links to
these headings land here.

### float_moment_cuda matches the CPU float_moment at 16 bits (2026-10-02)

Moved to the
[twin notes](twin-notes.md#float_moment_cuda-matches-the-cpu-float_moment).

### vif_cuda returns the CPU's scores bit for bit (2026-10-02)

Moved to the
[twin notes](twin-notes.md#vif_cuda-returns-the-cpus-scores-bit-for-bit).

### CAMBI reads its device buffers in one transfer (2026-09-17)

Superseded; the entry is in the
[twin notes history](twin-notes.md#2026-09-17-cambi-reads-its-device-buffers-in-one-transfer-superseded).
