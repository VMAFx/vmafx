<!-- markdownlint-disable MD013 MD060 -->
# AGENTS.md — core/src/feature/cuda

Orientation for agents working on per-feature CUDA kernels (host
glue + `.cu` device code). Parent: [../AGENTS.md](../AGENTS.md). The
backend runtime (context, stream, picture-pool) lives one level up
in [`../../cuda/AGENTS.md`](../../cuda/AGENTS.md).

## Deleted orphan TU (ADR-0546)

`float_ssim_cuda.c` was removed by ADR-0546 (`chore/hip-cuda-orphan-tu-cleanup`,
2026-05-18). It defined `vmaf_fex_float_ssim_cuda` but was not listed in
`core/src/meson.build`; `integer_ssim_cuda.c` (which is compiled) also
defines the same symbol and is the current canonical TU (it adds the
`enable_chroma` option and other improvements absent from the orphan copy).
Do not re-add `float_ssim_cuda.c` without consulting ADR-0546.

## Scope

```text
feature/cuda/
  <feature>_cuda.{c,h}        # host glue: registration, submit/collect, kernel-template wiring
  <feature>/                  # subdirectory of `.cu` device code (where the host glue is non-trivial)
    *.cu                      # CUDA kernel TUs (compiled with nvcc)
    *.cuh                     # device-side helpers (included from .cu only)
```

Examples: `integer_psnr_cuda.c` is a single-file consumer using the
kernel-template flat shape; `integer_adm/` is a multi-`.cu` consumer
because ADM splits across DWT2 + decouple + CSF + CM passes.

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md) +
  [../../AGENTS.md](../../AGENTS.md) +
  [`../../cuda/AGENTS.md`](../../cuda/AGENTS.md)).
- **Wholly-new fork files use the dual Netflix + Lusoris/Claude
  copyright header** per [ADR-0025](../../../../docs/adr/0025-copyright-handling-dual-notice.md).
  Many TUs here predate the dual-notice rule and carry only the
  Netflix header (with NVIDIA contributor lines on the `integer_adm/`
  CUDA kernels) — that is correct for upstream-mirrored files; do
  not retro-fit.
- **`#include` order** mirrors the SYCL / Vulkan twins:
  `feature_collector.h` / `feature_extractor.h` first, then
  `cuda/integer_<feature>_cuda.h`, then `cuda_helper.cuh` /
  `kernel_template.h`. Don't shuffle.
- **fmaf contraction is OFF for precision-critical kernels.** The
  parent build line passes `--fmad=false` to `nvcc` for feature
  TUs that participate in cross-backend gates with `places=4`.
  Removing it drifts `float_adm_cuda` /  `ssimulacra2_cuda` past
  the gate (mirror of the SYCL `-fp-model=precise` and Vulkan
  GLSL `precise` / `NoContraction` rules). On rebase: keep the
  flag.

## Twin-update rules

Every TU in this directory has at least one cross-backend twin.
A change to one twin **must** ship with the matching change(s) in
the same PR:

| Feature | Twins |
| --- | --- |
| **psnr** | `integer_psnr_cuda.c` ↔ `../sycl/integer_psnr_sycl.cpp` ↔ `../vulkan/psnr_vulkan.c` (+ `psnr.comp`) ↔ `../hip/integer_psnr_hip.c` |
| **ciede** | `integer_ciede_cuda.c` ↔ `../sycl/integer_ciede_sycl.cpp` ↔ `../vulkan/ciede_vulkan.c` (+ `ciede.comp`) ↔ `../hip/ciede_hip.c` |
| **moment** | `integer_moment_cuda.c` ↔ `../sycl/integer_moment_sycl.cpp` ↔ `../vulkan/moment_vulkan.c` (+ `moment.comp`) ↔ `../hip/float_moment_hip.c` |
| **motion** | `integer_motion_cuda.c` ↔ `../sycl/integer_motion_sycl.cpp` ↔ `../vulkan/motion_vulkan.c` (+ `motion.comp`) |
| **motion_v2** | `integer_motion_v2_cuda.c` ↔ `../sycl/integer_motion_v2_sycl.cpp` ↔ `../vulkan/motion_v2_vulkan.c` (+ `motion_v2.comp`) ↔ `../hip/integer_motion_v2_hip.c` |
| **vif (integer)** | `integer_vif_cuda.c` (+ `integer_vif/filter1d.cu`) ↔ `../sycl/integer_vif_sycl.cpp` ↔ `../vulkan/vif_vulkan.c` (+ `vif.comp`) |
| **adm (integer)** | `integer_adm_cuda.c` (+ `integer_adm/*.cu`) ↔ `../sycl/integer_adm_sycl.cpp` ↔ `../vulkan/adm_vulkan.c` (+ `adm.comp`) |
| **ssim (float)** | `integer_ssim_cuda.c` (misnomer; provides `"float_ssim"` — 11-tap float Gaussian) ↔ `../sycl/integer_ssim_sycl.cpp` (float_ssim part) ↔ `../vulkan/ssim_vulkan.c` (+ `ssim.comp`) |
| **ssim (integer)** | `ssim_cuda.c` (real integer_ssim; provides `"ssim"` — 9-tap int64) ↔ `../hip/integer_ssim_hip.c` ↔ `../sycl/integer_ssim_sycl.cpp` (integer_ssim part) — Vulkan integer_ssim pending (ADR-0564) |
| **ms_ssim** | `integer_ms_ssim_cuda.c` ↔ `../sycl/integer_ms_ssim_sycl.cpp` ↔ `../vulkan/ms_ssim_vulkan.c` (+ `ms_ssim.comp`) |
| **psnr_hvs** | `integer_psnr_hvs_cuda.c` ↔ `../sycl/integer_psnr_hvs_sycl.cpp` ↔ `../vulkan/psnr_hvs_vulkan.c` (+ `psnr_hvs.comp`) |
| **ssimulacra2** | `ssimulacra2_cuda.c` (+ `ssimulacra2/*.cu`) ↔ `../sycl/ssimulacra2_sycl.cpp` ↔ `../vulkan/ssimulacra2_vulkan.c` (+ `ssimulacra2_*.comp`) |
| **float_*** | `float_adm_cuda.c` / `float_motion_cuda.c` / `float_psnr_cuda.c` / `float_vif_cuda.c` ↔ matching `../sycl/float_*_sycl.cpp` ↔ partial `../hip/float_*_hip.c` (`float_ansnr_cuda.c` and its twins removed in commit 70ed8b3ce3 / PR #38) |
| **cambi** | `integer_cambi_cuda.c` (+ `integer_cambi/cambi_score.cu`) ↔ `../vulkan/cambi_vulkan.c` (+ `cambi_*.comp`) — Strategy II hybrid twin. SYCL twin pending (T3-15b). |

The full GPU twin matrix is governed by the GPU long-tail batches:
[ADR-0182](../../../../docs/adr/0182-gpu-long-tail-batch-1.md) (psnr /
ciede / moment), [ADR-0188](../../../../docs/adr/0188-gpu-long-tail-batch-2.md)
(ssim / ms_ssim / psnr_hvs), [ADR-0192](../../../../docs/adr/0192-gpu-long-tail-batch-3.md)
(motion_v2 / float-twins / ssimulacra2 / cambi; `float_ansnr` removed
in commit 70ed8b3ce3 / PR #38).

## Parity invariant — motion3 CPU and CUDA moving-average paths

`integer_motion.c` (CPU) and `integer_motion_cuda.c` (CUDA) both implement
the motion3 post-process as a host-side moving average over blended motion2
scores. These two paths **must stay in numerical parity at places=4** (delta
≤ 1e-4, per ADR-0214). The gate is enforced by
`core/test/test_cuda_motion3_parity.c`; any change to the blend formula
(`motion_blend()`), the moving-average guard condition, or `motion_max_val`
clipping must be mirrored across both files (and across the SYCL / Vulkan /
HIP / Metal motion twins listed in the Twin-update table below) in the same PR.

## Rebase-sensitive invariants

- **`vmaf_cuda_kernel_readback_free` owns the pinned-host free
  (2026-05-29 sweep).** The helper in `core/src/cuda/kernel_template.h`
  calls `vmaf_cuda_buffer_host_free(cu_state, rb->host_pinned)` before
  NULLing the pointer. Callers of `vmaf_cuda_kernel_readback_free` must
  NOT also call `vmaf_cuda_buffer_host_free` on `rb->host_pinned` — doing
  so would double-free the pinned allocation. The pre-2026-05-29 pattern
  where callers called `vmaf_cuda_buffer_host_free` explicitly is
  incorrect; the helper now owns the free. See
  PR fix/cuda-pinned-host-leak-sweep-20260529.

- **`integer_ms_ssim_cuda.c` honours the `enable_lcs`, `enable_db`,
  and `clip_db` GPU contracts** (ADR-0243, ADR-0460). Emits 15 extra
  metrics (`float_ms_ssim_{l,c,s}_scale{0..4}`) when `enable_lcs=true`,
  all `l_scale*` first then `c_*` then `s_*` (metric ordering is
  public API; renaming or reordering breaks the cross-backend parity
  gate). Returns dB-domain score (`-10*log10(1-ms_ssim)`) when
  `enable_db=true`, optionally clipping via `clip_db`. See
  [../../AGENTS.md §"MS-SSIM `enable_lcs` GPU
  contract"](../../AGENTS.md).

- **`integer_motion_cuda.c::motion3_postprocess_*` honours the
  motion3 GPU contract** (ADR-0219). Applies CPU's host-side
  post-process to motion2 with no device-side state. Two
  invariants flow: (1) `motion_five_frame_window=true` returns
  `-ENOTSUP` at `init()`; (2) any change to `motion_blend()` /
  `motion_max_val` / moving-average must mirror across the three
  GPU motion twins in the same PR. See [../../AGENTS.md §"motion3_score
  GPU contract"](../../AGENTS.md).

- **`integer_motion_cuda.c::submit_fex_cuda` runs the SAD
  `cuMemsetD8Async` on `pic_stream`, NOT on `s->str`** (ADR-0358).
  The kernel atomic-adds into the same single-int64 buffer on
  `pic_stream`; both streams are `CU_STREAM_NON_BLOCKING` and have
  no event linking them, so co-locating the memset on the same
  stream as the kernel is the only thing that orders them. The
  matching pattern lives at `integer_motion_v2_cuda.c:188`. Any
  rebase or follow-up that reverts the memset onto a separate
  stream silently re-introduces the cross-stream race.

- **`integer_motion_cuda.c::collect_fex_cuda` and `flush_fex_cuda`
  emit `motion2_score = MIN(score * motion_fps_weight, motion_max_val)`,
  NOT the raw `min(prev, cur)` SAD score** (ADR-0358). Mirrors the
  CPU reference at `integer_motion.c:563`. The
  `motion3_postprocess_cuda` moving-average guard reads
  `frame_index > 2` (NOT `> 1`) because `frame_index` is
  pre-incremented in `collect()` before the helper runs. Tripped
  by non-default `motion_fps_weight ≠ 1.0` /
  `motion_moving_average = true`.

- **`integer_ms_ssim_cuda.c` and `integer_ssim_cuda.c` pass
  `channel=0` to `picture_copy()`** per the upstream
  d3647c73 prerequisite port. If a future upstream commit
  evolves the signature further, update these call sites in
  lockstep with the upstream-mirror callers (`float_*` series).
  See [../../AGENTS.md §"`picture_copy()` carries a `channel`
  parameter"](../../AGENTS.md).

- **`integer_psnr_hvs_cuda.c` participates in the engine-scope CUDA
  drain batch.** Its `submit_fex_cuda` queues all three plane partial
  DtoH copies on `s->lc.str`, records `s->lc.finished` via
  `vmaf_cuda_kernel_submit_post_record`, and registers the lifecycle
  with `drain_batch`. Its `collect_fex_cuda` must call
  `vmaf_cuda_kernel_collect_wait` before reading `h_partials[]`;
  reintroducing raw `cuMemcpyDtoHAsync` + `cuStreamSynchronize` in
  collect reopens T-GPU-OPT-3's per-frame sync stall. The scheduling
  change is CUDA-only and does not require SYCL / Vulkan twin edits
  because it does not alter kernel math or emitted metrics.

- **`integer_psnr_hvs_cuda.c` honours `enable_chroma` option parity** (mirrors
  ADR-0453 on the psnr_hvs surface). The `enable_chroma` option (default
  `false`) clamps `n_planes` to 1 in `init_fex_cuda` when set to `false`,
  and YUV400P sources always force `n_planes=1` regardless of the option.
  All plane loops (`upload_frame`, `launch_plane_kernels`,
  `enqueue_partials_readback`, `collect_fex_cuda`, `close_fex_cuda`) iterate
  over `s->n_planes`, not the compile-time constant `PSNR_HVS_NUM_PLANES`.
  The Vulkan and SYCL twins do not yet carry this option; add it there in
  lockstep if the combined-score formula diverges. The `collect_fex_cuda`
  combined-score path emits luma dB only when `n_planes == 1`.

- **`integer_psnr_hvs/psnr_hvs_score.cu` parallelises only the integer
  DCT passes.** The first eight CUDA threads perform the two 8-point
  DCT passes over shared memory; all float means, variance, masking,
  and final masked-error accumulation remain thread-0 serial in CPU
  scan order. Do not move the float reductions into warp or block
  reductions without a new numeric-contract ADR and a refreshed
  cross-backend tolerance row.

- **`integer_psnr_hvs/psnr_hvs_score.cu` uses `__ldg()` for tile loads**
  (ADR-0764). `const float *__restrict__ ref_buf` and `dist_buf` are
  extracted from the `VmafCudaBuffer` struct arguments once before the
  cooperative tile load; `__ldg(&ref_buf[src_idx])` and
  `__ldg(&dist_buf[src_idx])` route the 128 per-block reads through
  the L1 read-only texture cache. `__launch_bounds__(64)` is set to
  match the actual 8x8 block dispatch. Do not revert to plain
  `ref_buf[src_idx]` or pass VmafCudaBuffer by-value reads without
  first verifying the compiler still emits LDG.E.CONSTANT in SASS
  (via `cuobjdump --dump-sass`).

- **`integer_cambi_cuda.c` + `integer_cambi/cambi_score.cu` are
  Strategy II hybrid** (ADR-0360 / T3-15a). The GPU kernels
  (`cambi_spatial_mask_kernel`, `cambi_decimate_kernel`,
  `cambi_filter_mode_kernel`) are bit-exact w.r.t. the CPU
  implementation. The host residual calls `vmaf_cambi_calculate_c_values`
  and `vmaf_cambi_spatial_pooling` via `cambi_internal.h`. If upstream
  Netflix refactors `cambi.c` and renames those entry points,
  `cambi_internal.h` **and** `cambi_vulkan.c` must be updated in the
  same PR. Never remove the `cuStreamSynchronize` calls inside
  `submit_fex_cuda` — they guard the DtoH coherency for the host
  residual. `places=4` gate is load-bearing; do not loosen it.

- **`cuLaunchKernel` `kernelParams[]` must point to the device-pointer
  VALUE, not to a `VmafCudaBuffer` struct** (Issue #857 / fix PR). The
  dispatch helpers in `integer_cambi_cuda.c` (`dispatch_mask`,
  `dispatch_decimate`, `dispatch_filter_mode`) pass `&buf->data`
  (address of the `CUdeviceptr` field) to `cuLaunchKernel`. Passing
  `(void *)buf` (address of the struct) makes the driver read
  `buf->size` (a host byte count) as a device pointer, causing an
  immediate GPU invalid-address fault (SIGSEGV/SIGBUS on the host).
  The same invariant applies to every CUDA feature extractor that
  allocates device-side flat buffers via `vmaf_cuda_buffer_alloc`
  and passes them directly to `cuLaunchKernel`: always use
  `&buf->data`, never `(void *)buf`. Device-pointer arithmetic must
  also be performed on the `CUdeviceptr` integer type directly —
  avoid casting through `uint8_t *` (UB even though it round-trips
  on x86-64 today).

- **`integer_psnr_cuda.c` honours `enable_chroma` option parity** (ADR-0453).
  The `enable_chroma` option (default `true`) clamps `n_planes` to 1 in
  `init_fex_cuda` when set to `false`, matching CPU
  `integer_psnr.c::init`'s behaviour. The clamp runs after the
  `pix_fmt == YUV400P` guard so that YUV400 sources are always luma-only
  regardless of the option. On rebase: if upstream Netflix adds an
  `enable_chroma` option to the CPU path that behaves differently from the
  fork's GPU guard, audit both and keep the GPU clamp semantically
  equivalent. The SYCL and Vulkan twins carry the identical guard and must
  move in lockstep with any change to this one. The cross-backend parity
  gate at `places=4` covers both `enable_chroma=true` (default) and
  `enable_chroma=false` paths.

- **Host-side preprocessing in CUDA feature extractor `submit` callbacks
  must download GPU→host first.** Pictures passed to a CUDA extractor's
  `submit()` have device pointers in `data[]`; the host cannot read them
  directly. Use `vmaf_cuda_picture_download_async` followed by
  `cuStreamSynchronize` on the picture's private stream (obtained via
  `vmaf_cuda_picture_get_stream`) before passing the picture to any
  host-side function that dereferences `data[]`. The CAMBI extractor
  (`integer_cambi_cuda.c::submit_fex_cuda`) is the canonical example
  of this pattern (Issue #857 fix). All other CUDA extractors in this
  directory currently keep preprocessing on the GPU and are not affected,
  but the rule applies to any future extractor that mixes GPU input
  pictures with host-side preprocessing.

- **`integer_adm_cuda.c` must NOT include `feature/adm_options.h`
  directly.** `DEFAULT_ADM_NOISE_WEIGHT`, `DEFAULT_ADM_CSF_SCALE`,
  `DEFAULT_ADM_CSF_DIAG_SCALE`, and the full 4-member
  `enum ADM_CSF_MODE` arrive transitively via
  `cuda/integer_adm_cuda.h` → `feature/integer_adm.h`.  A direct
  include reintroduces the 2-member `enum ADM_CSF_MODE` from
  `adm_options.h` and causes a redeclaration error.

- **`integer_adm_cuda.c` / `float_adm_cuda.c` expose three ADM
  tuning parameters** (`adm_csf_scale`, `adm_csf_diag_scale`,
  `noise_weight`) with the same defaults as the CPU path (PR #731).
  If upstream Netflix adds or renames these parameters in
  `integer_adm.c` / `float_adm.c`, the CUDA twins must be updated
  in the same PR.

- **`float_adm_cuda.c` / `float_adm/float_adm_score.cu` AIM/ADM3
  slot-sync invariant** (ADR-0574). `FADM_ACCUM_SLOTS = 9` must
  remain identical in both files. The `.cu` compile unit defines
  the per-WG slot layout (`[0..2]`=csf\_den, `[3..5]`=cm\_num,
  `[6..8]`=aim\_cm); the `.c` host file uses the same constant for
  buffer allocation, `cuMemsetD8Async` size, D2H copy byte-count,
  and the per-WG accumulator read in `collect_fex_cuda`. A slot-count
  mismatch silently overwrites adjacent host memory or produces
  incorrect AIM/ADM3 scores without any crash.
  If the `.cu` file is replaced by a rebase with a pre-ADR-0574
  version (`FADM_ACCUM_SLOTS = 6`), update `float_adm_cuda.c`
  accordingly in the same commit. The `--fmad=false` nvcc flag on
  `float_adm_score.cu` covers all six kernels including the two new
  AIM stages (`float_adm_csf_r`, `float_adm_aim_cm`); do not remove
  it. AIM/ADM3 options: `adm_bypass_cm`, `adm_adm3_apply_hm`,
  `adm_p_norm`, `adm_dlm_weight`, `adm_min_val`, `adm_skip_aim_scale`
  must keep the same defaults as `float_adm.c`.

- **`motion_fps_weight` is a cross-backend parity parameter** — all
  motion-family GPU twins must expose `motion_fps_weight` in their
  `VmafOption options[]` table and apply it identically: for
  `integer_motion_v2_*` (flush-based motion2), the weight scales both
  `score_cur` and `score_next` before the min in `flush()`; for
  `float_motion_*` (collect-based motion2), the weight scales both
  `prev_motion_score` and `motion_score` before the min in `collect()`
  (for index >= 2) and scales `prev_motion_score` alone in `flush()`.
  When `motion_fps_weight = 1.0` (default) the arithmetic is a
  no-op and the `places=4` cross-backend gate must continue to pass.
  If the application math ever changes in the CPU reference
  (`integer_motion_v2.c` / `float_motion.c`), all GPU twins must be
  updated in the same PR. Twins in scope: `integer_motion_v2_cuda.c`,
  `integer_motion_v2_sycl.cpp`, `motion_v2_vulkan.c`,
  `integer_motion_v2_hip.c`, `integer_motion_v2_metal.mm`,
  `float_motion_cuda.c`, `float_motion_sycl.cpp`,
  `float_motion_vulkan.c`, `float_motion_hip.c`,
  `float_motion_metal.mm`. PR #863 initially wired this option.

- **`integer_motion_v2_*` mirror contract** (ADR-0662) — CPU
  `integer_motion_v2.c::mirror` maps `idx >= size` to
  `2 * size - idx - 2`. The CUDA, SYCL, and Vulkan `motion_v2`
  kernels must keep that same high-edge literal. The old `-1`
  formula is stale prose from ADR-0193 bring-up and creates a
  measurable CPU/GPU drift.

- **`integer_adm/adm_cm.cu` (and the rest of the `integer_adm/`
  subdirectory) carries an NVIDIA copyright line** alongside the
  Netflix one. This is upstream-mirror — keep both headers
  verbatim on rebase.

- **Every CUDA reduce kernel SHOULD use warp-reduce + `atomicAdd_int64`
  into a single accumulator; a separate per-thread scratch buffer plus a
  separate reduce kernel launch is the pre-fix legacy pattern.**
  Scale 0 of ADM CM (`adm_cm_line_kernel_8` in `integer_adm/adm_cm.cu`)
  is the canonical model: compute per-pixel result, warp-reduce the
  int64 accumulator, first lane atomicAdds into `accum_global`.
  Scales 1-3 were migrated to this pattern by `i4_adm_cm_line_kernel_fused`
  (PR perf/adm-cm-cuda-warp-reduce-fusion).  Any future reduce kernel
  that writes to a scratch buffer and launches a second kernel to sum it
  should be refactored to the fused pattern instead.

- **`kernel_template.h` mirror with HIP** (ADR-0241). The CUDA
  `cuda/kernel_template.h` (one level up) and HIP
  `../hip/kernel_template.h` move in lockstep. Any change to
  the CUDA template's struct fields, helper signatures, or
  semantics requires a paired HIP change in the same PR.
  Consumers of the template (`integer_psnr_cuda.c` and
  follow-on `integer_ciede_cuda.c` / `integer_moment_cuda.c` /
  ...) lock the HIP twins call-graph-for-call-graph; see
  [`../../hip/AGENTS.md`](../../hip/AGENTS.md) for the full
  consumer list.

- **kernels with high spatial overlap (>=50% redundant cross-thread reads) must
  stage into `__shared__`** (ADR-0464). `cambi_spatial_mask_kernel` sets the
  precedent: a 22x22 `uint8_t zd_tile[22][32]` tile is populated cooperatively
  by the 16x16 block (2-pass, 256 threads, 484 elements, 3x484 = 1452 global
  reads per block) before the 7x7 box-sum loop reads exclusively from SLM.
  Any new stencil kernel with a halo >= half the block dimension and >= 50%
  cross-thread read overlap must follow the same pattern: compute the shared
  footprint as (BLOCK + 2*HALO)^2 elements, load cooperatively in
  ceil(N/BLOCK_AREA) passes, `__syncthreads()`, then read from SLM.
  Omitting the tile for such kernels is a performance regression; the
  parity-gate alone does not catch it.
- **`ssimulacra2/ssimulacra2_blur.cu` blur kernels with per-channel loops must
  fuse via `gridDim.z`** (ADR-0456). The fused kernels `ssimulacra2_blur_h3`,
  `ssimulacra2_transpose`, and `ssimulacra2_blur_v3_transposed` all take a
  `plane_stride` argument and use `blockIdx.z` to select the XYB channel. The
  V-pass operates on a column-major transposed buffer to convert stride-`width`
  per-thread access to stride-1 sequential reads. Any future blur kernel that
  iterates over columns (V-direction IIR) requires a preceding transpose for
  coalescing; do not skip the transpose to save one launch — the V-pass
  performance benefit outweighs the launch cost at all resolutions ≥ 480p.

- **`ssim_cuda.c` and `integer_ssim_cuda.c` provide different features — do not
  conflate them** (ADR-0564). `ssim_cuda.c` registers `vmaf_fex_integer_ssim_cuda`
  and provides `"ssim"` (the real 9-tap int64 integer SSIM, bit-exact with CPU).
  `integer_ssim_cuda.c` is a historical misnomer: it registers a *different*
  `vmaf_fex_integer_ssim_cuda` symbol and provides `"float_ssim"` (11-tap
  floating-point Gaussian). Both symbols are linked and registered in
  `feature_extractor.c`; `feature_extractor_list[]` puts `ssim_cuda.c`'s extractor
  first so that `vmaf_get_feature_extractor_by_name("ssim")` resolves correctly.
  **Never swap the order** of these two entries. A planned follow-up (post-ADR-0564)
  will rename `integer_ssim_cuda.c` → `float_ssim_cuda.c` to eliminate the confusion;
  until then, keep the naming mismatch explicit and do not merge the two files.

## Build

CUDA feature TUs compile only when `meson setup -Denable_cuda=true`.
The `enable_cuda` umbrella flag gates inclusion via
`#if HAVE_CUDA` blocks in `feature/feature_extractor.c`.

## Governing ADRs

- [ADR-0182](../../../../docs/adr/0182-gpu-long-tail-batch-1.md) +
  [ADR-0188](../../../../docs/adr/0188-gpu-long-tail-batch-2.md) +
  [ADR-0192](../../../../docs/adr/0192-gpu-long-tail-batch-3.md) —
  GPU long-tail batches. Every CUDA feature kernel here corresponds
  to a row in one of these.
- [ADR-0214](../../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  GPU-parity CI gate.
- [ADR-0219](../../../../docs/adr/0219-motion3-gpu-contract.md) —
  motion3 GPU contract.
- [ADR-0241](../../../../docs/adr/0241-hip-first-consumer-psnr.md) —
  kernel-template mirror between CUDA and HIP.
- [ADR-0243](../../../../docs/adr/0243-enable-lcs-gpu.md) — MS-SSIM
  `enable_lcs` GPU contract.
- [ADR-0246](../../../../docs/adr/0246-cuda-kernel-template-feature.md) —
  per-feature CUDA kernel-template scaffolding.
- [ADR-0360](../../../../docs/adr/0360-cambi-cuda.md) —
  CAMBI CUDA port (Strategy II hybrid, T3-15a).
- [ADR-0464](../../../../docs/adr/0464-cambi-cuda-smem-tile.md) --
  CAMBI CUDA spatial-mask SLM tile (perf-audit 2026-05-16 win 3).
- [ADR-0456](../../../../docs/adr/0456-ssimulacra2-cuda-blur-fusion-transpose.md) —
  SSIMULACRA2 CUDA blur: 3-channel kernel fusion + V-pass transpose.
- [ADR-0574](../../../../docs/adr/0574-hdr-features-cuda-twins-phase-1.md) —
  CUDA twins for HDR-model `aim` and `adm3` sub-features (Phase 1);
  `FADM_ACCUM_SLOTS` 6→9 slot-sync invariant.

## Stencil/convolution kernel invariant (ADR-0454)

- **Stencil and convolution kernels with data reuse > 2 taps must stage
  input into `__shared__` memory; never re-read 17 taps from L2.**
  Specifically: `integer_vif/filter1d.cu` stages a tile of
  `(BLOCKY + fwidth - 1)` rows (vertical pass) or
  `(BLOCKX * val_per_thread + 2*half_fw + 1)` elements per channel (horizontal
  pass) into `__shared__` before the convolution loop.  The smem load phase
  handles mirror-boundary clamping; the compute phase reads smem unconditionally.
  Any new separable filter kernel with filter width ≥ 5 must follow this pattern.
  Removing the smem staging layer reverts the 15–35% VIF speedup.
  See [ADR-0454](../../../../docs/adr/0454-vif-cuda-smem-staging.md) and
  [Research-0135](../../../../docs/research/0135-vif-cuda-smem-staging-2026-05-16.md).

## `__mul24` / `__umul24` prohibition (Research-0734)

**Never introduce `__mul24`, `__umul24`, or `__mul24hi` anywhere in this
directory.** NVIDIA confirmed a silent data-corruption bug in these intrinsics
present since CUDA 11.1 and fixed only in CUDA 13.3: any `__mul24(val, CONSTANT)`
call where one argument is a compile-time constant may produce incorrect results
on PTX and SASS generated by CUDA 11.1–13.2.

The 2026-05-28 audit (Research-0734) confirmed zero `__mul24` calls exist in
the entire `core/src/feature/cuda/` and `core/src/cuda/` trees; the codebase
has no exposure to this bug. Future kernel authors must use the plain C `*`
multiply operator for all integer arithmetic, which the CUDA compiler handles
correctly at all toolchain versions.

If a future author believes `__mul24` is required for a documented performance
reason (e.g. a specific `dp4a` / `imad.lo.u32` targeting pattern), they must:

1. Cite this invariant note in the PR.
2. Demonstrate via `ncu --section InstructionStats` that the performance gain
   is load-bearing.
3. Obtain CODEOWNERS sign-off acknowledging the minimum-CUDA-13.3 container
   constraint (CLAUDE.md §15).
4. Document the constraint in `docs/backends/cuda/overview.md` under
   "Known gaps / CUDA version notes".

## extern "C" invariant — mandatory for every new CUDA kernel TU (ADR-0747)

Every `__global__` kernel that the host looks up by name via
`cuModuleGetFunction` **must** be defined inside an `extern "C" { }`
block in its `.cu` file.

Rationale: nvcc compiles `.cu` files as C++ by default. Without
`extern "C"`, the kernel symbol receives C++ name-mangling (e.g.,
`_Z31integer_ssim_horiz_8bpc...`). `cuModuleGetFunction` uses the
plain C name and receives `CUDA_ERROR_NOT_FOUND`, silently disabling
the feature. This was found by a full sweep (Research-0747) that
identified `integer_ssim/integer_ssim_score.cu` as broken; the
pattern caused `--feature ssim --backend cuda` to fail silently from
the file's introduction until this fix.

Rules:

- Wrap only `__global__` entry points. `__device__` helpers,
  `__constant__` arrays, and `#define` macros do not need wrapping.
- For macro-expanded kernel instantiations (e.g., the `FILTER1D_*`
  and `ADM_CSF_KERNEL` patterns), place the macro invocations inside
  the `extern "C" { }` block, not the macro definition.
- CI gate: `scripts/dev/check-cuda-extern-c.sh` fails if any kernel
  referenced by `cuModuleGetFunction` is found outside `extern "C"`.
  Run it locally before pushing CUDA kernel changes.

See [ADR-0747](../../../../docs/adr/0747-cuda-extern-c-sweep.md).

## Register-pressure ceiling pattern for horizontal convolution kernels (ADR-0743)

- **Occupancy-critical kernels with > 48 registers per thread must carry
  `__launch_bounds__(BLOCKX, min_blocks)` where `min_blocks` satisfies
  `floor(65536 / BLOCKX / min_blocks) ≤ 48` for sm_89 target.**
  `filter1d_8_horizontal_kernel` (17-tap, vpt=2) hit 56 registers at baseline,
  capping the register-limited block count at 9/SM and theoretical occupancy at 75%.
  Adding `__launch_bounds__(128, 10)` reduced registers to 48, raising theoretical
  occupancy to 83.3% (sm_89).

  Key derivation: floor(65536 / 128 / 10) = 51 registers max → ptxas allocates
  48.  This is a **compiler hint only** — it sets a maximum register budget; it
  does not guarantee a particular schedule.

  Caveat: on sm_75/sm_80/sm_86 (max 1024 threads/SM), `min_blocks=10` × 128 = 1280
  exceeds the per-SM thread limit.  ptxas emits a non-fatal advisory
  "minnctapersm out of range, ignored" for those targets; those targets retain
  the pre-hint register count.  This is acceptable for the fork's primary target
  (sm_89 / RTX 4090) and causes no regression on older targets.

- **Do not increase `val_per_thread` past 2 for the 17-tap horizontal kernel.**
  vpt=4 was evaluated during ADR-0743 profiling.  smem grows 7644 → 14812 B/block;
  on sm_89 (102400 B/SM smem) this makes the kernel smem-limited at 6 blocks/SM =
  37.5% occupancy — worse than the 48-reg vpt=2 path at 10 blocks/SM = 62.5%.
  See [ADR-0743](../../../../docs/adr/0743-cuda-vif-filter1d-ncu-driven-perf.md).

- **`__ldg()` on read-only tmp-channel loads is correct and beneficial at ≥1080p.**
  The 7 tmp buffers (mu1, mu2, ref, dis, ref_dis, ref_convol, dis_convol) are
  written exclusively by the preceding vertical pass.  `__ldg()` routes their
  horizontal-pass loads through the read-only L1 cache.  At 576p the workload is
  wave-limited (0.76 waves / 128 SMs) and the effect is neutral; at ≥1080p the
  combined tmp footprint exceeds L2 capacity and the cache-routing provides
  measurable L2-pressure relief.  See ADR-0743.

## `__ldg()` pattern for pass-2 read-only intermediate buffers (ADR-0754)

## `__ldg()` pattern for pass-2 read-only intermediate buffers (ADR-0754, ADR-0757)

- **Extract raw `const float *__restrict__` pointers from `VmafCudaBuffer` structs
  BEFORE the inner loop, then use `__ldg(&ptr[idx])` for every load.**
  `calculate_ssim_vert_combine` in `integer_ssim/ssim_score.cu` is the canonical
  example: the 5 horizontal-pass intermediate buffers are written exclusively by
  the horiz kernel and are never aliased in the vert pass. Passing `VmafCudaBuffer`
  by value hides the pointer from the compiler's non-coherent-load analysis; the
  one-time pointer extraction at kernel entry makes the alias-free invariant visible
  so `__ldg()` can route all 5×11 = 55 inner-loop loads through the read-only L1
  texture cache rather than L2. Any future pass-2 kernel with a similar write-once /
  read-many intermediate buffer must follow the same pattern.
  See [ADR-0754](../../../../docs/adr/0754-cuda-ssim-vert-combine-ldg-pinned-leak.md).

## Pinned-host memory free invariant after `readback_free` (ADR-0754)

- **`vmaf_cuda_kernel_readback_free` NULLs `rb->host_pinned` but does NOT free it.**
  The kernel template explicitly documents this as a caller responsibility (see
  comment in `cuda/kernel_template.h` near `vmaf_cuda_kernel_readback_free`). Every
  `close_fex_cuda` that calls `readback_free` must save `rb.host_pinned` to a local
  BEFORE calling `readback_free`, then call `vmaf_cuda_buffer_host_free(cu_state,
  saved)` afterward. Omitting the host-free leaks one page of CUDA pinned host
  memory per `vmaf_close()` cycle. `integer_ssim_cuda.c::close_fex_cuda` is the
  reference fix (ADR-0754). Verify with:
  `compute-sanitizer --tool memcheck --leak-check full ./vmaf --feature float_ssim --backend cuda ...`
  — the summary must show 0 bytes from `cuMemHostAlloc` after fix.
  Note: `integer_psnr_cuda.c` has the same gap and is scheduled for a follow-up.

## Motion kernel dispatch bottleneck (Research-0760)

- **`calculate_motion_score_kernel_8bpc` is dispatch-bottlenecked at all resolutions
  below 4K.** ncu profile (2026-05-29, RTX 4090) shows GPU busy fraction <1% of
  wall time at 576p (kernel 7 µs, dispatch ~12.7 ms/frame). CUDA/CPU ratio is 0.22×
  at 576p and 5.8× at 4K — the crossover is entirely explained by dispatch overhead.
  Any optimization that does not address per-frame dispatch will not improve
  sub-4K throughput regardless of kernel-level changes. The primary fix is
  multi-frame SAD batching (accumulate N frames before readback synchronization).
  See [Research-0760](../../../../docs/research/0760-cuda-motion-ncu-multi-resolution-20260529.md).

## Motion SAD batch fencing — MOTION_BATCH_DEPTH invariants (ADR-0845)

- **`integer_motion_cuda.c` uses MOTION_BATCH_DEPTH=8 per-slot SAD buffers to
  reduce cuStreamSynchronize from once-per-frame to once-per-8-frames (ADR-0845).**
  Key invariants that must be preserved on rebase:

  1. `sad[MOTION_BATCH_DEPTH]` is a ring of independent device buffers (not a single
     shared accumulator). Each submit() zeroes `sad[index % MOTION_BATCH_DEPTH]` on
     pic_stream BEFORE the kernel launch so the memset and the atomicAdd are on the
     same stream (per ADR-0358 / AGENTS.md "integer_motion_cuda.c::submit_fex_cuda
     runs the SAD cuMemsetD8Async on pic_stream" invariant).
  2. `s->str` is the readback drain stream. Every submit() chains its kernel-complete
     event from pic_stream to s->str via `cuStreamWaitEvent`. The DtoH copies are
     NOT queued in submit(); they are queued in batch-boundary collect() calls.
  3. Non-boundary collect() calls (where `index % MOTION_BATCH_DEPTH != MOTION_BATCH_DEPTH-1`)
     increment frame_index and return 0 without emitting scores or touching s->str.
     Emitting from non-boundary collects would break the batch fence.
  4. `emit_batch_scores()` temporarily overrides `s->frame_index` to `i + 1` for
     each frame `i` in the batch before calling `motion3_postprocess_cuda()`. This
     preserves the moving-average guard semantics (ADR-0219). Removing or bypassing
     this frame_index override produces incorrect motion3 scores when
     `motion_moving_average=true`.
  5. The drain_batch engine-scope optimization (ADR-0242) is NOT used by
     integer_motion_cuda after ADR-0845. Do not re-add `vmaf_cuda_drain_batch_register_event`
     calls to submit() — they would conflict with the batch fence logic.
  6. flush() handles the final partial batch for frame counts that are not a
     multiple of MOTION_BATCH_DEPTH. The `flush_start` clamp to 1 skips frame 0
     (which has no valid SAD — the kernel runs but prev_blurred is uninitialized).

## Resolution-aware kernel variant dispatch (ADR-0753)

`resolution_dispatch.h` / `resolution_dispatch.c` in this directory provide a
lightweight `vmaf_cuda_workload_class(w, h)` classifier used to pick between
kernel variants at runtime. The current policy table is in ADR-0753.

**How to add a new resolution-aware variant:**

1. In the extractor's `.cu` file, define two kernel entry points using sibling
   macros: one WITH `__launch_bounds__` (or the other occupancy hint), one
   WITHOUT, giving the no-hint variant a `_no_bounds` suffix.
   Both must be inside the `extern "C" { }` block (ADR-0747).
2. In the extractor state struct (e.g. `AdmStateCuda`), add a second
   `CUfunction` pointer for the no-hint variant.
3. In `init_fex_cuda`, load both pointers via `cuModuleGetFunction`.
   Add a short comment citing the ADR-0753 policy (see existing examples).
4. At the kernel-launch site in `submit_fex_cuda`, call
   `vmaf_cuda_workload_class(w, h)` and branch on the result.
   The branch structure is always a single ternary / if-else — no nested
   policy trees. Consult the policy table in ADR-0753 for which class gets
   the bounded variant; the pattern so far:
   - `adm_cm`: BOUNDED at `WS_MEDIUM` only; NO_BOUNDS at `WS_SMALL` + `WS_LARGE`.
   - `filter1d` + `ssim_vert_combine`: BOUNDED at `WS_MEDIUM` + `WS_LARGE`;
     NO_BOUNDS at `WS_SMALL` only.
5. Add a row to the policy table in ADR-0753 `## Decision` and to the kernel
   list in `resolution_dispatch.h`.
6. Note the new invariant in this file under "Rebase-sensitive invariants".
7. Update `docs/backends/cuda/overview.md` kernel table.

**Verified wirings (as of the ADR-0753 extended scope):**

| Feature | BOUNDED variant (kernel name) | NO_BOUNDS variant | Policy |
|---|---|---|---|
| `adm_cm` | `adm_cm_line_kernel_8` | `adm_cm_line_kernel_8_no_bounds` | MEDIUM only |
| `filter1d` | `filter1d_8_horizontal_kernel_2_17_9` | `filter1d_8_horizontal_kernel_2_17_9_no_bounds` | MEDIUM + LARGE |
| `ssim_vert_combine` | `calculate_ssim_vert_combine` | `calculate_ssim_vert_combine_no_bounds` | MEDIUM + LARGE |

- **`integer_vif_cuda.c::filter1d_8` picks `filter1d_8_horizontal_kernel_2_17_9_no_bounds`
  at `WS_SMALL` and the bounded variant at `WS_MEDIUM`/`WS_LARGE`** (ADR-0753 extended
  scope). `VifStateCuda` carries `func_filter1d_8_horizontal_kernel_2_17_9_no_bounds`.
  `filter1d.cu` defines `FILTER1D_8_HORI_NO_BOUNDS(2, 17, 9)` inside `extern "C" {}`.
  On rebase: verify both `cuModuleGetFunction` calls in `init_fex_cuda` reference
  valid symbols. If upstream refactors the macro or adds new `fwidth` variants,
  apply the `_NO_BOUNDS` sibling macro around the new body too.

- **`integer_ssim_cuda.c::submit_fex_cuda` picks `calculate_ssim_vert_combine_no_bounds`
  at `WS_SMALL` and the bounded variant at `WS_MEDIUM`/`WS_LARGE`** (ADR-0753 extended
  scope). `SsimStateCuda` carries `func_vert_no_bounds`.
  `integer_ssim/ssim_score.cu` defines both variants inside `extern "C" {}`.
  On rebase: if upstream modifies `calculate_ssim_vert_combine`, apply the same
  diff to `calculate_ssim_vert_combine_no_bounds` (body is identical; only the
  `__launch_bounds__(128)` annotation differs).

## `__ldg()` pattern for VmafPicture channel reads (ADR-0762)

- **Extract typed `const uint8_t *__restrict__` (or `uint16_t *__restrict__` for
  16bpc) channel pointers from `VmafPicture` struct args BEFORE the per-pixel body,
  then use `__ldg(&ptr[idx])` for all channel reads.**
  `calculate_ciede_kernel_8bpc` and `calculate_ciede_kernel_16bpc` in
  `integer_ciede/ciede_score.cu` are the canonical examples: the `VmafPicture` struct
  carries `void *data[3]`, which prevents the compiler from seeing that the reads are
  alias-free when the struct is passed by value. Extracting typed `__restrict__`
  pointers at kernel entry makes the invariant visible and routes the 6 per-pixel
  channel reads through the L1 read-only texture cache. Any future kernel that reads
  per-pixel plane data from `VmafPicture` must follow the same pattern.
  See [ADR-0762](../../../../docs/adr/0762-cuda-ciede-ldg.md).

- **`integer_adm/adm_decouple.cu` carries the same F3 `__ldg()` fix (ADR-0763).**
  `adm_decouple_kernel` (scale-0, `int16_t`) and `adm_decouple_s123_kernel`
  (scales 1-3, `int32_t`) both extract `const T *__restrict__` read-only band
  pointers before the per-pixel body and use `__ldg()` for all reads. Write-back
  band pointers are plain non-`const` (no `__ldg()` on stores). Note: the file
  is currently dead (decouple computation is inlined into `adm_csf.cu` /
  `adm_cm.cu` via `adm_decouple_inline.cuh`) — rebase on a change to that file
  does NOT affect `adm_decouple.cu`.
  See [ADR-0763](../../../../docs/adr/0763-cuda-adm-decouple-ldg.md).

- **`integer_adm/adm_cm.cu` `x_sq` reduction requires explicit parentheses around
  `add_shift_sq` before the right-shift (r6-cuda-kernel / 2026-06-04).** The expression
  `(int64_t)accum * accum + add_shift_sq >> shift_sq` is parsed by C++ as
  `+ (add_shift_sq >> shift_sq)` = `+ 0` because `>>` binds tighter than `+`. The
  correct form is `((int64_t)accum * accum + add_shift_sq) >> shift_sq`, which matches
  the CPU reference macro `I4_ADM_CM_ACCUM_ROUND` in `integer_adm.c:743` and the fused
  kernel at `adm_cm.cu:259`. This defect affected two reduction loops in the file
  (lines 373 and 712). On rebase: if either loop is modified, verify the parenthesisation
  of the `x_sq` computation before pushing.

- **`integer_vif/filter1d.cu` 16-bit rd-filter upper-bound guard must use
  `(fwidth - fwidth_rd)`, not `(fwidth_rd - fwidth_rd)` (r6-cuda-kernel / 2026-06-04).**
  The correct guard is `fi < (fwidth - (fwidth - fwidth_rd) / 2)`, matching the 8-bit
  form at line 183. Writing `(fwidth_rd - fwidth_rd)` (always zero) widens the tap
  window to all `fwidth` taps and causes OOB reads into `vif_filt.filter[scale+1]`.
  On rebase: if the vertical-pass loop in the 16-bit path is modified, verify the
  upper-bound guard expression before pushing.

- **`integer_adm/adm_csf.cu` and `integer_adm/adm_cm.cu` carry the F3 `__ldg()` fix on the
  active path (ADR-0773).** The six inline `__device__` helpers in `adm_cm.cu`
  (`inline_i4_csf_a`, `inline_i4_decouple_r`, `inline_s0_csf_a`, `inline_s0_decouple_r`,
  `inline_i4_csf_r`, `inline_s0_csf_r`) and the two kernel templates in `adm_csf.cu`
  (`i4_adm_csf_kernel<>`, `adm_csf_kernel<>`) all extract `const T *__restrict__` band
  pointers from `cuda_*_adm_dwt_band_t` structs before any indexed load. All six per-pixel
  DWT2 band reads use `__ldg()`. When rebasing or modifying these helpers: preserve the
  `__restrict__` extraction pattern; do not add writes through these pointers (they are
  read-only inputs). ADR-0773 completes the ADR-0756 `adm_decouple` dispatch item.
  See [ADR-0773](../../../../docs/adr/0773-cuda-adm-decouple-inline-ldg.md).
