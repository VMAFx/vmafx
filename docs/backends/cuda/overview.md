<!-- markdownlint-disable MD013 MD060 -->
# CUDA Backend

The CUDA backend runs VMAF's core feature extractors (VIF, ADM, Motion)
directly on an NVIDIA GPU, keeping frames on the device across the full
pipeline to avoid PCIe round-trips.

## Build

```bash
meson setup build -Denable_cuda=true
ninja -C build
```

Requires the CUDA toolkit (`nvcc`, driver API headers). The build uses the
driver API only — through `ffnvcodec` dynlink wrappers — so applications
that already load CUDA through FFmpeg share the same primary context.

Meson options:

- `-Denable_cuda=true` — compile the CUDA backend + kernels.
- `-Denable_nvtx=true` — instrument kernels with NVTX ranges (see [nvtx/profiling.md](../nvtx/profiling.md)).
- `-Denable_nvcc=true` — build NVCC-compiled kernel objects (default when `enable_cuda` is on).

### GPU architecture coverage

The fork ships cubins for every currently-shipping consumer Nvidia
generation from Turing through Blackwell whenever the host `nvcc`
supports them, plus a `compute_80` PTX as an unconditional JIT
fallback:

| Generation | Arch     | Emitted as      | Host `nvcc` gate |
| ---------- | -------- | --------------- | ---------------- |
| Turing     | `sm_75`  | cubin           | always           |
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
at Txx major boundaries; see [ADR-0122](../../adr/0122-cuda-gencode-coverage-and-init-hardening.md).

## Runtime requirements

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

Statically-linked consumers (for example, ffmpeg binaries built with
`--enable-libvmaf` in static mode) are **not** exempt: the driver
library is loaded through `dlopen`, which bypasses `DT_NEEDED` and
therefore does not show up in `ldd <binary>`. An otherwise
self-contained static ffmpeg will still fail on the first frame if
`libcuda.so.1` is not on the loader path.

## Runtime

When the binary is built with CUDA, the backend is auto-selected on GPU-capable
hosts. CLI controls:

```bash
./build/tools/vmaf ...            # CUDA used automatically
./build/tools/vmaf --no_cuda ...  # force CPU path
```

The FFmpeg filter name is `libvmaf_cuda` — see [usage/ffmpeg.md](../../usage/ffmpeg.md)
for a hwaccel pipeline that keeps decoded frames on the GPU. For
software-decoded input the regular `libvmaf` filter accepts a
fork-added `cuda=1` AVOption (per
[ADR-0350](../../adr/0350-ffmpeg-libvmaf-cuda-backend-selector.md));
build FFmpeg with `--enable-libvmaf-cuda` to enable it.

## Source layout

```text
core/src/cuda/                # queue, picture, ring-buffer runtime
core/src/feature/cuda/        # per-feature kernels
  integer_vif_cuda.{c,h}         # VIF extractor dispatch
  integer_vif/                   # VIF .cu kernels
  integer_adm_cuda.{c,h}         # ADM extractor dispatch
  integer_adm/                   # ADM .cu kernels
  float_adm_cuda.{c,h}           # float ADM extractor dispatch (ADR-0202)
  float_adm/                     # float ADM .cu kernels (single fatbin compiled with --fmad=false)
  integer_motion_cuda.{c,h}      # Motion extractor dispatch
  integer_motion/                # Motion .cu kernels
  integer_cambi_cuda.{c,h}      # CAMBI extractor dispatch (T3-15a / ADR-0360)
  integer_cambi/                 # CAMBI .cu kernels (cambi_score.cu)
```

Adding a new CUDA extractor: see [`/add-feature-extractor`](../../../.claude/skills/add-feature-extractor/SKILL.md).

## Design notes

- **Driver API only.** We link against `cuda.h` via `ffnvcodec` and do not
  depend on the CUDA Runtime API. This keeps libvmaf linkable against FFmpeg
  builds that already load CUDA dynamically.
- **Pinned host staging.** Input pictures are uploaded from
  `cuMemHostAlloc`-pinned buffers. See [picture_cuda.c](../../../core/src/cuda/picture_cuda.c).
- **Non-default streams per extractor.** Each feature extractor owns its own
  stream so submit/collect for different features can overlap.
- **Ring-buffered double-buffer submit.** Frame N+1 starts uploading while
  frame N is still on the device. The legacy `ring_buffer.c` was folded
  into the per-stream dispatch strategy and event-drain machinery — see
  [`dispatch_strategy.c`](../../../core/src/cuda/dispatch_strategy.c)
  and [`drain_batch.c`](../../../core/src/cuda/drain_batch.c).
- **Shared primary context.** We retain the device's primary context with
  `cuDevicePrimaryCtxRetain` so FFmpeg and VMAF share one GPU context rather
  than fighting over time-sliced contexts.
- **Engine-scope fence batching (T-GPU-OPT).** Each feature extractor owns
  a private non-blocking stream + a `finished` event for its DtoH readback;
  the engine collects every frame's pending events in a single thread-local
  drain batch
  ([`src/cuda/drain_batch.c`](../../../core/src/cuda/drain_batch.c))
  and waits on them in one `cuStreamSynchronize(drain_str)` between submit
  and collect phases. A frame's per-extractor `collect()` calls then become
  host-side buffer reads only — the per-stream sync is short-circuited via
  `vmaf_cuda_kernel_collect_wait`'s `lc->drained` fast path. Participating
  extractors at time of writing: `psnr_cuda`, `adm_cuda`,
  `vif_cuda`, `ssimulacra2_cuda`, `integer_ms_ssim_cuda`, and
  `integer_psnr_hvs_cuda`. MS-SSIM's 5-scale pyramid required allocating
  per-scale partials buffers so all DtoH copies could enqueue back-to-back
  on the same stream ([ADR-0271](../../adr/0271-cuda-drain-batch-ms-ssim.md));
  PSNR-HVS follows the same submit-side readback + `lc.finished` registration
  pattern for its three plane partial buffers.
  Bit-exactness is preserved (same kernels, same stream order — only the
  host wait point moves).
  **Note**: `motion_cuda` no longer participates in the drain batch (ADR-0845).
  It uses its own per-extractor 8-frame SAD batching (`MOTION_BATCH_DEPTH=8`)
  that amortises `cuStreamSynchronize` over 8 frames rather than 1, superseding
  the engine-level optimization for this extractor.

### Error-path resource release (PR #1279)

Every CUDA feature extractor releases what it has already acquired when `init`
or `submit` fails part-way: pinned host buffers, device buffers, the module and
the stream, in the reverse order of acquisition and each one NULL-guarded. The
`speed_chroma` / `speed_temporal` twins share one `release_cuda_module_and_stream()`
helper so the two failure labels cannot drift apart again (PR #1007 added the
calls inline and PR #1029 removed them the same day); `integer_ms_ssim` and
`integer_psnr_hvs` release their pinned buffers on the same labels. There is no
user-visible behaviour change: a failed init still returns the same error code,
it just no longer leaks (docs/state.md `T-CUDA-INIT-SUBMIT-LEAKS-2026-06-19`).

## Profiling

See [nvtx/profiling.md](../nvtx/profiling.md) for Nsight Systems recipes that
rely on the backend's NVTX annotations.

## Numerical tolerance vs the CPU scalar path

CUDA kernels target **close agreement** with the CPU fixed-point path,
not bit-exact equality. Different reduction orders, FMA contractions,
and parallel-prefix sums can perturb the final integer accumulator by a
fraction of a ULP — this has always been the case for VMAF on GPU,
not a fork regression. In practice the per-frame pooled VMAF agrees
to ~6 decimal places (the default `%.6f` truncation hides the delta
entirely; `--precision=max` exposes it). See
[ADR-0119](../../adr/0119-cli-precision-default-revert.md) for the
precision-default rationale.

The `motion` / `motion2` / `motion3` CUDA outputs in particular are
verified bit-exact against the CPU fixed-point path at `places = 4`
under default settings on the Netflix `src01_hrc00_576x324.yuv` ↔
`src01_hrc01_576x324.yuv` pair (0 / 144 mismatches, max_abs =
0.00e+00). Equivalent parity holds under the non-default
`motion_fps_weight ≠ 1.0` and `motion_moving_average = true` paths
after [ADR-0358](../../adr/0358-cuda-motion-race-and-precision-fixes.md)
fixed the host-side post-processing: `motion2_score` now applies
`MIN(score * motion_fps_weight, motion_max_val)` mirroring the CPU
reference at `integer_motion.c:563`, and the moving-average guard in
`motion3_postprocess_cuda` now skips averaging at framework-collect
index 1 to match `integer_motion.c:523`'s `index >
minimum_past_frames_needed` rule.

The **GPU SpEED** extractors (`speed_chroma` / `speed_temporal`) are
verified bit-parity against the CPU reference at `places = 4` (≤ 1e-4)
on an RTX 4090 via `test_cuda_speed_chroma_parity` /
`test_cuda_speed_temporal_parity`. This is a **score correction**, not
just a tolerance statement: before the fix the GPU SpEED kernels
computed a per-tile block-local covariance (instead of the CPU's single
global covariance over the 5×5-phase-shifted submatrix) and reused the
reference eigenvalue basis for the distorted path, so GPU SpEED scores
were ~7× low (and chroma ~2× high on the distorted path). They now match
the CPU output. If you previously recorded GPU SpEED numbers, re-extract
them. See
[research-1120](../../research/1120-gpu-speed-covariance-eigenbasis-correctness-2026-06-20.md).
The same correction was ported to the HIP and SYCL SpEED twins.

The **Netflix golden-data gate is CPU-only** — the three reference
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

## Backend dispatch knob

`VMAF_CUDA_DISPATCH` controls how CUDA feature extractors batch and synchronise
work.  Three values are accepted:

| Value | Behaviour |
|---|---|
| `adaptive` | Runtime heuristic: selects `batched` above 720p frame area, `serial` below. **Default** when unset. |
| `batched` | Drain-batch: enqueues all per-extractor events, waits once per frame (ADR-0483). Lowest latency at ≥ 1080p. |
| `serial` | Synchronises after each extractor. Lower overhead at small resolutions. |

Per-feature overrides use the `feature=strategy[,...]` syntax:

```bash
VMAF_CUDA_DISPATCH=adaptive,integer_vif=batched ./build/tools/vmaf ...
```

See [ADR-0483](../../adr/0483-gpu-dispatch-parse-dedup.md) for the full parse
grammar and the [env-var reference](../../usage/env-vars.md#cuda-dispatch-knob)
for the complete table.

## Known gaps

### Default model feature dispatch (`vmaf_v1.0.16_3d0h`)

When running the default model `vmaf_v1.0.16_3d0h` under `--backend cuda`, features are
selectively dispatched between GPU and CPU based on option support ([ADR-1183](../../adr/1183-model-options-gate-gpu-twin-selection.md)):

- **CAMBI** (`cambi_cuda`) and **SpEED** (`speed_chroma_cuda`) run directly on the GPU.
- **Motion** (`integer_motion_cuda`) computes motion scores on device while honoring `motion_max_val`.
- **ADM** (`integer_adm_cuda`) lacks support for `adm_csf_mode: 2` (required by `vmaf_v1.0.16_3d0h`),
  so libvmaf automatically dispatches ADM to the CPU reference extractor with an informational
  notice (`adm_cuda extractor lacks option 'adm_csf_mode', computing it on the CPU`). This ensures
  complete numerical correctness without breaking model evaluation until `adm_csf_mode` is ported
  to CUDA device kernels (`T-GPU-ADM-CSF-MODE-NOT-PORTED-2026-09-05`).

- **CIEDE2000** — no CUDA kernel (same CPU-fallback behaviour).
- **PSNR** — `psnr_cuda` ships with the full luma + chroma set
  (`psnr_y`, `psnr_cb`, `psnr_cr`); luma landed in
  [ADR-0182](../../adr/0182-gpu-long-tail-batch-1.md) batch 1b,
  chroma in [ADR-0351](../../adr/0351-cuda-chroma-psnr.md) (T3-15(b)).
  YUV400P clamps to luma-only at runtime. Cross-backend gate vs CPU
  is bit-exact (`max_abs_diff = 0.0` at `places=4` on the 576×324 +
  640×480 testdata fixtures, RTX 4090, 8-bit 4:2:0).
- **SSIM / MS-SSIM / PSNR-HVS** — SSIM, MS-SSIM, and PSNR-HVS have
  CUDA kernels and participate in the cross-backend parity gate
  (`psnr_hvs` uses the relaxed DCT/reduction tolerance from
  [ADR-0191](../../adr/0191-psnr-hvs-vulkan.md) /
  [ADR-0214](../../adr/0214-gpu-parity-ci-gate.md)). The CUDA
  `float_ansnr` extractor was removed together with its CPU twin in
  [ADR-0709](../../adr/0709-vmafx-phase4b-distributed-platform.md)
  (PR #38); ANSNR is no longer dispatched on any backend.
- **Float-twin extractors (`float_*`)** — the CUDA backend
  implements the float twins for PSNR / Motion / VIF / ADM
  ([ADR-0202](../../adr/0202-float-adm-cuda-sycl.md)). Requesting
  `--feature float_<x>` with `--no_cuda=false` dispatches to GPU
  for those metrics.
- **`float_motion` extra options (`motion_add_scale1`,
  `motion_add_uv`, `motion_filter_size`, `motion_max_val`,
  `motion3_score`)** — these were added to the CPU `float_motion`
  extractor by the upstream port from Netflix/vmaf
  [`b949cebf`](https://github.com/Netflix/vmaf/commit/b949cebf)
  (2026-04-29). As of T3-15(c) /
  [ADR-0219](../../adr/0219-motion3-gpu-coverage.md), the
  `integer_motion_cuda` kernel emits `motion3_score` in 3-frame
  window mode via host-side `motion_blend()` post-processing of
  `motion2_score`; the full options surface
  (`motion_blend_factor`, `motion_blend_offset`, `motion_fps_weight`,
  `motion_max_val`, `motion_moving_average`) is exposed.
  `motion_five_frame_window=true` is rejected with `-ENOTSUP` at
  `init()` (the 5-deep blur ring is still deferred). The
  `motion_add_uv=true` path is independent from motion3 and
  remains **not yet wired through to the CUDA backend**. The CUDA
  `picture_copy()` callsite at
  [`src/feature/cuda/integer_ms_ssim_cuda.c`](../../../core/src/feature/cuda/integer_ms_ssim_cuda.c)
  passes `0` for the new trailing `channel` argument (Y-plane only,
  preserving CUDA pre-port behaviour). UV-plane motion on GPU is a
  follow-up tracked in [docs/state.md](../../state.md).
- **`psnr_hvs_cuda` DCT scheduling** — the backend keeps the
  established places=3 cross-backend contract by leaving the float
  means, variances, masking, and masked-error accumulation in
  thread-0 CPU scan order. The 8×8 integer DCT itself is parallelised
  across the first eight CUDA threads inside each block; this is a
  scheduling optimisation only and does not change emitted feature
  names or CLI/API usage.
- **SSIMULACRA 2** — `ssimulacra2_cuda` shipped per
  [ADR-0206](../../adr/0206-ssimulacra2-cuda-sycl.md) (hybrid
  host/GPU pipeline, IIR fatbin pinned with `--fmad=false`). The
  2026-05-09 cuda-reviewer pass tightened the lifecycle path
  (paired `cuModuleUnload` for both PTX modules, pre-allocated
  pinned downsample scratch in place of a per-scale `malloc`,
  per-plane H2D/D2H byte counts shrunk to the valid sub-region,
  `__launch_bounds__(64, 32)` on the blur kernels) — see
  [ADR-0356](../../adr/0410-ssimulacra2-cuda-leaks-perf.md). The
  H-pass non-coalesced reads and V-pass L1 pressure remain known
  architectural ceilings (require a shared-memory tile-transpose
  rewrite).
- **CUDA graph capture dispatch fallback** — While `VMAF_CUDA_DISPATCH` parses
  the `graph` strategy token, CUDA graph capture (`cuGraphCreate`, `cuGraphLaunch`)
  is not implemented for the CUDA feature extractors. Setting `VMAF_CUDA_DISPATCH=graph`
  (or `feature=graph`) logs a warning (`libvmaf: CUDA graph dispatch requested for '%s'
  but graph capture is not implemented; falling back to direct`) and safely falls
  back to `VMAF_CUDA_DISPATCH_DIRECT`. Graph capture requires static pitch allocation
  and graph instance lifecycle management across frames (deferred; see `docs/state.md`).
- **Zero-copy picture import** — `core/src/cuda/picture_cuda.c` stages picture data via
  host-staged pinned memory transfers (`cuMemcpyHtoDAsync` / `cuMemcpyDtoHAsync`).
  Linux `dmabuf` / external memory zero-copy import (via `cuImportExternalMemory` /
  `cuExternalMemoryGetMappedBuffer`) is not implemented on CUDA. Unlike the SYCL
  backend which supports VA-API / Linux dmabuf zero-copy import (`dmabuf_import.cpp`),
  CUDA driver API dmabuf import requires Vulkan/EGL external handle negotiation and
  is currently deferred (see `docs/state.md`).
- **HIP / AMD** — separate backend; 19 registered feature extractors +
  3 unregistered legacy stubs. See
  [backends/hip/overview.md](../hip/overview.md) for details.

See [metrics/features.md](../../metrics/features.md) for the
per-extractor coverage matrix.

## CUDA version notes

- **`__mul24` / `__umul24` / `__mul24hi` — absent from this codebase (safe).**
  NVIDIA confirmed a silent data-corruption bug in these intrinsics present from
  CUDA 11.1 and fixed only in CUDA 13.3: `__mul24(val, CONSTANT)` where one
  operand is a compile-time constant may produce incorrect results on PTX/SASS
  generated by CUDA 11.1–13.2 (surfaced in PR #64 impact-assessment digest,
  Research-0734). The 2026-05-28 audit of all 78 files under
  `core/src/feature/cuda/` and `core/src/cuda/` found **zero** uses of these
  intrinsics. No scores are affected. Future kernel authors are prohibited from
  introducing these intrinsics; see the invariant note in
  `core/src/feature/cuda/AGENTS.md`.

- **Integer SSIM `extern "C"` sweep (fixed, ADR-0747)** — A full audit
  of all 24 `.cu` kernel files confirmed that `integer_ssim/integer_ssim_score.cu`
  was the only file with `__global__` kernels referenced by
  `cuModuleGetFunction` but not wrapped in `extern "C"`.  This caused
  `--feature ssim --backend cuda` to silently return `-EINVAL` from
  `init_fex_cuda` (the driver returned `CUDA_ERROR_NOT_FOUND` for all
  three kernel names) since the file was introduced.  Fixed in this PR by
  wrapping the three entry points in `extern "C" { }`.  A CI script
  (`scripts/dev/check-cuda-extern-c.sh`) prevents recurrence.
  The analogous bug in `ssim_score.cu` was fixed earlier in PR #77.

## References

- [CUDA C++ Best Practices Guide](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/)
- [CUDA Driver API Reference](https://docs.nvidia.com/cuda/cuda-driver-api/)
- [CUDA Runtime API Reference](https://docs.nvidia.com/cuda/cuda-runtime-api/) (informational — libvmaf itself uses the Driver API)

## VIF filter1d horizontal kernel performance (ADR-0743, 2026-05-28)

`filter1d_8_horizontal_kernel_2_17_9` is the scale-0 8-bit 17-tap horizontal
convolution pass and accounts for 35.3% of VIF self-time on RTX 4090 / CUDA 13.3.

Two ncu-driven optimizations were applied:

1. **`__launch_bounds__(128, 10)`** on the kernel: reduces registers 56 → 48
   per thread on sm_89 (RTX 4090), lifting theoretical occupancy 75% → 83.3%.
   At production resolutions (≥ 1080p) the higher block count per SM improves
   latency hiding. At 576×324 the workload is wave-limited (< 1 wave / 128 SMs)
   and the gain is invisible in achieved occupancy but causes no regression.

2. **`__ldg()` on the 7 read-only tmp-channel loads** in the smem-fill phase:
   routes these loads through the read-only L1 (texture) cache. Beneficial at
   ≥ 1080p where the combined tmp footprint (7 channels × stride × height) exceeds
   the 50 MB L2 capacity.

`val_per_thread=4` was evaluated but rejected: smem grows 7644 → 14812 B/block,
making the kernel smem-limited at 37.5% occupancy vs 62.5% for the retained
vpt=2 path.

Correctness: CUDA-optimized scores agree with the CPU reference within
ADR-0214 places=4 tolerance (max absolute delta: 0.000010 per frame).

ncu reproducer (see research digest):

```bash
ncu -k 'filter1d_8_horizontal_kernel_2_17_9' --set basic --csv \
    build/tools/vmaf -r ref.yuv -d dis.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 --backend cuda
```

See [ADR-0743](../../adr/0743-cuda-vif-filter1d-ncu-driven-perf.md) and
[Research-0743](../../research/research-0743-cuda-vif-filter1d-perf-impl.md).

## SSIM vert_combine kernel performance (ADR-0754, 2026-05-29)

`calculate_ssim_vert_combine` is the pass-2 (vertical 11-tap + SSIM combine)
kernel in the `float_ssim_cuda` extractor. Three optimizations applied in
[ADR-0754](../../adr/0754-cuda-ssim-vert-combine-ldg-pinned-leak.md):

1. **`__launch_bounds__(128)`** — constrains register budget to the actual
   128-thread (16×8) launch configuration. Minimum-form hint with no
   `min_blocks` argument (conservative, zero risk of regression).

2. **`__ldg()` on the 5×11 = 55 inner-loop loads** — the five intermediate
   float buffers (h_ref_mu, h_cmp_mu, h_ref_sq, h_cmp_sq, h_refcmp) are
   written once by the horizontal pass and never aliased in the vertical
   pass. Extracting `const float *__restrict__` pointers from the
   `VmafCudaBuffer` struct arguments before the inner loop makes the
   alias-free invariant visible to the compiler, enabling `__ldg()` to
   route all 55 loads through the L1 read-only cache. Expected benefit at
   ≥ 1080p where the combined 5-plane footprint exceeds L2 capacity.

3. **Pinned-host memory leak fix** — `vmaf_cuda_kernel_readback_free` NULLs
   `rb->host_pinned` but does not free it (documented in the template as
   caller responsibility). `close_fex_cuda` now saves the pointer before
   calling `readback_free` and calls `vmaf_cuda_buffer_host_free` afterward.
   Verified with `compute-sanitizer --tool memcheck --leak-check full`.

Live ncu A/B numbers pending; static analysis predicts behaviour analogous
to the VIF filter1d `__ldg()` pattern at 1080p+.

ncu reproducer:

```bash
ncu --kernel-name calculate_ssim_vert_combine \
    --section MemoryWorkloadAnalysis --section LaunchStats \
    build/tools/vmaf \
      --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
      --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
      --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
      --feature float_ssim --backend cuda
```
