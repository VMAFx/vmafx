---
paths:
  - core/src/meson.build
  - core/src/cuda/common.c
  - core/src/cuda/picture_cuda.c
  - core/src/cuda/drain_batch.c
  - core/src/cuda/kernel_template.h
invariant: Shared strict-FP fatbin flags (--fmad=false), context-current resource teardown, drain_batch owner, and unwinds.
---
<!-- markdownlint-disable MD013 -->
# Fatbin strict-FP flags, context teardown, and drain batch

## Kernel nvcc flag invariants

- **One FP flag list for every fatbin
  ([ADR-1403](../../../../docs/adr/1403-cuda-strict-fp-every-kernel.md)).**
  `cuda_device_strict_fp_args` (`core/src/meson.build`, between
  `VMAF CUDA device strict FP policy` markers) = `vmaf_cuda_host_strict_fp_args`
  plus `--fmad=false` under nvcc, `-ffp-contract=off` under clang CUDA. Fatbin
  `custom_target` appends it to every kernel's command. No kernel contracts
  `a * b + c`; fused operation reference performs = explicit `__fmaf_rn()`
  in kernel source. Until ADR-1403 only `float_adm_score`
  ([ADR-0202](../../../../docs/adr/0202-float-adm-cuda-sycl.md)),
  `ssimulacra2_blur`
  ([ADR-0206](../../../../docs/adr/0206-ssimulacra2-cuda-sycl.md)),
  `ssimulacra2_device`
  ([ADR-1391](../../../../docs/adr/1391-cuda-ssimulacra2-device-resident.md)),
  `integer_ssim_score`, `speed_score` and `psnr_hvs_score` (ADR-1397) had
  it, through `cuda_cu_extra_flags`.
  **On rebase**: never restore per-kernel FP entries and never drop shared
  list from fatbin command. Without it `float_adm` drifts past `places=4` at
  scale 3, `ssimulacra2_cuda` loses 1e-9 contract (ADR-1391),
  `float_ms_ssim_cuda` loses bit-exactness (ADR-1403).
- `cuda_cu_extra_flags` map = per-kernel flags that are not floating-point
  (empty today). `core/test/test_strict_fp_compiler_args.py` executes policy
  for nvcc and clang and fails on per-kernel FP flag, `--fmad=true`, fatbin
  command without shared list, second definition.

## Lifecycle invariants

- **Feature resources are destroyed with their owning context current
  ([ADR-1336](../../../../docs/adr/1336-cuda-context-owned-resource-teardown.md)).**
  Modules, streams, and events owned by extractor must use
  `vmaf_cuda_module_unload`, `vmaf_cuda_stream_destroy`, and
  `vmaf_cuda_event_destroy`; feature close/unwind code must not call raw
  driver destroy functions. helpers push `VmafCudaState::ctx`, preserve
  first error, restore caller's previous context, and clear handle only
  after confirmed destruction. `vmaf_cuda_kernel_lifecycle_init` must unwind
  every successfully created earlier handle if later step fails.
  device-free `test_cuda_runtime_unwind` and complete owner inventory in
  `test_cuda_module_lifecycle_contract.py` guard this rule. Adding
  `cuModuleLoadData` owner requires updating that inventory in same PR.
  Wrapped, raw-device, and pinned feature buffers use
  `vmaf_cuda_buffer_free_owned`, `vmaf_cuda_deviceptr_free_owned`, and
  `vmaf_cuda_buffer_host_free_owned`; field is cleared only after driver
  confirms actual free. CUDA extractor contexts publish `close_required`
  before entering `init`, so failed partial init remains closeable even
  though `is_initialized` is false. Do not extend that failed-init behavior to
  non-CUDA extractors without auditing their close callbacks first.
- **Drain batch belongs to one engine at time.** `drain_batch.c`'s
  `g_drain_batch` thread-local (ADR-0242), but two `VmafContext`s can
  run on one OS thread, so it carries owning `VmafCudaState`:

  - `vmaf_cuda_drain_batch_open()` takes that state, drops entries
    left by different owner.
  - `vmaf_cuda_drain_batch_flush()` returns 0 without touching CUDA
    when caller isn't owner; clears entries it consumed.
  - `vmaf_cuda_drain_batch_thread_destroy()` first quiesces and destroys its
    stream; failure retains stream, entries, open flag, and owner for
    retry. It clears registration ownership only after success.

  Never restore owner-less `open(void)` signature: without it,
  closed context leaves dangling `CUevent`s and freed `bool *` flags
  for next one (docs/state.md
  T-UPSTREAM-1305-CUDA-DRAIN-BATCH-THREAD-GLOBAL-2026-09-03, pinned
  by `core/test/test_cuda_drain_batch.c`).

- **ADR-0982 error-path unwinds and partial-init cleanup (BUG-048 Sec A3)**:
  - In `picture_cuda.c`: `vmaf_cuda_picture_alloc` zeroes `priv` struct immediately upon allocation (`memset(priv, 0, sizeof(*priv))`). On plane allocation failure (`device_pic_alloc_planes` non-zero), `device_pic_unwind` is invoked with `DEV_PIC_UNWIND_DATA` so that prior successfully allocated device planes are freed (`cuMemFree(pic->data[i])`) and pointers zeroed (`pic->data[i] = NULL`), rather than `DEV_PIC_UNWIND_FINISHED` which skipped plane unwinding.
  - In `common.c`: `vmaf_cuda_release()` retains `cu_state`, its live handle,
    and `CudaFunctions` table after driver failure. Only successful stream
    quiescence/destruction and primary-context release commit zeroed state
    and function-table free.
  - In `drain_batch.c`: in `drain_stream_ensure()`, if `cuCtxPopCurrent` fails after `cuStreamCreateWithPriority`, execution branches to `fail_after_stream` to destroy `g_drain_batch.drain_str` before returning `-ENOTRECOVERABLE`.
  - Guarded by deterministic mock-driver unit test `core/test/test_cuda_runtime_unwind.c`.
  - On rebase: maintain these unwinds, never bypass `DEV_PIC_UNWIND_DATA` on
    plane allocation failures, and never clear `VmafCudaState` after failed
    `vmaf_cuda_release`. Allocator-internal `device_pic_unwind` and
    `device_pic_free_after_pop`, including earlier-slot cleanup during failed
    pool construction, remain best-effort rollback; ADR-1336 retry guarantees
    apply to fully published extractor and ring-picture owners, not half-built
    allocator-local objects.
