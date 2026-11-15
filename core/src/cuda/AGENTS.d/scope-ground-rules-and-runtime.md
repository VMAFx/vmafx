---
paths:
  - core/src/cuda/common.c
  - core/src/cuda/common.h
  - core/src/cuda/picture_cuda.c
  - core/src/cuda/dispatch_strategy.c
  - core/src/gpu_picture_pool.cpp
  - core/src/meson.build
  - core/include/libvmaf/libvmaf_cuda.h
invariant: CUDA runtime scope, sm_80 floor, non-blocking upload streams, synchronous cuMemFree, and state_free ownership.
---
<!-- markdownlint-disable MD013 -->
# CUDA scope and picture/state runtime

## Scope

CUDA-side runtime (picture lifecycle, buffer pools, launch helpers).
CUDA **feature kernels** live one level deeper in
[../feature/cuda/](../../feature/cuda/). CUDA execution-provider wiring for
ONNX Runtime lives in [../dnn/](../../dnn/AGENTS.md).

```text
cuda/
  common.c/.h          # CUDA context + stream management
  cuda_helper.cuh      # launch macros, error-check, types
  kernel_template.h    # per-feature CUDA kernel scaffolding (ADR-0246)
  picture_cuda.c/.h    # VmafPicture on a CUDA device
  # picture-pool round-robin lives in core/src/gpu_picture_pool.{h,c}
  # (ADR-0239 — backend-agnostic; CUDA was the original consumer)
```

## Rebase-sensitive invariants

- **`VMAF_CUDA_DISPATCH` is consulted at extractor init (ADR-1571).**
  `feature_extractor.cpp::consult_cuda_dispatch()` calls
  `vmaf_cuda_select_strategy(fex->name, &fex->chars, w, h)` for every
  CUDA-flagged extractor before `init()`; key = registered extractor name
  (`vif_cuda:graph`). Non-`DIRECT` result -> init fails `-ENOSYS` (no silent
  fallback). Keep the call when touching `dispatch_strategy.c` or the init
  path; `test_gpu_dispatch_env_contract.py` fails when the selector loses its
  caller. HIP has no dispatch variable (`hip/dispatch_strategy.c` deleted).

- **Every CUDA picture records state that allocated it.**
  `vmaf_cuda_picture_alloc_pinned()` (`picture_cuda.c`) sets
  `priv->cuda.state = cuda_state` next to `priv->cuda.ctx`, like device
  path. Upstream Netflix/vmaf master sets only `ctx`, so its
  `default_release_pinned_picture()` loads `state->f` through NULL state and
  `test_cuda_pic_preallocation` dies with SIGSEGV in host-pinned case (the
  fix is upstream PR Netflix/vmaf#1573, hunk (a)). upstream sync that takes
  upstream's `vmaf_cuda_picture_alloc_pinned()` keeps assignment.
  `test_pinned_picture_release_uses_the_allocating_state`
  (`core/test/test_cuda_runtime_unwind.c`) fails without it and needs no device.

- **CUDA fatbin kernel header dependency tracking**
  ([ADR-1320](../../../../docs/adr/1320-cuda-hip-kernel-header-dependency-tracking.md);
  [Research-2106](../../../../docs/research/2106-cuda-hip-kernel-header-dependency-tracking.md)):
  All CUDA fatbin custom targets (`cu_ptx_target_*`) in `core/src/meson.build`
  must bind `depend_files: cuda_kernel_shared_headers` covering complete
  repo-local quoted include closure plus `config_h_target`, combined with compiler
  depfiles (`cu_depfile = host_machine.system() == 'windows' ? '' : '@0@.fatbin.d'.format(name)`
  and `nvcc_dep_flags = host_machine.system() == 'windows' ? [] : ['-MD', '-MF', '@DEPFILE@']`).
  Editing structs in shared headers (e.g. `integer_adm_cuda.h`, `vif_cuda.h`, `moment_cuda.h`)
  must reliably trigger incremental fatbin rebuilds in Ninja without manual `touch`
  workarounds. Do not remove `depend_files` or omit shared headers on rebase.

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
  [ADR-0378](../../../../docs/adr/0378-picture-stream-non-blocking.md)
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
  [ADR-0131](../../../../docs/adr/0131-port-netflix-1382-cumemfree.md)
  and [rebase-notes 0031](../../../../docs/rebase-notes.md).

- **`vmaf_cuda_state_free()` ownership contract** (fork-local,
  ADR-0157): public API at
  [`include/libvmaf/libvmaf_cuda.h`](../../../include/libvmaf/libvmaf_cuda.h)
  now includes `vmaf_cuda_state_free(VmafCudaState *cu_state)`.
  Ownership model: `vmaf_cuda_state_init` allocates →
  `vmaf_cuda_import_state` copies-by-value (no ownership transfer)
  → `vmaf_close` destroys CUDA stream + context + frees
  `CudaFunctions` driver table (via fork-local
  `cuda_free_functions()` call in `vmaf_cuda_release`) →
  `vmaf_cuda_state_free` frees heap allocation itself. Any nonzero
  `vmaf_close` retains teardown-only context and imported state;
  retry close before releasing dependencies. Call order
  `successful vmaf_close → vmaf_cuda_state_free → vmaf_model_destroy`
  load-bearing; reversing first two = use-after-free. Mirrors SYCL
  backend's `vmaf_sycl_state_free()` pattern. **On rebase**: keep
  fork's public symbol; upstream doesn't have this API as of
  2026-04-24. See
  [ADR-0157](../../../../docs/adr/0157-cuda-preallocation-leak-netflix-1300.md)
  and [rebase-notes 0050](../../../../docs/rebase-notes.md).
  initialized state that was never imported is exception:
  `vmaf_cuda_state_free` first invokes retry-safe `vmaf_cuda_release` and
  retains wrapper on error. Import marks caller wrapper so imported
  state-free stays allocation-only after exact-zero context close. Never remove
  `-EBUSY` guard against duplicate import or live CUDA-owner overwrite.
- **`vmaf_gpu_picture_pool_close` mutex destroy order** (fork-local,
  ADR-0157, promoted out of `cuda/` to `core/src/gpu_picture_pool.c`
  per ADR-0239): function does `pthread_mutex_unlock` →
  `pthread_mutex_destroy` → `free(pic)` → `free(pool)`. Destroying
  locked mutex = POSIX UB; old code destroyed it locked. On rebase:
  keep unlock-before-destroy order. Slot release is also two-phase:
  successful callbacks are marked committed, failed callback retains
  pool, and retry visits only slots not yet released.
