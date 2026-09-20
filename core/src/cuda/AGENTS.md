<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src/cuda

Orientation for agents working on CUDA backend runtime. Parent:
[../../AGENTS.md](../../AGENTS.md).

## Scope

CUDA-side runtime (picture lifecycle, buffer pools, launch helpers).
CUDA **feature kernels** live one level deeper in
[../feature/cuda/](../feature/cuda/). CUDA execution-provider wiring for
ONNX Runtime lives in [../dnn/](dnn/AGENTS.md).

```text
cuda/
  common.c/.h          # CUDA context + stream management
  cuda_helper.cuh      # launch macros, error-check, types
  kernel_template.h    # per-feature CUDA kernel scaffolding (ADR-0246)
  picture_cuda.c/.h    # VmafPicture on a CUDA device
  # picture-pool round-robin lives in core/src/gpu_picture_pool.{h,c}
  # (ADR-0239 — backend-agnostic; CUDA was the original consumer)
```

## Ground rules

- **Parent rules** apply in full: see [../../AGENTS.md](../../AGENTS.md).
- **Every CUDA call has its error checked.** Use `cuda_helper.cuh`
  macros; don't invent new wrappers silently.
- **`cudaMemcpyAsync` requires pinned host memory** for true async;
  using pageable host buffers silently serialises. Picture pools
  allocate pinned.
- **Experimental toolchain flags enabled**
  (`--expt-relaxed-constexpr`, `--extended-lambda`,
  `--expt-extended-lambda`; Blackwell `sm_120` gencode). See
  [ADR-0027](../../../docs/adr/0027-non-conservative-image-pins.md).
  "Experimental" means *feature flags on stable CUDA ≥13.2*, not
  preview branches.
- **Numerical snapshots**: kernels that cannot bit-match CPU
  reference regenerate `testdata/scores_cpu_cuda.json` via
  [`/regen-snapshots`](../../../.claude/skills/regen-snapshots/SKILL.md)
  with justification in commit message. See
  [CLAUDE.md §9](../../../CLAUDE.md).

## Rebase-sensitive invariants

- **CUDA architecture floor = compute capability 8.0 (Ampere)**
  (ADR-1223) — gencode list in `core/src/meson.build` emits cubins
  for `sm_80` / `sm_86` / `sm_89` (plus `sm_90` / `sm_100` / `sm_120`
  when host nvcc supports them) and `compute_80` PTX as backward-JIT
  floor. Nothing below 8.0 emitted: Turing (`sm_75`) dropped, same
  for CUDA-12-only `compute_50` PTX. Two consequences for anyone
  touching this: (1) `vmaf_cuda_state_init()` enforces floor at
  runtime via `check_device_arch()`. Runs on BOTH primary-context and
  provided-context paths — dropping gencode entry without guard
  turns unsupported GPU into opaque `CUDA_ERROR_NO_BINARY_FOR_GPU`
  from whichever extractor loaded first; (2) floor lives in exactly
  two places that must move together: `VMAF_CUDA_MIN_COMPUTE_MAJOR` /
  `_MINOR` in `common.h`, gencode list in `meson.build`. Upstream
  Netflix still ships `sm_75`; rebase taking their gencode block
  wholesale reintroduces it. Guarded by
  `core/test/test_cuda_arch_floor.c`, pure-function test — no runner
  in fleet has Turing GPU to test rejection path on.

- **`picture_cuda.c` picture-upload stream must use
  `CU_STREAM_NON_BLOCKING`** (fork-local, ADR-0378):
  `vmaf_cuda_picture_alloc` creates per-picture upload stream with
  `cuStreamCreateWithPriority(..., CU_STREAM_NON_BLOCKING, 0)`.
  `CU_STREAM_DEFAULT` (flag = 0) participates in CUDA legacy
  null-stream implicit serialisation rule. Serialises every
  per-frame upload with all other streams in context, reducing
  motion throughput at sub-4K from expected ≥5x CPU speedup to 0.55x
  CPU. **On rebase**: upstream introducing `cuStreamCreate` with
  default flag at this site -> replace immediately with
  `cuStreamCreateWithPriority(..., CU_STREAM_NON_BLOCKING, 0)`. All
  three runtime stream-creation sites in fork (`common.c`,
  `picture_cuda.c`, per-extractor `init_fex_cuda`) must consistently
  use `CU_STREAM_NON_BLOCKING`. See
  [ADR-0378](../../../docs/adr/0378-picture-stream-non-blocking.md)
  and rebase-notes entry for PR #695.

- **`picture_cuda.c` synchronous free**: `vmaf_cuda_picture_free`
  deliberately calls `cuMemFree` — *not* `cuMemFreeAsync`. Previous
  `cuStreamSynchronize` already drains pending work; stream handle
  about to be destroyed. Async variant asserted
  (`Assertion 0 failed`) with two or more concurrent CUDA sessions. Upstream
  carries this fix in
  [Netflix#1382](https://github.com/Netflix/vmaf/pull/1382), still
  OPEN as of 2026-04-20. Rebase reintroducing `cuMemFreeAsync` here —
  whether from upstream merge of #1382 (unlikely to conflict, same
  substance) or refactor that "restores async symmetry" — keep
  synchronous form. Async variant = multi-session data hazard, not
  perf optimisation. Tracker:
  [Netflix#1381](https://github.com/Netflix/vmaf/issues/1381). See
  [ADR-0131](../../../docs/adr/0131-port-netflix-1382-cumemfree.md)
  and [rebase-notes 0031](../../../docs/rebase-notes.md).

- **`vmaf_cuda_state_free()` ownership contract** (fork-local,
  ADR-0157): public API at
  [`include/libvmaf/libvmaf_cuda.h`](../../include/libvmaf/libvmaf_cuda.h)
  now includes `vmaf_cuda_state_free(VmafCudaState *cu_state)`.
  Ownership model: `vmaf_cuda_state_init` allocates →
  `vmaf_cuda_import_state` copies-by-value (no ownership transfer)
  → `vmaf_close` destroys CUDA stream + context + frees
  `CudaFunctions` driver table (via fork-local
  `cuda_free_functions()` call in `vmaf_cuda_release`) →
  `vmaf_cuda_state_free` frees heap allocation itself. Call order
  `vmaf_close → vmaf_cuda_state_free → vmaf_model_destroy`
  load-bearing; reversing first two = use-after-free. Mirrors SYCL
  backend's `vmaf_sycl_state_free()` pattern. **On rebase**: keep
  fork's public symbol; upstream doesn't have this API as of
  2026-04-24. See
  [ADR-0157](../../../docs/adr/0157-cuda-preallocation-leak-netflix-1300.md)
  and [rebase-notes 0050](../../../docs/rebase-notes.md).
- **`vmaf_gpu_picture_pool_close` mutex destroy order** (fork-local,
  ADR-0157, promoted out of `cuda/` to `core/src/gpu_picture_pool.c`
  per ADR-0239): function does `pthread_mutex_unlock` →
  `pthread_mutex_destroy` → `free(pic)` → `free(pool)`. Destroying
  locked mutex = POSIX UB; old code destroyed it locked. On rebase:
  keep unlock-before-destroy order.

- **`CHECK_CUDA` graceful error propagation** (fork-local,
  ADR-0156): `CHECK_CUDA` macro in
  [`cuda_helper.cuh`](cuda_helper.cuh) does NOT call `assert(0)` on
  CUDA errors — replaced wholesale by
  `CHECK_CUDA_GOTO(funcs, CALL, label)` and
  `CHECK_CUDA_RETURN(funcs, CALL)`, which log + return
  `-errno` translated from `CUresult` via
  `vmaf_cuda_result_to_errno`. All 178 call sites across `common.c`,
  `picture_cuda.c`, `libvmaf.c`, and three `feature/cuda/*.c` feature
  extractors use new macros. Twelve `static` helpers
  (`calculate_motion_score`, `filter1d_8/16`, `adm_dwt2_*_device`,
  `adm_csf_device`, `i4_adm_csf_device`, `adm_csf_den_*_device`,
  `adm_cm_device`, `i4_adm_cm_device`, `integer_compute_adm_cuda`)
  are `int`-returning to carry errors upward. **On rebase**: keep
  fork's macro definitions and every cleanup-label pattern; upstream
  still uses `assert(0)` as of 2026-04-24. Upstream port adding new
  `CHECK_CUDA(...)` sites -> rewrite to graceful variants inside port
  commit. See
  [ADR-0156](../../../docs/adr/0156-cuda-graceful-error-propagation-netflix-1420.md)
  and [rebase-notes 0049](../../../docs/rebase-notes.md).

- **`integer_psnr_hvs_cuda.c` async-upload + persistent pinned
  staging** (fork-local, T-GPU-OPT-2/3): file deliberately does NOT
  use `kernel_template.h`'s single-readback shape. Carries own
  dedicated H2D `upload_str` stream + cross-stream `upload_done`
  event, plus per-plane persistent pinned `h_uint_*` staging buffers
  allocated once in `init_fex_cuda`, reused every frame.
  `submit_fex_cuda` flow = three explicit phases: queue all 6 D2H
  copies on pic streams → host-block on each pic stream → CPU
  normalise uint→float → queue all 6 H2Ds on `upload_str` → record
  `upload_done` → `cuStreamWaitEvent(s->lc.str, upload_done, ...)`
  before kernel launches. Only CUDA feature extractor where
  `upload_plane_cuda` lived as local helper (other CUDA extractors
  upload through picture pool); helper now split into
  `issue_d2h_plane` / `convert_plane` / `issue_h2d_plane`. **On
  rebase**: do NOT collapse three-phase flow back into per-call sync
  pattern; do NOT migrate upload into `kernel_template.h` (template's
  single-readback bundle doesn't model 6-buffer multi-plane
  uploads). Keep persistent pinned buffer lifecycle in `init` /
  `close`. CUDA graph capture (future T-GPU-OPT-N) depends on
  no-per-frame-alloc invariant from this change.

- **`integer_ms_ssim_cuda.c` per-scale partials topology**
  (fork-local, T-GPU-OPT-2 / ADR-0271): file allocates **per-scale**
  device + pinned-host partials buffers (`l_partials[MS_SSIM_SCALES]`,
  `c_partials[...]`, `s_partials[...]`, matching `h_*_partials[...]`).
  All 5 SSIM scales' `horiz` + `vert_lcs` launches and DtoH copies
  enqueue back-to-back on `s->lc.str` inside `submit()`;
  `cuEventRecord(s->lc.finished, s->lc.str)` recorded once after last
  DtoH, registered with `vmaf_cuda_drain_batch_register(&s->lc)` so
  engine's batched drain (`drain_batch.h`) covers this extractor.
  Shared SSIM intermediate buffers (`h_ref_mu`, `h_cmp_mu`,
  `h_ref_sq`, `h_cmp_sq`, `h_refcmp`) stay shared: same-stream
  ordering serialises per-scale `horiz ⇒ vert_lcs ⇒ DtoH` chain
  naturally. **On rebase**: do NOT collapse per-scale partials arrays
  back to single buffers. Host-side reduction loop in `collect()`
  walks all 5 scales' `h_*_partials[i]` after engine drains; aliasing
  buffers would force per-scale `cuStreamSynchronize`, break
  drain_batch coalesce. Do NOT parallelise per-scale work onto
  multiple streams — would break same-stream serialisation that
  makes shared intermediates safe. See
  [ADR-0271](../../../docs/adr/0271-cuda-drain-batch-ms-ssim.md)
  and [rebase-notes 0228](../../../docs/rebase-notes.md).

- **`integer_psnr_cuda.c` chroma-extension invariants** (fork-local,
  T3-15(b) / ADR-0351): kernel
  (`calculate_psnr_kernel_{8,16}bpc` in
  `feature/cuda/integer_psnr/psnr_score.cu`) takes `plane` parameter
  (`unsigned`) appended after `(width, height)`. Host's
  `psnr_cuda_dispatch` packs `kernelParams[]` in exact
  `(ref, dis, sse, &width, &height, &plane)` order —
  `cuLaunchKernel` cannot
  validate argument types, so any reorder / drop of trailing
  argument silently corrupts plane indexing. State carries one
  `VmafCudaKernelReadback` per plane (`rb[3]`); per-frame async
  lifecycle stays singleton — all per-plane launches enqueue
  back-to-back on picture stream, all per-plane DtoHs enqueue
  back-to-back on `lc.str` (no inter-plane barrier — accumulators
  independent). Host relies on
  `libvmaf.c::translate_picture_host`'s `upload_mask` having
  uploaded all 3 planes for non-`YUV400P` inputs. **On rebase**: do
  NOT collapse `rb[3]` array back to singleton; do NOT remove `plane`
  kernel argument; do NOT add per-extractor `needs_chroma` flag that
  would let future "minimise upload" optimisation skip chroma —
  chroma kernels (`ciede_cuda`, `psnr_cuda`'s chroma branch) silently
  break if upload mask narrowed. See
  [ADR-0351](../../../docs/adr/0351-cuda-chroma-psnr.md) and
  [rebase-notes 0320](../../../docs/rebase-notes.md).

- **`kernel_template.h` = canonical kernel scaffolding** (fork-local,
  ADR-0246): inline helpers `vmaf_cuda_kernel_lifecycle_init/_close`,
  `vmaf_cuda_kernel_readback_alloc/_free`,
  `vmaf_cuda_kernel_submit_pre_launch`, and
  `vmaf_cuda_kernel_collect_wait` capture private non-blocking
  stream + 2-event + device-accumulator + pinned-readback shape
  every fork-added CUDA feature kernel uses. Templates land unused
  in PR #NNN — each future kernel migration = its own gated PR
  (`places=4` cross-backend-diff per ADR-0214). **On rebase**: keep
  both header and any kernel call-sites that later adopt it;
  upstream has no equivalent. Reference implementation mirroring
  template's shape lives in
  `core/src/feature/cuda/integer_psnr_cuda.c`. See
  [ADR-0246](../../../docs/adr/0246-gpu-kernel-template.md) and
  [docs/backends/kernel-scaffolding.md](../../../docs/backends/kernel-scaffolding.md).

## Per-kernel nvcc flag invariants

- `cuda_cu_extra_flags` map in `core/src/meson.build` routes
  per-kernel nvcc flags. Currently inhabited by `float_adm_score`
  (added in PR #157,
  [ADR-0202](../../../docs/adr/0202-float-adm-cuda-sycl.md)) and
  `ssimulacra2_blur` (added in
  [ADR-0206](../../../docs/adr/0206-ssimulacra2-cuda-sycl.md)). Both
  pass `-Xcompiler=-ffp-contract=off --fmad=false` so recursive /
  cross-band float reductions keep their CPU-port FMUL/FSUB
  ordering. **On rebase**: never drop these per-kernel entries —
  without them, `float_adm` drifts past `places=4` at scale 3,
  `ssimulacra2`'s pooled score drifts past `places=2` through IIR +
  6-scale pyramid.
- Matching `ssimulacra2_mul` fatbin = single FMUL with no fused-add
  candidate. Intentionally does **not** carry flag — keeping FMA on
  kernels where it isn't precision risk preserves whatever
  optimisation NVCC can apply.

## Lifecycle invariants

- **`cuModuleLoadData` requires paired `cuModuleUnload` in
  `close()`** — modules carry GPU-resident backing storage (~200-500
  KB per module on consumer GPUs), survives `cuStreamDestroy` and
  (for primary contexts) `cuCtxDestroy`.
  `compute-sanitizer --tool memcheck` does **not** report module leaks (tool tracks
  `cuMem*Alloc` only) — why leak in `ssimulacra2_cuda` survived
  initial review. Adding `cuModuleLoadData` call -> add guarded
  `cuModuleUnload` in matching `close_fex_cuda` after
  `cuStreamSynchronize`, before `cuStreamDestroy`. Reference fix:
  [ADR-0356](../../../docs/adr/0356-ssimulacra2-cuda-leaks-perf.md)
  (`ssimulacra2_cuda` had two unloaded modules — `module_blur` +
  `module_mul`).
- **Drain batch belongs to one engine at a time.** `drain_batch.c`'s
  `g_drain_batch` thread-local (ADR-0242), but two `VmafContext`s can
  run on one OS thread, so it carries owning `VmafCudaState`:

  - `vmaf_cuda_drain_batch_open()` takes that state, drops entries
    left by different owner.
  - `vmaf_cuda_drain_batch_flush()` returns 0 without touching CUDA
    when caller isn't owner; clears entries it consumed.
  - `vmaf_cuda_drain_batch_thread_destroy()` wipes entries, open
    flag, and owner before engine state freed.

  Never restore owner-less `open(void)` signature: without it,
  closed context leaves dangling `CUevent`s and freed `bool *` flags
  for next one (docs/state.md
  T-UPSTREAM-1305-CUDA-DRAIN-BATCH-THREAD-GLOBAL-2026-09-03, pinned
  by `core/test/test_cuda_drain_batch.c`).

## Governing ADRs

- [ADR-0022](../../../docs/adr/0022-inference-runtime-onnx.md) — CUDA execution provider mapping.
- [ADR-0027](../../../docs/adr/0027-non-conservative-image-pins.md) — CUDA 13.2 + experimental flags.
- [ADR-0131](../../../docs/adr/0131-port-netflix-1382-cumemfree.md) —
  `vmaf_cuda_picture_free` synchronous free.
- [ADR-0202](../../../docs/adr/0202-float-adm-cuda-sycl.md) —
  `float_adm_cuda` requires `--fmad=false` on its fatbin to
  match GLSL `precise` qualifier in `float_adm.comp`. See
  per-kernel `cuda_cu_extra_flags` dict in
  `core/src/meson.build`. **On rebase**: do not consolidate
  `float_adm_score` into global `cuda_flags` block; FMA-off
  scope intentionally one fatbin only.

- **FFmpeg `libvmaf` filter — `cuda` selector consumer** (fork-local,
  ADR-0350): in-tree
  [`ffmpeg-patches/0010-libvmaf-wire-cuda-backend-selector.patch`](../../../ffmpeg-patches/0010-libvmaf-wire-cuda-backend-selector.patch)
  consumes public CUDA C-API surface
  (`vmaf_cuda_state_init` / `_state_free` / `_import_state` /
  `_preallocate_pictures` / `_fetch_preallocated_picture` from
  [`include/libvmaf/libvmaf_cuda.h`](../../include/libvmaf/libvmaf_cuda.h))
  exactly like SYCL/Vulkan do via patches `0003`/`0004`. **On
  rename / signature change of any of those entry points**: FFmpeg
  patch must update in same PR per CLAUDE.md §12 r14. Verify by
  cumulative `git am --3way` replay of every entry in
  `ffmpeg-patches/series.txt` against pristine FFmpeg `n9.0.2`.
  CUDA filter selector mirrors picture-pool ownership contract above: state freed
  *after* `vmaf_close()`. Reversing order = use-after-free.

## Build

```bash
meson setup build -Denable_cuda=true -Denable_sycl=false
ninja -C build
```

Requires `/opt/cuda` + `nvcc` on PATH.

## Per-plane kernels: the signature must match `kernelParams` by count (ADR-1215)

`cuLaunchKernel` does not validate that kernel consumes every entry
of `kernelParams` array — kernel declared with five parameters,
launched with six, ignores sixth entirely. That's how
`calculate_psnr_kernel_16bpc` shipped reading `data[0]` for every
plane while host faithfully passed `plane`. Host dispatch shared
between bit-depth variants -> diff two kernel signatures against
params array before trusting parity test. Also make sure fixture's
chroma isn't flat, or wrong plane reads same sentinel as right one.

## Teardown helpers replace the cleanup labels (HISS-21 / 2026-09-21)

`common.c` and `picture_cuda.c` have no explicit `goto` left. `provided_ctx_unwind` holds what the
`fail` -> `fail_after_stream` fall-through used to do, and takes `ctx_pushed` so the caller says
whether the context pop still has to run; `pinned_alloc_unwind` takes a `PINNED_UNWIND_*` stage
because it replaced the `free_priv` -> `free_data` -> `fail_no_data` cascade. Adding a resource to
either path means adding a stage to the helper, not a second exit. The `CHECK_CUDA_GOTO` labels stay
— they are the macro's jump targets and now just call the helper.
