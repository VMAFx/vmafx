---
paths:
  - core/src/cuda/cuda_helper.cuh
  - core/src/cuda/kernel_template.h
  - core/src/cuda/drain_batch.h
  - core/src/feature/cuda/integer_psnr_hvs_cuda.c
  - core/src/feature/cuda/integer_ms_ssim_cuda.c
  - core/src/feature/cuda/integer_psnr_cuda.c
invariant: Graceful CHECK_CUDA macros, persistent PSNR-HVS staging, MS-SSIM partials, chroma PSNR, and kernel_template.h.
---
<!-- markdownlint-disable MD013 -->
# CHECK_CUDA macros, extractor staging, and kernel template

## Rebase-sensitive invariants

- **`CHECK_CUDA` graceful error propagation** (fork-local,
  ADR-0156): `CHECK_CUDA` macro in
  [`cuda_helper.cuh`](../cuda_helper.cuh) does NOT call `assert(0)` on
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
  [ADR-0156](../../../../docs/adr/0156-cuda-graceful-error-propagation-netflix-1420.md)
  and [rebase-notes 0049](../../../../docs/rebase-notes.md).

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
  [ADR-0271](../../../../docs/adr/0271-cuda-drain-batch-ms-ssim.md)
  and [rebase-notes 0228](../../../../docs/rebase-notes.md).

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
  [ADR-0351](../../../../docs/adr/0351-cuda-chroma-psnr.md) and
  [rebase-notes 0320](../../../../docs/rebase-notes.md).

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
  [ADR-0246](../../../../docs/adr/0246-gpu-kernel-template.md) and
  [docs/backends/kernel-scaffolding.md](../../../../docs/backends/kernel-scaffolding.md).
