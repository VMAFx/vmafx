<!-- markdownlint-disable MD013 MD060 -->
# AGENTS.md — core/src/feature/cuda

Orientation for agents on per-feature CUDA kernels (host
glue + `.cu` device code). Parent: [../AGENTS.md](../AGENTS.md). Backend
runtime (context, stream, picture-pool) lives one level up
in [`../../cuda/AGENTS.md`](../../cuda/AGENTS.md).

## Deleted orphan TU (ADR-0546)

`float_ssim_cuda.c` removed by ADR-0546 (`chore/hip-cuda-orphan-tu-cleanup`,
2026-05-18). Defined `vmaf_fex_float_ssim_cuda` but not listed in
`core/src/meson.build`; `integer_ssim_cuda.c` (which is compiled) also
defines same symbol and = current canonical TU (adds
`enable_chroma` option and other improvements absent from orphan copy).
Do not re-add `float_ssim_cuda.c` without consulting ADR-0546.

## Scope

```text
feature/cuda/
  <feature>_cuda.{c,h}        # host glue: registration, submit/collect, kernel-template wiring
  <feature>/                  # subdirectory of `.cu` device code (where the host glue is non-trivial)
    *.cu                      # CUDA kernel TUs (compiled with nvcc)
    *.cuh                     # device-side helpers (included from .cu only)
```

Examples: `integer_psnr_cuda.c` = single-file consumer using
kernel-template flat shape; `integer_adm/` = multi-`.cu` consumer
because ADM splits across DWT2 + decouple + CSF + CM passes.

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md) +
  [../../AGENTS.md](../../AGENTS.md) +
  [`../../cuda/AGENTS.md`](../../cuda/AGENTS.md)).
- **Wholly-new fork files use dual Netflix + Lusoris/Claude
  copyright header** per [ADR-0025](../../../../docs/adr/0025-copyright-handling-dual-notice.md).
  Many TUs here predate dual-notice rule, carry only
  Netflix header (with NVIDIA contributor lines on `integer_adm/`
  CUDA kernels) — correct for upstream-mirrored files; do
  not retro-fit.
- **`#include` order** mirrors SYCL / Vulkan twins:
  `feature_collector.h` / `feature_extractor.h` first, then
  `cuda/integer_<feature>_cuda.h`, then `cuda_helper.cuh` /
  `kernel_template.h`. Don't shuffle.
- **fmaf contraction OFF for precision-critical kernels.** Parent
  build line passes `--fmad=false` to `nvcc` for feature
  TUs participating in cross-backend gates with `places=4`.
  Removing it drifts `float_adm_cuda` / `ssimulacra2_cuda` past
  gate (mirror of SYCL `-fp-model=precise` and Vulkan
  GLSL `precise` / `NoContraction` rules). On rebase: keep
  flag.

## Twin-update rules

Every TU here has at least one cross-backend twin.
Change to one twin **must** ship with matching change(s) in
same PR:

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

Full GPU twin matrix governed by GPU long-tail batches:
[ADR-0182](../../../../docs/adr/0182-gpu-long-tail-batch-1.md) (psnr /
ciede / moment), [ADR-0188](../../../../docs/adr/0188-gpu-long-tail-batch-2.md)
(ssim / ms_ssim / psnr_hvs), [ADR-0192](../../../../docs/adr/0192-gpu-long-tail-batch-3.md)
(motion_v2 / float-twins / ssimulacra2 / cambi; `float_ansnr` removed
in commit 70ed8b3ce3 / PR #38).

## Parity invariant — motion3 CPU and CUDA moving-average paths

`integer_motion.c` (CPU) and `integer_motion_cuda.c` (CUDA) both implement
motion3 post-process as host-side moving average over blended motion2
scores. Both paths **must stay in numerical parity at places=4** (delta
≤ 1e-4, per ADR-0214). Gate enforced by
`core/test/test_cuda_motion3_parity.c`. Any change to blend formula
(`motion_blend()`), moving-average guard condition, or `motion_max_val`
clipping must mirror across both files. Same for SYCL / Vulkan /
HIP / Metal motion twins listed in Twin-update table below — same PR.

## Rebase-sensitive invariants

- **Every successfully loaded CUDA module has an owned handle and context-owned
  unload path ([ADR-1336](../../../../docs/adr/1336-cuda-context-owned-resource-teardown.md)).**
  Any extractor that calls `cuModuleLoadData` must retain the resulting
  `CUmodule` in its state and call `vmaf_cuda_module_unload` on every matching
  close or init-unwind path after pending device work is synchronized. Custom
  streams and events use `vmaf_cuda_stream_destroy` and
  `vmaf_cuda_event_destroy` for the same reason: raw driver teardown acts on
  the current context, which may be absent or belong to another caller at
  close. Guard partially initialized handles, clear them only after confirmed
  destruction, preserve the first teardown error, and continue best-effort
  cleanup. Preserve the four-module teardown in `integer_adm_cuda.c` and the
  two-module teardown in `ssimulacra2_cuda.c`. The complete inventory in
  `test_cuda_module_lifecycle_contract.py` must change with every new owner.

- **GPU SpEED means/cov must match CPU GLOBAL covariance, and ref/dis
  must use SEPARATE eigenvalue bases** (PR #1029,
  `research-1120-gpu-speed-covariance-eigenbasis-correctness`). In
  `speed/speed_score.cu` (and HIP / SYCL twins) means kernel computes
  **scalar global mean** for each of 25 phase-shift elements over
  full phase-shifted submatrix — `means[25]`, indices `[0, 25)` — **NOT**
  per-tile `means[25 * num_blocks]` block-local mean. Cov kernel does one
  global submatrix sweep with those scalar means, divides by `N` **once**,
  with **no** per-tile loop. `means[]` buffer stays over-allocated at
  `25 * num_blocks` (launch geometry unchanged) but only `[0, 25)`
  written/read — do not "tidy" it back to tiled layout. Separately,
  `speed_chroma_cuda.c` / `speed_temporal_cuda.c` keep reference and
  distorted **covariance + eigenvalues independent**: after reference
  linalg, `cuMemcpyDtoD` ref eigenvalues into `d_eigenvalues_ref`; distorted
  path keeps distorted covariance (do **not** re-add old
  cov save/restore); `speed_score_kernel` takes both `ref_eigenvalues` and
  `dis_eigenvalues` (ref entropy uses ref, dis entropy uses dis). Mixing them
  reintroduces ~2× chroma error (masked on temporal, where `ref ≈ dis`).
  Verify with `test_cuda_speed_chroma_parity` / `test_cuda_speed_temporal_parity`
  at places=4 (1e-4, ADR-0214); pass bit-parity on RTX 4090. Any
  change to SpEED kernel math must mirror across CPU
  (`speed.c`), CUDA, HIP, and SYCL backends in same PR.

- **`vmaf_cuda_kernel_readback_free` owns pinned-host free
  (2026-05-29 sweep).** Helper in `core/src/cuda/kernel_template.h`
  calls `vmaf_cuda_buffer_host_free(cu_state, rb->host_pinned)` before
  NULLing pointer. Callers of `vmaf_cuda_kernel_readback_free` must
  NOT also call `vmaf_cuda_buffer_host_free` on `rb->host_pinned` — doing
  so would double-free pinned allocation. Pre-2026-05-29 pattern
  where callers called `vmaf_cuda_buffer_host_free` explicitly =
  incorrect; helper now owns free. See
  PR fix/cuda-pinned-host-leak-sweep-20260529.

- **Feature-local pinned host buffers must be freed in BOTH `close_fex_cuda`
  AND init `free_buffers` error path (2026-06-27 bug-hunt).** Buffers
  allocated directly via `vmaf_cuda_buffer_host_alloc` (i.e. NOT routed through
  `vmaf_cuda_kernel_readback_free`) — e.g. `float_vif_cuda::num_host`/`den_host`,
  `float_adm_cuda::accum_host`, `integer_ms_ssim_cuda::h_ref`/`h_cmp`/`h_*_partials`
  — owned by feature, must be released with
  `vmaf_cuda_buffer_host_free` (no separate `free()`, unlike device-buffer
  wrappers which need both). Missing close-path free leaks page-locked host
  memory on every `vmaf_close()`. See branch fix/bughunt-cuda.

- **CUDA error-path labels return mapped errno, not literal (2026-06-27
  bug-hunt).** Any `fail:` / `fail_pop:` / `fail_after_pop:` label reached from
  `CHECK_CUDA_GOTO` must `return _cuda_err;` — macro already set `_cuda_err`
  from `vmaf_cuda_result_to_errno(CUresult)` (`-ENOMEM` / `-ENODEV` / `-EINVAL`
  / `-EIO`). Returning literal `-EIO` discards real failure cause,
  diverges from `CHECK_CUDA_RETURN` convention in `cuda_helper.cuh`.
  `speed_temporal_cuda.c` / `speed_chroma_cuda.c` extractors keep literal
  `-EIO` only on two manual (non-macro) `cuMemcpyDtoH` / `cuCtxPushCurrent`
  boolean checks. See branch fix/bughunt-cuda.

- **`integer_ms_ssim_cuda.c` honours `enable_lcs`, `enable_db`,
  `clip_db` GPU contracts** (ADR-0243, ADR-0460). Emits 15 extra
  metrics (`float_ms_ssim_{l,c,s}_scale{0..4}`) when `enable_lcs=true`,
  all `l_scale*` first then `c_*` then `s_*` (metric ordering =
  public API; renaming or reordering breaks cross-backend parity
  gate). Returns dB-domain score (`-10*log10(1-ms_ssim)`) when
  `enable_db=true`, optionally clipping via `clip_db`. See
  [../../AGENTS.md §"MS-SSIM `enable_lcs` GPU
  contract"](../../AGENTS.md).

- **`integer_motion_cuda.c::motion3_postprocess_*` honours
  motion3 GPU contract** (ADR-0219). Applies CPU's host-side
  post-process to motion2 with no device-side state. Two
  invariants flow: (1) `motion_five_frame_window=true` returns
  `-ENOTSUP` at `init()`; (2) any change to `motion_blend()` /
  `motion_max_val` / moving-average must mirror across three
  GPU motion twins in same PR. See [../../AGENTS.md §"motion3_score
  GPU contract"](../../AGENTS.md).

- **`integer_motion_cuda.c::submit_fex_cuda` runs SAD
  `cuMemsetD8Async` on `pic_stream`, NOT on `s->str`** (ADR-0358).
  Kernel atomic-adds into same single-int64 buffer on
  `pic_stream`; both streams `CU_STREAM_NON_BLOCKING`, have
  no event linking them, so co-locating memset on same
  stream as kernel = only thing that orders them.
  Matching pattern lives at `integer_motion_v2_cuda.c:188`. Any
  rebase or follow-up reverting memset onto separate
  stream silently re-introduces cross-stream race.

- **`integer_motion_cuda.c::collect_fex_cuda` and `flush_fex_cuda`
  emit `motion2_score = MIN(score * motion_fps_weight, motion_max_val)`,
  NOT raw `min(prev, cur)` SAD score** (ADR-0358). Mirrors
  CPU reference at `integer_motion.c:563`.
  `motion3_postprocess_cuda` moving-average guard reads
  `frame_index > 2` (NOT `> 1`) because `frame_index`
  pre-incremented in `collect()` before helper runs. Tripped
  by non-default `motion_fps_weight ≠ 1.0` /
  `motion_moving_average = true`.

- **`integer_ms_ssim_cuda.c` and `integer_ssim_cuda.c` pass
  `channel=0` to `picture_copy()`** per upstream
  d3647c73 prerequisite port. If future upstream commit
  evolves signature further, update these call sites in
  lockstep with upstream-mirror callers (`float_*` series).
  See [../../AGENTS.md §"`picture_copy()` carries a `channel`
  parameter"](../../AGENTS.md).

- **`integer_psnr_hvs_cuda.c` participates in engine-scope CUDA
  drain batch.** Its `submit_fex_cuda` queues all three plane partial
  DtoH copies on `s->lc.str`, records `s->lc.finished` via
  `vmaf_cuda_kernel_submit_post_record`, registers lifecycle
  with `drain_batch`. Its `collect_fex_cuda` must call
  `vmaf_cuda_kernel_collect_wait` before reading `h_partials[]`;
  reintroducing raw `cuMemcpyDtoHAsync` + `cuStreamSynchronize` in
  collect reopens T-GPU-OPT-3's per-frame sync stall. Scheduling
  change is CUDA-only, does not require SYCL / Vulkan twin edits,
  because it does not alter kernel math or emitted metrics.

- **`integer_psnr_hvs_cuda.c` honours `enable_chroma` option parity** (mirrors
  ADR-0453 on psnr_hvs surface). `enable_chroma` option (default
  `false`) clamps `n_planes` to 1 in `init_fex_cuda` when set to `false`;
  YUV400P sources always force `n_planes=1` regardless of option.
  All plane loops (`upload_frame`, `launch_plane_kernels`,
  `enqueue_partials_readback`, `collect_fex_cuda`, `close_fex_cuda`) iterate
  over `s->n_planes`, not compile-time constant `PSNR_HVS_NUM_PLANES`.
  Vulkan and SYCL twins do not yet carry this option; add it there in
  lockstep if combined-score formula diverges. `collect_fex_cuda`
  combined-score path emits luma dB only when `n_planes == 1`.

- **`integer_psnr_hvs/psnr_hvs_score.cu` parallelises only integer
  DCT passes.** First eight CUDA threads perform two 8-point
  DCT passes over shared memory; all float means, variance, masking,
  and final masked-error accumulation remain thread-0 serial in CPU
  scan order. Do not move float reductions into warp or block
  reductions without new numeric-contract ADR and refreshed
  cross-backend tolerance row.

- **`integer_psnr_hvs/psnr_hvs_score.cu` uses `__ldg()` for tile loads**
  (ADR-0764). `const float *__restrict__ ref_buf` and `dist_buf`
  extracted from `VmafCudaBuffer` struct arguments once before
  cooperative tile load; `__ldg(&ref_buf[src_idx])` and
  `__ldg(&dist_buf[src_idx])` route 128 per-block reads through
  L1 read-only texture cache. `__launch_bounds__(64)` set to
  match actual 8x8 block dispatch. Do not revert to plain
  `ref_buf[src_idx]` or pass VmafCudaBuffer by-value reads without
  first verifying compiler still emits LDG.E.CONSTANT in SASS
  (via `cuobjdump --dump-sass`).

- **`integer_cambi_cuda.c` + `integer_cambi/cambi_score.cu` are
  Strategy II hybrid** (ADR-0360 / T3-15a). GPU kernels
  (`cambi_spatial_mask_kernel`, `cambi_decimate_kernel`,
  `cambi_filter_mode_kernel`) bit-exact w.r.t. CPU
  implementation. Host residual calls `vmaf_cambi_calculate_c_values`
  and `vmaf_cambi_spatial_pooling` via `cambi_internal.h`. If upstream
  Netflix refactors `cambi.c` and renames those entry points,
  `cambi_internal.h` **and** `cambi_vulkan.c` must update in
  same PR. Never remove `cuStreamSynchronize` calls inside
  `submit_fex_cuda` — guard DtoH coherency for host
  residual. `places=4` gate load-bearing; do not loosen it.

- **`cuLaunchKernel` `kernelParams[]` must point to device-pointer
  VALUE, not to `VmafCudaBuffer` struct** (Issue lusoris/vmaf#857 /
  lusoris/vmaf#866).
  Dispatch helpers in `integer_cambi_cuda.c` (`dispatch_mask`,
  `dispatch_decimate`, `dispatch_filter_mode`) pass `&buf->data`
  (address of `CUdeviceptr` field) to `cuLaunchKernel`. Passing
  `(void *)buf` (address of struct) makes driver read
  `buf->size` (host byte count) as device pointer, causing
  immediate GPU invalid-address fault (SIGSEGV/SIGBUS on host).
  Same invariant applies to every CUDA feature extractor that
  allocates device-side flat buffers via `vmaf_cuda_buffer_alloc`
  and passes them directly to `cuLaunchKernel`: always use
  `&buf->data`, never `(void *)buf`. Device-pointer arithmetic must
  also perform on `CUdeviceptr` integer type directly —
  avoid casting through `uint8_t *` (UB even though it round-trips
  on x86-64 today).

- **`integer_psnr_cuda.c` honours `enable_chroma` option parity** (ADR-0453).
  `enable_chroma` option (default `true`) clamps `n_planes` to 1 in
  `init_fex_cuda` when set to `false`, matching CPU
  `integer_psnr.c::init`'s behaviour. Clamp runs after
  `pix_fmt == YUV400P` guard so YUV400 sources always luma-only
  regardless of option. On rebase: if upstream Netflix adds
  `enable_chroma` option to CPU path behaving differently from
  fork's GPU guard, audit both, keep GPU clamp semantically
  equivalent. SYCL and Vulkan twins carry identical guard, must
  move in lockstep with any change to this one. Cross-backend parity
  gate at `places=4` covers both `enable_chroma=true` (default) and
  `enable_chroma=false` paths.

- **Host-side preprocessing in CUDA feature extractor `submit` callbacks
  must download GPU→host first.** Pictures passed to CUDA extractor's
  `submit()` have device pointers in `data[]`; host cannot read them
  directly. Use `vmaf_cuda_picture_download_async` followed by
  `cuStreamSynchronize` on picture's private stream (obtained via
  `vmaf_cuda_picture_get_stream`) before passing picture to any
  host-side function dereferencing `data[]`. CAMBI extractor
  (`integer_cambi_cuda.c::submit_fex_cuda`) = canonical example
  of this pattern (Issue lusoris/vmaf#857, fixed by lusoris/vmaf#870).
  All other CUDA extractors here
  currently keep preprocessing on GPU, not affected,
  but rule applies to any future extractor mixing GPU input
  pictures with host-side preprocessing.

- **`integer_adm_cuda.c` must NOT include `feature/adm_options.h`
  directly.** `DEFAULT_ADM_NOISE_WEIGHT`, `DEFAULT_ADM_CSF_SCALE`,
  `DEFAULT_ADM_CSF_DIAG_SCALE`, and full 4-member
  `enum ADM_CSF_MODE` arrive transitively via
  `cuda/integer_adm_cuda.h` → `feature/integer_adm.h`. Direct
  include reintroduces 2-member `enum ADM_CSF_MODE` from
  `adm_options.h`, causes redeclaration error.

- **`integer_adm_cuda.c` / `float_adm_cuda.c` expose three ADM
  tuning parameters** (`adm_csf_scale`, `adm_csf_diag_scale`,
  `noise_weight`) with same defaults as CPU path (PR #731).
  If upstream Netflix adds or renames these parameters in
  `integer_adm.c` / `float_adm.c`, CUDA twins must update
  in same PR.

- **`float_adm_cuda.c` / `float_adm/float_adm_score.cu` AIM/ADM3
  slot-sync invariant** (ADR-0574). `FADM_ACCUM_SLOTS = 9` must
  remain identical in both files. `.cu` compile unit defines
  per-WG slot layout (`[0..2]`=csf\_den, `[3..5]`=cm\_num,
  `[6..8]`=aim\_cm); `.c` host file uses same constant for
  buffer allocation, `cuMemsetD8Async` size, D2H copy byte-count,
  and per-WG accumulator read in `collect_fex_cuda`. Slot-count
  mismatch silently overwrites adjacent host memory, or produces
  incorrect AIM/ADM3 scores without any crash.
  If `.cu` file replaced by rebase with pre-ADR-0574
  version (`FADM_ACCUM_SLOTS = 6`), update `float_adm_cuda.c`
  accordingly in same commit. `--fmad=false` nvcc flag on
  `float_adm_score.cu` covers all six kernels including two new
  AIM stages (`float_adm_csf_r`, `float_adm_aim_cm`); do not remove
  it. AIM/ADM3 options: `adm_bypass_cm`, `adm_adm3_apply_hm`,
  `adm_p_norm`, `adm_dlm_weight`, `adm_min_val`, `adm_skip_aim_scale`
  must keep same defaults as `float_adm.c`.

- **`motion_fps_weight` = cross-backend parity parameter** — all
  motion-family GPU twins must expose `motion_fps_weight` in their
  `VmafOption options[]` table, apply it identically: for
  `integer_motion_v2_*` (flush-based motion2), weight scales both
  `score_cur` and `score_next` before min in `flush()`; for
  `float_motion_*` (collect-based motion2), weight scales both
  `prev_motion_score` and `motion_score` before min in `collect()`
  (for index >= 2), scales `prev_motion_score` alone in `flush()`.
  When `motion_fps_weight = 1.0` (default), arithmetic =
  no-op; `places=4` cross-backend gate must continue to pass.
  If application math ever changes in CPU reference
  (`integer_motion_v2.c` / `float_motion.c`), all GPU twins must
  update in same PR. Twins in scope: `integer_motion_v2_cuda.c`,
  `integer_motion_v2_sycl.cpp`, `motion_v2_vulkan.c`,
  `integer_motion_v2_hip.c`, `integer_motion_v2_metal.mm`,
  `float_motion_cuda.c`, `float_motion_sycl.cpp`,
  `float_motion_vulkan.c`, `float_motion_hip.c`,
  `float_motion_metal.mm`. PR #863 initially wired this option.

- **`motion_fps_weight` applied EXACTLY ONCE on v1
  `integer_motion_*` twins** (ADR-1216) — CPU reference
  (`integer_motion.c`) scales SAD-derived score by weight in
  `extract()`, stores *weighted* value as `motion_sad_score`.
  Then blends already-weighted value into `motion2` / `motion3`
  in `flush()` without touching weight again. GPU twins mirror
  this: every caller of `motion3_postprocess_{cuda,sycl,hip}()` hands
  it value already fps-weighted and `motion_max_val`-clipped,
  so **`motion3_postprocess_*` must not multiply by
  `motion_fps_weight`**. It did until ADR-1216, squaring weight in
  `motion3_score`. Because default = `1.0` and `1.0² = 1.0`, no
  default-options parity test can see this. Guard =
  `test_{cuda,sycl,hip}_motion3_parity`
  `test_motion3_fps_weight_applied_once` variant, which pins
  `motion_fps_weight = 0.6`, reads derived
  `integer_motion3_mfw_0.6` key. Keep that variant when touching these
  twins; deleting it re-opens blind spot.

- **`integer_motion_v2_*` mirror contract** (ADR-0662) — CPU
  `integer_motion_v2.c::mirror` maps `idx >= size` to
  `2 * size - idx - 2`. CUDA, SYCL, and Vulkan `motion_v2`
  kernels must keep that same high-edge literal. Old `-1`
  formula = stale prose from ADR-0193 bring-up, creates
  measurable CPU/GPU drift.

- **`integer_motion_v2_cuda.c::flush_fex_cuda` emits `motion3_v2_score`,
  mirrors CPU `integer_motion_v2.c::flush()` formula
  byte-for-byte** (ADR-1108). CUDA twin computes
  `motion3_v2_score` host-side over kernel's SAD scores, using
  same per-frame `motion_blend(motion2, motion_blend_factor,
  motion_blend_offset)` + `MIN(…, motion_max_val)` clip + `stamp_value`
  seeding for `i < min_idx` (`min_idx = 1`) + optional 2-tap
  `motion_moving_average`. Four options
  (`motion_blend_factor`/`motion_blend_offset`/`motion_max_val`/
  `motion_moving_average`) mirror CPU `VmafOption[]` table exactly.
  Any change to CPU `motion_v2` flush blend/clip/seed/average logic
  must mirror here in same PR to keep `places=4` parity
  gate (`test_cuda_motion_v2_parity`) green. `motion2_v2_score`
  emitted via `append_with_dict` (not bare `append`), so sfr/hfr
  co-schedule names match CPU path. SYCL/HIP/Metal twins still emit
  only `sad` + `motion2_v2`; closing that gap = follow-up that must
  reuse this same host-side formula. Unlike v1 `motion_cuda` flush,
  this = batch loop over collected SAD scores (no per-frame streaming
  post-process / `frame_index` override).

- **`integer_adm/adm_cm.cu` (and rest of `integer_adm/`
  subdirectory) carries NVIDIA copyright line** alongside
  Netflix one. Upstream-mirror — keep both headers
  verbatim on rebase.

- **Integer ADM scales 1-3 keep ADR-0155's negative rounding term as
  `INT32_MIN`.** The one site in `integer_adm/adm_csf.cu` and both fused sites
  in `integer_adm/adm_cm.cu` deliberately subtract 2^31 before their 32-bit
  right shift to stay Netflix-golden compatible. Do not restore
  `1u << 31` assigned into `int32_t`: it generates NVCC diagnostic `#68-D`.
  Do not widen the constant either; that changes ADM output. Focused CUDA 13.4
  builds produced byte-identical fatbins after changing only the spelling.
  See [Research-2076](../../../../docs/research/2076-cuda-adm-signbit-warning.md).

- **Every CUDA reduce kernel SHOULD use warp-reduce + `atomicAdd_int64`
  into single accumulator; separate per-thread scratch buffer plus
  separate reduce kernel launch = pre-fix legacy pattern.**
  Scale 0 of ADM CM (`adm_cm_line_kernel_8` in `integer_adm/adm_cm.cu`)
  = canonical model: compute per-pixel result, warp-reduce
  int64 accumulator, first lane atomicAdds into `accum_global`.
  Scales 1-3 migrated to this pattern by `i4_adm_cm_line_kernel_fused`
  (PR perf/adm-cm-cuda-warp-reduce-fusion). Any future reduce kernel
  writing to scratch buffer and launching second kernel to sum it
  should refactor to fused pattern instead.

- **`kernel_template.h` mirror with HIP** (ADR-0241). CUDA
  `cuda/kernel_template.h` (one level up) and HIP
  `../hip/kernel_template.h` move in lockstep. Any change to
  CUDA template's struct fields, helper signatures, or
  semantics requires paired HIP change in same PR.
  Consumers of template (`integer_psnr_cuda.c` and
  follow-on `integer_ciede_cuda.c` / `integer_moment_cuda.c` /
  ...) lock HIP twins call-graph-for-call-graph; see
  [`../../hip/AGENTS.md`](../../hip/AGENTS.md) for full
  consumer list.

- **Kernels with high spatial overlap (>=50% redundant cross-thread reads) must
  stage into `__shared__`** (ADR-0464). `cambi_spatial_mask_kernel` sets
  precedent: 22x22 `uint8_t zd_tile[22][32]` tile populated cooperatively
  by 16x16 block (2-pass, 256 threads, 484 elements, 3x484 = 1452 global
  reads per block) before 7x7 box-sum loop reads exclusively from SLM.
  Any new stencil kernel with halo >= half block dimension and >= 50%
  cross-thread read overlap must follow same pattern: compute shared
  footprint as (BLOCK + 2*HALO)^2 elements, load cooperatively in
  ceil(N/BLOCK_AREA) passes, `__syncthreads()`, then read from SLM.
  Omitting tile for such kernels = performance regression; the
  parity-gate alone does not catch it.
- **`ssimulacra2/ssimulacra2_blur.cu` blur kernels with per-channel loops must
  fuse via `gridDim.z`** (ADR-0456). Fused kernels `ssimulacra2_blur_h3`,
  `ssimulacra2_transpose`, and `ssimulacra2_blur_v3_transposed` all take
  `plane_stride` argument, use `blockIdx.z` to select XYB channel.
  V-pass operates on column-major transposed buffer to convert stride-`width`
  per-thread access to stride-1 sequential reads. Any future blur kernel
  iterating over columns (V-direction IIR) requires preceding transpose for
  coalescing; do not skip transpose to save one launch — V-pass
  performance benefit outweighs launch cost at all resolutions ≥ 480p.

- **`ssim_cuda.c` and `integer_ssim_cuda.c` provide different features — do not
  conflate them** (ADR-0564). `ssim_cuda.c` registers `vmaf_fex_integer_ssim_cuda`,
  provides `"ssim"` (real 9-tap int64 integer SSIM, bit-exact with CPU).
  `integer_ssim_cuda.c` = historical misnomer: registers *different*
  `vmaf_fex_integer_ssim_cuda` symbol, provides `"float_ssim"` (11-tap
  floating-point Gaussian). Both symbols linked, registered in
  `feature_extractor.c`; `feature_extractor_list[]` puts `ssim_cuda.c`'s extractor
  first so `vmaf_get_feature_extractor_by_name("ssim")` resolves correctly.
  **Never swap order** of these two entries. Planned follow-up (post-ADR-0564)
  will rename `integer_ssim_cuda.c` → `float_ssim_cuda.c` to eliminate confusion;
  until then, keep naming mismatch explicit, do not merge two files.

## Build

CUDA feature TUs compile only when `meson setup -Denable_cuda=true`.
`enable_cuda` umbrella flag gates inclusion via
`#if HAVE_CUDA` blocks in `feature/feature_extractor.c`.

## Governing ADRs

- [ADR-0182](../../../../docs/adr/0182-gpu-long-tail-batch-1.md) +
  [ADR-0188](../../../../docs/adr/0188-gpu-long-tail-batch-2.md) +
  [ADR-0192](../../../../docs/adr/0192-gpu-long-tail-batch-3.md) —
  GPU long-tail batches. Every CUDA feature kernel here = row
  in one of these.
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
  Specifically: `integer_vif/filter1d.cu` stages tile of
  `(BLOCKY + fwidth - 1)` rows (vertical pass) or
  `(BLOCKX * val_per_thread + 2*half_fw + 1)` elements per channel (horizontal
  pass) into `__shared__` before convolution loop. Smem load phase
  handles mirror-boundary clamping; compute phase reads smem unconditionally.
  Any new separable filter kernel with filter width ≥ 5 must follow this pattern.
  Removing smem staging layer reverts 15–35% VIF speedup.
  See [ADR-0454](../../../../docs/adr/0454-vif-cuda-smem-staging.md) and
  [Research-0135](../../../../docs/research/0135-vif-cuda-smem-staging-2026-05-16.md).

## `__mul24` / `__umul24` prohibition (Research-0734)

**Never introduce `__mul24`, `__umul24`, or `__mul24hi` anywhere in this
directory.** NVIDIA confirmed silent data-corruption bug in these intrinsics
present since CUDA 11.1, fixed only in CUDA 13.3: any `__mul24(val, CONSTANT)`
call where one argument = compile-time constant may produce incorrect results
on PTX and SASS generated by CUDA 11.1–13.2.

2026-05-28 audit (Research-0734) confirmed zero `__mul24` calls exist in
entire `core/src/feature/cuda/` and `core/src/cuda/` trees; codebase
has no exposure to this bug. Future kernel authors must use plain C `*`
multiply operator for all integer arithmetic, which CUDA compiler handles
correctly at all toolchain versions.

If future author believes `__mul24` required for documented performance
reason (e.g. specific `dp4a` / `imad.lo.u32` targeting pattern), must:

1. Cite this invariant note in PR.
2. Demonstrate via `ncu --section InstructionStats` that performance gain
   load-bearing.
3. Obtain CODEOWNERS sign-off acknowledging minimum-CUDA-13.3 container
   constraint (CLAUDE.md §15).
4. Document constraint in `docs/backends/cuda/overview.md` under
   "Known gaps / CUDA version notes".

## extern "C" invariant — mandatory for every new CUDA kernel TU (ADR-0747)

Every `__global__` kernel that host looks up by name via
`cuModuleGetFunction` **must** be defined inside `extern "C" { }`
block in its `.cu` file.

Rationale: nvcc compiles `.cu` files as C++ by default. Without
`extern "C"`, kernel symbol receives C++ name-mangling (e.g.,
`_Z31integer_ssim_horiz_8bpc...`). `cuModuleGetFunction` uses
plain C name, receives `CUDA_ERROR_NOT_FOUND`, silently disabling
feature. Found by full sweep (Research-0747) that
identified `integer_ssim/integer_ssim_score.cu` as broken; the
pattern caused `--feature ssim --backend cuda` to fail silently from
file's introduction until this fix.

Rules:

- Wrap only `__global__` entry points. `__device__` helpers,
  `__constant__` arrays, and `#define` macros do not need wrapping.
- For macro-expanded kernel instantiations (e.g., `FILTER1D_*`
  and `ADM_CSF_KERNEL` patterns), place macro invocations inside
  `extern "C" { }` block, not macro definition.
- CI gate: `scripts/dev/check-cuda-extern-c.sh` fails if any kernel
  referenced by `cuModuleGetFunction` found outside `extern "C"`.
  Run it locally before pushing CUDA kernel changes.

See [ADR-0747](../../../../docs/adr/0747-cuda-extern-c-sweep.md).

## Register-pressure ceiling pattern for horizontal convolution kernels (ADR-0743)

- **Occupancy-critical kernels with > 48 registers per thread must carry
  `__launch_bounds__(BLOCKX, min_blocks)` where `min_blocks` satisfies
  `floor(65536 / BLOCKX / min_blocks) ≤ 48` for sm_89 target.**
  `filter1d_8_horizontal_kernel` (17-tap, vpt=2) hit 56 registers at baseline,
  capping register-limited block count at 9/SM, theoretical occupancy at 75%.
  Adding `__launch_bounds__(128, 10)` reduced registers to 48, raising theoretical
  occupancy to 83.3% (sm_89).

  Key derivation: floor(65536 / 128 / 10) = 51 registers max → ptxas allocates
  48. This = **compiler hint only** — sets maximum register budget;
  does not guarantee particular schedule.

  Caveat: on sm_75/sm_80/sm_86 (max 1024 threads/SM), `min_blocks=10` × 128 = 1280
  exceeds per-SM thread limit. ptxas emits non-fatal advisory
  "minnctapersm out of range, ignored" for those targets; those targets retain
  pre-hint register count. Acceptable for fork's primary target
  (sm_89 / RTX 4090), causes no regression on older targets.

- **Do not increase `val_per_thread` past 2 for 17-tap horizontal kernel.**
  vpt=4 evaluated during ADR-0743 profiling. smem grows 7644 → 14812 B/block;
  on sm_89 (102400 B/SM smem) this makes kernel smem-limited at 6 blocks/SM =
  37.5% occupancy — worse than 48-reg vpt=2 path at 10 blocks/SM = 62.5%.
  See [ADR-0743](../../../../docs/adr/0743-cuda-vif-filter1d-ncu-driven-perf.md).

- **`__ldg()` on read-only tmp-channel loads correct and beneficial at ≥1080p.**
  7 tmp buffers (mu1, mu2, ref, dis, ref_dis, ref_convol, dis_convol)
  written exclusively by preceding vertical pass. `__ldg()` routes their
  horizontal-pass loads through read-only L1 cache. At 576p workload
  wave-limited (0.76 waves / 128 SMs), effect neutral; at ≥1080p
  combined tmp footprint exceeds L2 capacity, cache-routing provides
  measurable L2-pressure relief. See ADR-0743.

## `__ldg()` pattern for pass-2 read-only intermediate buffers (ADR-0754)

## `__ldg()` pattern for pass-2 read-only intermediate buffers (ADR-0754, ADR-0757)

- **Extract raw `const float *__restrict__` pointers from `VmafCudaBuffer` structs
  BEFORE inner loop, then use `__ldg(&ptr[idx])` for every load.**
  `calculate_ssim_vert_combine` in `integer_ssim/ssim_score.cu` = canonical
  example: 5 horizontal-pass intermediate buffers written exclusively by
  horiz kernel, never aliased in vert pass. Passing `VmafCudaBuffer`
  by value hides pointer from compiler's non-coherent-load analysis; the
  one-time pointer extraction at kernel entry makes alias-free invariant visible,
  so `__ldg()` can route all 5×11 = 55 inner-loop loads through read-only L1
  texture cache rather than L2. Any future pass-2 kernel with similar write-once /
  read-many intermediate buffer must follow same pattern.
  See [ADR-0754](../../../../docs/adr/0754-cuda-ssim-vert-combine-ldg-pinned-leak.md).

## Pinned-host memory free invariant after `readback_free` (ADR-0754)

- **`vmaf_cuda_kernel_readback_free` NULLs `rb->host_pinned` but does NOT free it.**
  Kernel template explicitly documents this as caller responsibility (see
  comment in `cuda/kernel_template.h` near `vmaf_cuda_kernel_readback_free`). Every
  `close_fex_cuda` that calls `readback_free` must save `rb.host_pinned` to local
  BEFORE calling `readback_free`, then call `vmaf_cuda_buffer_host_free(cu_state,
  saved)` afterward. Omitting host-free leaks one page of CUDA pinned host
  memory per `vmaf_close()` cycle. `integer_ssim_cuda.c::close_fex_cuda` =
  reference fix (ADR-0754). Verify with:
  `compute-sanitizer --tool memcheck --leak-check full ./vmaf --feature float_ssim --backend cuda ...`
  — summary must show 0 bytes from `cuMemHostAlloc` after fix.
  Note: `integer_psnr_cuda.c` has same gap, scheduled for follow-up.

## Motion kernel dispatch bottleneck (Research-0760)

- **`calculate_motion_score_kernel_8bpc` dispatch-bottlenecked at all resolutions
  below 4K.** ncu profile (2026-05-29, RTX 4090) shows GPU busy fraction <1% of
  wall time at 576p (kernel 7 µs, dispatch ~12.7 ms/frame). CUDA/CPU ratio = 0.22×
  at 576p, 5.8× at 4K — crossover entirely explained by dispatch overhead.
  Any optimization not addressing per-frame dispatch will not improve
  sub-4K throughput regardless of kernel-level changes. Primary fix =
  multi-frame SAD batching (accumulate N frames before readback synchronization).
  See [Research-0760](../../../../docs/research/0760-cuda-motion-ncu-multi-resolution-20260529.md).

## Motion SAD batch fencing — MOTION_BATCH_DEPTH invariants (ADR-0845)

- **`integer_motion_cuda.c` uses MOTION_BATCH_DEPTH=8 per-slot SAD buffers to
  reduce cuStreamSynchronize from once-per-frame to once-per-8-frames (ADR-0845).**
  Key invariants must be preserved on rebase:

  1. `sad[MOTION_BATCH_DEPTH]` = ring of independent device buffers (not single
     shared accumulator). Each submit() zeroes `sad[index % MOTION_BATCH_DEPTH]`
     on pic_stream BEFORE kernel launch, so memset and atomicAdd on
     same stream (per ADR-0358 / AGENTS.md "integer_motion_cuda.c::submit_fex_cuda
     runs the SAD cuMemsetD8Async on pic_stream" invariant).
  2. `s->str` = readback drain stream. Every submit() chains its kernel-complete
     event from pic_stream to s->str via `cuStreamWaitEvent`. DtoH copies
     NOT queued in submit(); queued in batch-boundary collect() calls.
  3. Non-boundary collect() calls (where `index % MOTION_BATCH_DEPTH != MOTION_BATCH_DEPTH-1`)
     increment frame_index, return 0 without emitting scores or touching s->str.
     Emitting from non-boundary collects would break batch fence.
  4. `emit_batch_scores()` temporarily overrides `s->frame_index` to `i + 1` for
     each frame `i` in batch before calling `motion3_postprocess_cuda()`. This
     preserves moving-average guard semantics (ADR-0219). Removing or bypassing
     this frame_index override produces incorrect motion3 scores when
     `motion_moving_average=true`.
  5. drain_batch engine-scope optimization (ADR-0242) NOT used by
     integer_motion_cuda after ADR-0845. Do not re-add `vmaf_cuda_drain_batch_register_event`
     calls to submit() — would conflict with batch fence logic.
  6. flush() handles final partial batch for frame counts not
     multiple of MOTION_BATCH_DEPTH. `flush_start` clamp to 1 skips frame 0
     (has no valid SAD — kernel runs but prev_blurred uninitialized).

## Resolution-aware kernel variant dispatch (ADR-0753)

`resolution_dispatch.h` / `resolution_dispatch.c` here provide
lightweight `vmaf_cuda_workload_class(w, h)` classifier used to pick between
kernel variants at runtime. Current policy table is in ADR-0753.

**How to add a new resolution-aware variant:**

1. In extractor's `.cu` file, define two kernel entry points using sibling
   macros: one WITH `__launch_bounds__` (or other occupancy hint), one
   WITHOUT, giving no-hint variant `_no_bounds` suffix.
   Both must be inside `extern "C" { }` block (ADR-0747).
2. In extractor state struct (e.g. `AdmStateCuda`), add second
   `CUfunction` pointer for no-hint variant.
3. In `init_fex_cuda`, load both pointers via `cuModuleGetFunction`.
   Add short comment citing ADR-0753 policy (see existing examples).
4. At kernel-launch site in `submit_fex_cuda`, call
   `vmaf_cuda_workload_class(w, h)`, branch on result.
   Branch structure always single ternary / if-else — no nested
   policy trees. Consult policy table in ADR-0753 for which class gets
   bounded variant; pattern so far:
   - `adm_cm`: BOUNDED at `WS_MEDIUM` only; NO_BOUNDS at `WS_SMALL` + `WS_LARGE`.
   - `filter1d` + `ssim_vert_combine`: BOUNDED at `WS_MEDIUM` + `WS_LARGE`;
     NO_BOUNDS at `WS_SMALL` only.
5. Add row to policy table in ADR-0753 `## Decision` and to kernel
   list in `resolution_dispatch.h`.
6. Note new invariant in this file under "Rebase-sensitive invariants".
7. Update `docs/backends/cuda/overview.md` kernel table.

**Verified wirings (as of the ADR-0753 extended scope):**

| Feature | BOUNDED variant (kernel name) | NO_BOUNDS variant | Policy |
|---|---|---|---|
| `adm_cm` | `adm_cm_line_kernel_8` | `adm_cm_line_kernel_8_no_bounds` | MEDIUM only |
| `filter1d` | `filter1d_8_horizontal_kernel_2_17_9` | `filter1d_8_horizontal_kernel_2_17_9_no_bounds` | MEDIUM + LARGE |
| `ssim_vert_combine` | `calculate_ssim_vert_combine` | `calculate_ssim_vert_combine_no_bounds` | MEDIUM + LARGE |

- **`float_vif` options must be kernel ARGUMENTS, never kernel-local
  constants** (ADR-1217) — `vif_sigma_nsq` and `vif_enhn_gain_limit`
  = `VMAF_OPT_FLAG_FEATURE_PARAM` options on every float-VIF twin.
  Until ADR-1217 all three GPU compute kernels declared
  `const float vif_sigma_nsq = 2.0f; const float vif_egl = 100.0f;`
  locally. Non-default value accepted, folded into derived
  feature name, then discarded — including
  `vif_enhn_gain_limit = 1.0` that `model/vmaf_float_v0.6.1neg.json`
  sets on all four VIF scales, publishing non-NEG scores under NEG
  keys. Pass both plus host-derived `sigma_max_inv` in; missing
  kernel argument = compile error, shadowing local is not.
  `sigma_max_inv` derived on host exactly as
  `vif_tools.c::vif_statistic_s` derives it —
  `powf(nsq, 2.0f)` in `float`, divided by `255.0 * 255.0` in `double`,
  narrowed to `float` — so default path stays bit-identical; do not
  recompute it in device code. Twins in scope: `float_vif_cuda.c` (+
  `float_vif/float_vif_score.cu`, TWO `func_compute` launch sites),
  `../sycl/float_vif_sycl.cpp`, `../hip/float_vif_hip.c` (+
  `../hip/float_vif/float_vif_score.hip`). Guarded by
  `test_float_vif_options_reach_kernel` variant in each backend's
  float-VIF parity test.
- **SpEED's singular-covariance path has TWO obligations** (ADR-1202 for
  chroma, ADR-1218 for both families). 25x25 SpEED covariance =
  regular only if EVERY eigenvalue >= 1e-6; CPU treats
  singular one as routine numerical condition, not failure. Twins
  must therefore: (1) zero **DEVICE** solution `d_sol`, never
  host `h_indterm` staging buffer. Score kernel reads `d_sol`;
  `h_indterm` re-downloaded from `d_indterm` at top of every
  pipeline run, so host memset = dead code, leaves device
  solution holding previous frame's result (or, on first frame,
  raw allocator memory). (2) Report singularity through
  `singular_out` out-parameter, keeping return value reserved for
  hard device failures. Caller then applies CPU's rule in
  `speed_extract_score()` — score `0` when exactly one of ref/dis
  singular — and, for chroma, imputes `speed_chroma_uv` from
  surviving channel. Twins in scope: `speed_chroma_cuda.c`,
  `speed_temporal_cuda.c`, `../sycl/speed_chroma_sycl.cpp`,
  `../sycl/speed_temporal_sycl.cpp`, `../hip/speed_chroma_hip.c`,
  `../hip/speed_temporal_hip.c`. Guarded by
  `test_{cuda,sycl,hip}_speed_singular_parity`. Note: *existing*
  `test_*_speed_{chroma,temporal}_parity` fixtures = 768x432, whose
  chroma planes give 4x2 = 8 blocks for 25x25 covariance — singular on
  every frame, so never reach regular path at all. Any new
  SpEED test needing regular frame must be at least 960x960.
- **`float_adm` options must reach KERNELS, not option
  table alone** (ADR-1220) — `adm_p_norm` (`apn`), `adm_bypass_cm` (`bcm`)
  and, on Metal, `adm_skip_scale0` (`ssz`) =
  `VMAF_OPT_FLAG_FEATURE_PARAM` options twins declare with
  CPU's names, aliases, defaults and ranges. Until ADR-1220,
  kernels hardcoded cube sum; host pooling hardcoded
  `1.0f / 3.0f` root. So `apn` moved only AIM exponent, produced
  hybrid quantity; `bcm` was read by nothing at all (`grep`
  returned only struct field and option entry). Three rules:
  (1) `adm_p_norm` has FOUR application points — DLM numerator sum,
  CSF denominator sum, pooling root, and
  `get_noise_constant()` — change them together; (2) keep CPU's
  `p == 3` literal-cube fast path in kernel, because device
  `powf(x, 3.0f)` not guaranteed to equal `x * x * x`, and
  default path must not move; (3) `adm_bypass_cm` gates BOTH DLM
  and AIM `adm_cm()` call. Guarded by
  `test_float_adm_*_reaches_kernel` variants in each backend's
  float-ADM parity test, which read ADR-1183-derived
  `adm2_apn_2` / `adm2_bcm_1` keys — default-options test cannot
  see any of this, because `p = 3` IS hardcoded exponent.

- **MS-SSIM `clip_db` = CEILING on dB output, not clamp on
  linear score** (ADR-1221) — `float_ms_ssim.c` derives
  `max_db = ceil(10 * log10(peak * peak / mse))` with
  `mse = 0.5 / (w * h)` at `init()`; `convert_to_db()` returns
  `MIN(-10*log10(1 - score), max_db)`, short-circuiting to `max_db`
  when `score >= 1.0`. Until ADR-1221 all three twins clamped
  LINEAR score into `[0, 1]`, converted with no ceiling; none
  had `max_db` field: identical reference/distorted pair returned
  `+Inf`, every high-similarity pair returned uncapped dB value.
  Keep `max_db` and `ms_ssim_convert_to_db()` in sync with CPU
  across `integer_ms_ssim_cuda.c`, `../sycl/integer_ms_ssim_sycl.cpp`
  and `../hip/integer_ms_ssim_hip.c`. Guard =
  `test_*_ms_ssim_*clip_db_ceiling` variant, which must feed
  IDENTICAL pair — on merely high-similarity fixture ceiling
  never binds, variant passes against unfixed twin.

- **`integer_vif_cuda.c::filter1d_8` picks `filter1d_8_horizontal_kernel_2_17_9_no_bounds`
  at `WS_SMALL`, bounded variant at `WS_MEDIUM`/`WS_LARGE`** (ADR-0753 extended
  scope). `VifStateCuda` carries `func_filter1d_8_horizontal_kernel_2_17_9_no_bounds`.
  `filter1d.cu` defines `FILTER1D_8_HORI_NO_BOUNDS(2, 17, 9)` inside `extern "C" {}`.
  On rebase: verify both `cuModuleGetFunction` calls in `init_fex_cuda` reference
  valid symbols. If upstream refactors macro or adds new `fwidth` variants,
  apply `_NO_BOUNDS` sibling macro around new body too.

- **`integer_ssim_cuda.c::submit_fex_cuda` picks `calculate_ssim_vert_combine_no_bounds`
  at `WS_SMALL`, bounded variant at `WS_MEDIUM`/`WS_LARGE`** (ADR-0753 extended
  scope). `SsimStateCuda` carries `func_vert_no_bounds`.
  `integer_ssim/ssim_score.cu` defines both variants inside `extern "C" {}`.
  On rebase: if upstream modifies `calculate_ssim_vert_combine`, apply same
  diff to `calculate_ssim_vert_combine_no_bounds` (body identical; only
  `__launch_bounds__(128)` annotation differs).

## `__ldg()` pattern for VmafPicture channel reads (ADR-0762)

- **Extract typed `const uint8_t *__restrict__` (or `uint16_t *__restrict__` for
  16bpc) channel pointers from `VmafPicture` struct args BEFORE per-pixel body,
  then use `__ldg(&ptr[idx])` for all channel reads.**
  `calculate_ciede_kernel_8bpc` and `calculate_ciede_kernel_16bpc` in
  `integer_ciede/ciede_score.cu` = canonical examples: `VmafPicture` struct
  carries `void *data[3]`, which prevents compiler from seeing reads are
  alias-free when struct passed by value. Extracting typed `__restrict__`
  pointers at kernel entry makes invariant visible, routes 6 per-pixel
  channel reads through L1 read-only texture cache. Any future kernel reading
  per-pixel plane data from `VmafPicture` must follow same pattern.
  See [ADR-0762](../../../../docs/adr/0762-cuda-ciede-ldg.md).

- **`integer_adm/adm_decouple.cu` carries same F3 `__ldg()` fix (ADR-0763).**
  `adm_decouple_kernel` (scale-0, `int16_t`) and `adm_decouple_s123_kernel`
  (scales 1-3, `int32_t`) both extract `const T *__restrict__` read-only band
  pointers before per-pixel body, use `__ldg()` for all reads. Write-back
  band pointers plain non-`const` (no `__ldg()` on stores). Note: file
  currently dead (decouple computation inlined into `adm_csf.cu` /
  `adm_cm.cu` via `adm_decouple_inline.cuh`) — rebase on change to that file
  does NOT affect `adm_decouple.cu`.
  See [ADR-0763](../../../../docs/adr/0763-cuda-adm-decouple-ldg.md).

- **`integer_adm/adm_cm.cu` `x_sq` reduction requires explicit parentheses around
  `add_shift_sq` before right-shift (r6-cuda-kernel / 2026-06-04).** Expression
  `(int64_t)accum * accum + add_shift_sq >> shift_sq` parsed by C++ as
  `+ (add_shift_sq >> shift_sq)` = `+ 0` because `>>` binds tighter than `+`.
  Correct form = `((int64_t)accum * accum + add_shift_sq) >> shift_sq`, matching
  CPU reference macro `I4_ADM_CM_ACCUM_ROUND` in `integer_adm.c:743` and fused
  kernel at `adm_cm.cu:259`. Defect affected two reduction loops in file
  (lines 373 and 712). On rebase: if either loop modified, verify parenthesisation
  of `x_sq` computation before pushing.

- **`integer_vif/filter1d.cu` 16-bit rd-filter upper-bound guard must use
  `(fwidth - fwidth_rd)`, not `(fwidth_rd - fwidth_rd)` (r6-cuda-kernel / 2026-06-04).**
  Correct guard = `fi < (fwidth - (fwidth - fwidth_rd) / 2)`, matching 8-bit
  form at line 183. Writing `(fwidth_rd - fwidth_rd)` (always zero) widens tap
  window to all `fwidth` taps, causes OOB reads into `vif_filt.filter[scale+1]`.
  On rebase: if vertical-pass loop in 16-bit path modified, verify
  upper-bound guard expression before pushing.

- **`integer_adm/adm_csf.cu` and `integer_adm/adm_cm.cu` carry F3 `__ldg()` fix on
  active path (ADR-0773).** Six inline `__device__` helpers in `adm_cm.cu`
  (`inline_i4_csf_a`, `inline_i4_decouple_r`, `inline_s0_csf_a`, `inline_s0_decouple_r`,
  `inline_i4_csf_r`, `inline_s0_csf_r`) and two kernel templates in `adm_csf.cu`
  (`i4_adm_csf_kernel<>`, `adm_csf_kernel<>`) all extract `const T *__restrict__` band
  pointers from `cuda_*_adm_dwt_band_t` structs before any indexed load. All six per-pixel
  DWT2 band reads use `__ldg()`. When rebasing or modifying these helpers: preserve
  `__restrict__` extraction pattern; do not add writes through these pointers (they are
  read-only inputs). ADR-0773 completes ADR-0756 `adm_decouple` dispatch item.
  See [ADR-0773](../../../../docs/adr/0773-cuda-adm-decouple-inline-ldg.md).

- **The `*_unwind` / `*_init_unwind` helpers are the single teardown path for
  their extractor, and the arithmetic helpers must stay `static` in the same
  translation unit (HISS-21 / 2026-09-21).** Every extractor's old `free_ref` /
  `free_buffers` / `fail_cuda` label now lives in one `static` helper that holds
  the label's statements verbatim; callers pass their live `err` / `ret` so the
  returned code is unchanged. `ssim_cuda.c`'s `issim_init_unwind` takes an
  `ISSIM_UNWIND_*` stage because it replaced a three-level fall-through cascade —
  adding a resource means adding a stage, not a second exit path. The helpers
  carrying score arithmetic (`integer_ssim_setup_geometry`'s `c1` / `c2`,
  `motion_v2_stamp_value`, `motion_v2_emit_frame`, `float_psnr_peak_for_bpc`)
  must stay `static` and keep each expression in one statement: moving an operand
  across a call boundary, or letting one of these become external, re-opens the
  FMA-contraction divergence ADR-1253 closed. On rebase: reapply the helper
  boundary, never restore the label.
  `integer_adm_cuda.c` is out of scope for the helper NAMES above: #1507
  rewrote its init and teardown while this branch was open, so it releases
  through paired helpers (`adm_cuda_init_device` / `adm_cuda_release_device`,
  `adm_cuda_init_buffers` / `adm_cuda_free_buffers`,
  `adm_cuda_load_modules` / `adm_cuda_unload_modules`) and keeps one
  `CHECK_CUDA_GOTO` ladder inside `adm_cuda_init_device_locked()`, the same
  macro the rest of the CUDA tree uses. The rules are unchanged for it: the
  release set and release order on every exit path, a real error code out of
  every failure, and no arithmetic expression split across a boundary.

## Integer ADM tiny frames (T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18)

- `init_fex_cuda()` calls `adm_frame_size_check()` first, before any device
  resource. Bound = CPU bound (17x17).
- Shift rounding constant = `adm_half_shift(x)`. Never `1 << (x - 1)`:
  scale-0 h/v cube shift = 0 at frame width 17..32 -> 2^31 on x86.
- Scale-0 CM kernels (`adm_cm_line_kernel`, `adm_cm_aim_line_kernel`):
  `x - 1`, `y - 1` -> `abs()`; `x + 1` -> `min(.., w - 1)`;
  `y + 1` -> `min(.., h - 1)`. Same rule as CPU `adm_cm_thresh()` (ADR-1210).
  Edges enter CM region only for bands <= 14 samples.
- Upstream Netflix form differs; keep fork form on sync (`docs/rebase-notes.md`).
- Guard: `test_cuda_adm_tiny_frames` (scalar CPU ref, 1e-4; rejection test
  needs no device).

## Integer ADM 16-bit vertical DWT sums in int64 (T-GPU-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18)

- `core/src/feature/cuda/integer_adm/adm_dwt2.cu`, scale-0 fused kernel: vertical accumulator = `DwtVertAccum<T>::type`
  -> int64 for `uint16_t`, int32 for `uint8_t`.
- Low-pass taps 1-3 sum 50582 -> int32 sum overflows (UB) once 3 16-bit
  samples >= 42456. CPU twin: `adm_dwt2_vpass16_tap4()` (int64).
- Normalised value fits int32 -> int64 form = old wrapped result. Scores
  identical; never narrow back to int32 for speed.
- Guard: `test_gpu_adm_bright_16bit_parity` in `test_gpu_adm_tiny_frames.c`
  (parity only; device wrap hides the UB itself).
