<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src/cuda

Orientation for agents working on the CUDA backend runtime. Parent:
[../../AGENTS.md](../../AGENTS.md).

## Scope

The CUDA-side runtime (picture lifecycle, buffer pools, launch helpers).
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

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
- **Every CUDA call has its error checked.** Use the `cuda_helper.cuh`
  macros; don't invent new wrappers silently.
- **`cudaMemcpyAsync` requires pinned host memory** for true async; using
  pageable host buffers silently serialises. Picture pools allocate pinned.
- **Experimental toolchain flags are enabled** (`--expt-relaxed-constexpr`,
  `--extended-lambda`, `--expt-extended-lambda`; Blackwell `sm_120` gencode).
  See [ADR-0027](../../../docs/adr/0027-non-conservative-image-pins.md).
  "Experimental" means *feature flags on stable CUDA ≥13.2*, not preview
  branches.
- **Numerical snapshots**: kernels that cannot bit-match the CPU reference
  regenerate `testdata/scores_cpu_cuda.json` via
  [`/regen-snapshots`](../../../.claude/skills/regen-snapshots/SKILL.md)
  with a justification in the commit message. See
  [CLAUDE.md §9](../../../CLAUDE.md).

## Rebase-sensitive invariants

- **`picture_cuda.c` picture-upload stream must use `CU_STREAM_NON_BLOCKING`**
  (fork-local, ADR-0378): `vmaf_cuda_picture_alloc` creates the per-picture
  upload stream with `cuStreamCreateWithPriority(..., CU_STREAM_NON_BLOCKING, 0)`.
  Using `CU_STREAM_DEFAULT` (flag = 0) participates in the CUDA legacy
  null-stream implicit serialisation rule and serialises every per-frame
  upload with all other streams in the context, reducing motion throughput
  at sub-4K from the expected ≥5x CPU speedup to 0.55x CPU. **On rebase**:
  if upstream introduces a `cuStreamCreate` with default flag at this site,
  replace it with `cuStreamCreateWithPriority(..., CU_STREAM_NON_BLOCKING, 0)`
  immediately. All three runtime stream-creation sites in the fork
  (`common.c`, `picture_cuda.c`, per-extractor `init_fex_cuda`) must
  consistently use `CU_STREAM_NON_BLOCKING`. See
  [ADR-0378](../../../docs/adr/0378-picture-stream-non-blocking.md) and
  the rebase-notes entry for PR #695.

- **`picture_cuda.c` synchronous free**: `vmaf_cuda_picture_free`
  deliberately calls `cuMemFree` — *not* `cuMemFreeAsync` — because
  the previous `cuStreamSynchronize` already drains any pending work
  and the stream handle is about to be destroyed, which made the
  async variant assert (`Assertion 0 failed`) with two or more
  concurrent CUDA sessions. Upstream carries this fix in
  [Netflix#1382](https://github.com/Netflix/vmaf/pull/1382), still
  OPEN as of 2026-04-20. If a rebase reintroduces `cuMemFreeAsync`
  here — whether from an upstream merge of #1382 (unlikely to
  conflict, this is the same substance) or a refactor that "restores
  async symmetry" — keep the synchronous form. The async variant is
  a multi-session data hazard, not a perf optimisation. Tracker:
  [Netflix#1381](https://github.com/Netflix/vmaf/issues/1381). See
  [ADR-0131](../../../docs/adr/0131-port-netflix-1382-cumemfree.md)
  and [rebase-notes 0031](../../../docs/rebase-notes.md).

- **`vmaf_cuda_state_free()` ownership contract** (fork-local,
  ADR-0157): the public API at
  [`include/libvmaf/libvmaf_cuda.h`](../../include/libvmaf/libvmaf_cuda.h)
  now includes `vmaf_cuda_state_free(VmafCudaState *cu_state)`.
  Ownership model: `vmaf_cuda_state_init` allocates →
  `vmaf_cuda_import_state` copies-by-value (no ownership transfer)
  → `vmaf_close` destroys the CUDA stream + context + frees the
  `CudaFunctions` driver table (via fork-local
  `cuda_free_functions()` call in `vmaf_cuda_release`) →
  `vmaf_cuda_state_free` frees the heap allocation itself. The
  call order `vmaf_close → vmaf_cuda_state_free → vmaf_model_destroy`
  is load-bearing; reversing the first two is a
  use-after-free. Mirrors the SYCL backend's
  `vmaf_sycl_state_free()` pattern. **On rebase**: keep the
  fork's public symbol; upstream doesn't have this API as of
  2026-04-24. See
  [ADR-0157](../../../docs/adr/0157-cuda-preallocation-leak-netflix-1300.md)
  and [rebase-notes 0050](../../../docs/rebase-notes.md).
- **`vmaf_gpu_picture_pool_close` mutex destroy order** (fork-local,
  ADR-0157, promoted out of `cuda/` to `core/src/gpu_picture_pool.c`
  per ADR-0239): the function does
  `pthread_mutex_unlock` → `pthread_mutex_destroy` → `free(pic)`
  → `free(pool)`. Destroying a locked mutex is POSIX UB; the old code
  destroyed it locked. On rebase: keep the unlock-before-destroy order.

- **`CHECK_CUDA` graceful error propagation** (fork-local,
  ADR-0156): the `CHECK_CUDA` macro in
  [`cuda_helper.cuh`](cuda_helper.cuh) does NOT call
  `assert(0)` on CUDA errors — it was replaced wholesale by
  `CHECK_CUDA_GOTO(funcs, CALL, label)` and
  `CHECK_CUDA_RETURN(funcs, CALL)` which log + return a
  `-errno` translated from `CUresult` via
  `vmaf_cuda_result_to_errno`. Every one of the 178 call
  sites across `common.c`, `picture_cuda.c`,
  `libvmaf.c`, and the three `feature/cuda/*.c` feature
  extractors uses the new macros. Twelve `static` helpers
  (`calculate_motion_score`, `filter1d_8/16`,
  `adm_dwt2_*_device`, `adm_csf_device`,
  `i4_adm_csf_device`, `adm_csf_den_*_device`,
  `adm_cm_device`, `i4_adm_cm_device`,
  `integer_compute_adm_cuda`) are `int`-returning to
  carry errors upward. **On rebase**: keep the fork's macro
  definitions and every cleanup-label pattern; upstream
  still uses `assert(0)` as of 2026-04-24. If an upstream
  port adds new `CHECK_CUDA(...)` sites, rewrite them to
  the graceful variants inside the port commit. See
  [ADR-0156](../../../docs/adr/0156-cuda-graceful-error-propagation-netflix-1420.md)
  and [rebase-notes 0049](../../../docs/rebase-notes.md).

- **`integer_psnr_hvs_cuda.c` async-upload + persistent pinned
  staging** (fork-local, T-GPU-OPT-2/3): the file deliberately
  does NOT use `kernel_template.h`'s single-readback shape — it
  carries its own dedicated H2D `upload_str` stream + cross-stream
  `upload_done` event, plus per-plane persistent pinned `h_uint_*`
  staging buffers allocated once in `init_fex_cuda` and reused
  every frame. The `submit_fex_cuda` flow is three explicit
  phases: queue all 6 D2H copies on the pic streams → host-block
  on each pic stream → CPU normalise uint→float → queue all 6
  H2Ds on `upload_str` → record `upload_done` →
  `cuStreamWaitEvent(s->lc.str, upload_done, ...)` before kernel
  launches. This is the only CUDA feature extractor where
  `upload_plane_cuda` lived as a local helper (the other CUDA
  extractors upload through the picture pool); the helper is now
  split into `issue_d2h_plane` / `convert_plane` / `issue_h2d_plane`.
  **On rebase**: do NOT collapse the three-phase flow back into
  the per-call sync pattern; do NOT migrate the upload into
  `kernel_template.h` (the template's single-readback bundle
  doesn't model 6-buffer multi-plane uploads). Keep the persistent
  pinned buffer lifecycle in `init` / `close`. CUDA graph capture
  (future T-GPU-OPT-N) depends on the no-per-frame-alloc invariant
  from this change.

- **`integer_ms_ssim_cuda.c` per-scale partials topology**
  (fork-local, T-GPU-OPT-2 / ADR-0271): the file allocates
  **per-scale** device + pinned-host partials buffers
  (`l_partials[MS_SSIM_SCALES]`, `c_partials[...]`, `s_partials[...]`
  the matching `h_*_partials[...]`). All 5 SSIM scales' `horiz` +
  `vert_lcs` launches and DtoH copies enqueue back-to-back on
  `s->lc.str` inside `submit()`; `cuEventRecord(s->lc.finished, s->lc.str)`
  is recorded once after the last DtoH and registered with
  `vmaf_cuda_drain_batch_register(&s->lc)` so the engine's batched
  drain (`drain_batch.h`) covers this extractor. The shared SSIM
  intermediate buffers (`h_ref_mu`, `h_cmp_mu`, `h_ref_sq`,
  `h_cmp_sq`, `h_refcmp`) stay shared because same-stream ordering
  serialises the per-scale `horiz ⇒ vert_lcs ⇒ DtoH` chain
  naturally. **On rebase**: do NOT collapse the per-scale partials
  arrays back to single buffers — the host-side reduction loop in
  `collect()` walks all 5 scales' `h_*_partials[i]` after the engine
  drains, and aliasing the buffers would force a per-scale
  `cuStreamSynchronize` and break the drain_batch coalesce. Do NOT
  parallelise the per-scale work onto multiple streams — that would
  break the same-stream serialisation that makes the shared
  intermediates safe. See
  [ADR-0271](../../../docs/adr/0271-cuda-drain-batch-ms-ssim.md)
  and [rebase-notes 0228](../../../docs/rebase-notes.md).

- **`integer_psnr_cuda.c` chroma-extension invariants**
  (fork-local, T3-15(b) / ADR-0351): the kernel
  (`calculate_psnr_kernel_{8,16}bpc` in
  `feature/cuda/integer_psnr/psnr_score.cu`) takes a `plane`
  parameter (`unsigned`) appended after `(width, height)`. The
  host's `psnr_cuda_dispatch` packs `kernelParams[]` in the exact
  `(ref, dis, sse, &width, &height, &plane)` order — `cuLaunchKernel`
  cannot validate argument types so any reorder / drop of the
  trailing argument silently corrupts plane indexing. State carries
  one `VmafCudaKernelReadback` per plane (`rb[3]`); the per-frame
  async lifecycle stays singleton — all per-plane launches enqueue
  back-to-back on the picture stream, all per-plane DtoHs enqueue
  back-to-back on `lc.str` (no inter-plane barrier — the
  accumulators are independent). The host relies on
  `libvmaf.c::translate_picture_host`'s `upload_mask` having
  uploaded all 3 planes for non-`YUV400P` inputs. **On rebase**: do
  NOT collapse the `rb[3]` array back to a singleton; do NOT remove
  the `plane` kernel argument; do NOT add a per-extractor
  `needs_chroma` flag that would let a future "minimise upload"
  optimisation skip chroma — chroma kernels (`ciede_cuda`,
  `psnr_cuda`'s chroma branch) silently break if the upload mask is
  narrowed. See
  [ADR-0351](../../../docs/adr/0351-cuda-chroma-psnr.md) and
  [rebase-notes 0320](../../../docs/rebase-notes.md).

- **`kernel_template.h` is the canonical kernel scaffolding**
  (fork-local, ADR-0246): the inline helpers
  `vmaf_cuda_kernel_lifecycle_init/_close`,
  `vmaf_cuda_kernel_readback_alloc/_free`,
  `vmaf_cuda_kernel_submit_pre_launch`, and
  `vmaf_cuda_kernel_collect_wait` capture the private non-blocking
  stream + 2-event + device-accumulator + pinned-readback shape
  every fork-added CUDA feature kernel uses. Templates land
  unused in PR #NNN — each future kernel migration is its own
  gated PR (`places=4` cross-backend-diff per ADR-0214). **On
  rebase**: keep both the header and any kernel call-sites that
  later adopt it; upstream has no equivalent. Reference
  implementation that mirrors the template's shape lives in
  `core/src/feature/cuda/integer_psnr_cuda.c`. See
  [ADR-0246](../../../docs/adr/0246-gpu-kernel-template.md) and
  [docs/backends/kernel-scaffolding.md](../../../docs/backends/kernel-scaffolding.md).

## Per-kernel nvcc flag invariants

- `cuda_cu_extra_flags` map in `core/src/meson.build` routes
  per-kernel nvcc flags. Currently inhabited by `float_adm_score`
  (added in PR #157,
  [ADR-0202](../../../docs/adr/0202-float-adm-cuda-sycl.md)) and
  `ssimulacra2_blur` (added in
  [ADR-0206](../../../docs/adr/0206-ssimulacra2-cuda-sycl.md)).
  Both pass `-Xcompiler=-ffp-contract=off --fmad=false` so the
  recursive / cross-band float reductions keep their CPU-port
  FMUL/FSUB ordering. **On rebase**: never drop these per-kernel
  entries — without them, `float_adm` drifts past `places=4` at
  scale 3, and `ssimulacra2`'s pooled score drifts past `places=2`
  through the IIR + 6-scale pyramid.
- The matching `ssimulacra2_mul` fatbin is a single FMUL with no
  fused-add candidate and intentionally does **not** carry the
  flag — keeping FMA on the kernels where it isn't a precision risk
  preserves whatever optimisation NVCC can apply.

## Lifecycle invariants

- **`cuModuleLoadData` requires a paired `cuModuleUnload` in `close()`**
  — modules carry GPU-resident backing storage (~200-500 KB per module
  on consumer GPUs) that survives `cuStreamDestroy` and (for primary
  contexts) `cuCtxDestroy`. `compute-sanitizer --tool memcheck` does
  **not** report module leaks (the tool tracks `cuMem*Alloc` only),
  which is why the leak in `ssimulacra2_cuda` survived initial
  review. If you add a `cuModuleLoadData` call, add a guarded
  `cuModuleUnload` in the matching `close_fex_cuda` after
  `cuStreamSynchronize` and before `cuStreamDestroy`. Reference fix:
  [ADR-0356](../../../docs/adr/0356-ssimulacra2-cuda-leaks-perf.md)
  (`ssimulacra2_cuda` had two unloaded modules — `module_blur` +
  `module_mul`).

## Governing ADRs

- [ADR-0022](../../../docs/adr/0022-inference-runtime-onnx.md) — CUDA execution provider mapping.
- [ADR-0027](../../../docs/adr/0027-non-conservative-image-pins.md) — CUDA 13.2 + experimental flags.
- [ADR-0131](../../../docs/adr/0131-port-netflix-1382-cumemfree.md) —
  `vmaf_cuda_picture_free` synchronous free.
- [ADR-0202](../../../docs/adr/0202-float-adm-cuda-sycl.md) —
  `float_adm_cuda` requires `--fmad=false` on its fatbin to
  match the GLSL `precise` qualifier in `float_adm.comp`. See
  the per-kernel `cuda_cu_extra_flags` dict in
  `core/src/meson.build`. **On rebase**: do not consolidate
  `float_adm_score` into the global `cuda_flags` block; the
  FMA-off scope is intentionally one fatbin only.

- **FFmpeg `libvmaf` filter — `cuda` selector consumer** (fork-local,
  ADR-0350): the in-tree
  [`ffmpeg-patches/0010-libvmaf-wire-cuda-backend-selector.patch`](../../../ffmpeg-patches/0010-libvmaf-wire-cuda-backend-selector.patch)
  consumes the public CUDA C-API surface
  (`vmaf_cuda_state_init` / `_state_free` / `_import_state` /
  `_preallocate_pictures` / `_fetch_preallocated_picture` from
  [`include/libvmaf/libvmaf_cuda.h`](../../include/libvmaf/libvmaf_cuda.h))
  exactly like SYCL/Vulkan do via patches `0003`/`0004`. **On
  rename / signature change of any of those entry points**: the FFmpeg
  patch must update in the same PR per CLAUDE.md §12 r14. Verify by
  cumulative `git am --3way` replay of `ffmpeg-patches/000{1..9}-*` +
  `0010-*` against pristine FFmpeg `n9.0.1`. The CUDA filter selector
  mirrors the picture-pool ownership contract above: state is freed
  *after* `vmaf_close()`. Reversing the order is a use-after-free.

## Build

```bash
meson setup build -Denable_cuda=true -Denable_sycl=false
ninja -C build
```

Requires `/opt/cuda` + `nvcc` on PATH.
