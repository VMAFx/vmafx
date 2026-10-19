---
paths:
  - core/src/libvmaf.c
  - core/src/hip/common.c
  - core/src/hip/common.h
  - core/include/libvmaf/libvmaf_hip.h
  - core/test/test_hip_device_selection.c
  - core/test/test_hip_device_index_contract.py
invariant: vmaf_hip_import_state lives in libvmaf.c and HIP state lifetime is caller-owned mirroring SYCL and Metal.
---
# HIP State Import and Context Lifetime

## Rebase-sensitive invariants (import-state — ADR-0519)

- **`vmaf_hip_import_state` lives in `core/src/libvmaf.c`, not in
  `core/src/hip/common.c`** (fork-local, ADR-0519). Function needs
  `VmafContext` field-level access; placing it next to CUDA / SYCL /
  Metal `_import_state` twins keeps borrowed-state implementations in
  one TU.
  Do NOT re-introduce copy of function in `hip/common.c` —
  duplicate-symbol link error is obvious failure mode, but more
  insidious one is divergent behaviour between two definitions. On
  rebase: if upstream port adds HIP-related function to
  `libvmaf.c`, leave `vmaf_hip_import_state` block intact next to
  its SYCL / Metal siblings.
- **`VmafContext::hip` substruct is appended after `metal`**
  (fork-local, ADR-0519). `hip` struct holds single
  `VmafHipState *state` pointer gated by `#ifdef HAVE_HIP`.
  Intentionally appended at end of GPU-backend substructs so
  CPU-only / CUDA-only / etc. builds see no offset shifts. On
  rebase: if upstream reorders file-private VmafContext definition,
  keep HIP block at end. Keep its `#ifdef HAVE_HIP` guard exactly
  aligned with public-header include block at top of file.
- **HIP state lifetime mirrors SYCL / Metal, not CUDA**
  (fork-local, ADR-0519). `vmaf_close` clears `vmaf->hip.state =
  NULL` without freeing underlying state — caller owns state, frees
  it via `vmaf_hip_state_free()` only after `vmaf_close()` returns exactly 0.
  Every nonzero close retains context and borrowed state for retry. This
  deliberately differs from CUDA twin's by-value copy semantics,
  which historically grew ownership-transfer ambiguity newer
  backends avoid. On rebase: if upstream changes CUDA twin's
  ownership model, do NOT propagate change to HIP without ADR —
  pointer-stash contract is load-bearing for caller-owned
  `VmafHipState` lifetime documented in
  `core/include/libvmaf/libvmaf_hip.h`.

## Device selection (ADR-1523)

- **`vmaf_hip_context_new()` selects device it is given.** It checks
  index against `vmaf_hip_device_count()` (`-EINVAL` outside `[0, count)`,
  `-ENODEV` with no device) and calls `hipSetDevice()`. Every HIP twin passes
  `fex->hip_device_index`, which `set_fex_hip_device()` in `libvmaf.c` fills
  from imported state for every extractor context, flagged or not. new
  twin passes same field; literal index is regression
  (`test_hip_device_index_contract`).
- **state's device is rebound before frame's twins run and before
  flush** (`vmaf_hip_state_bind()` in `read_pictures_hip_frame_begin()` and
  `flush_context()`). HIP binds device to calling thread; without
  rebind caller that scores on another thread lands on device 0.
- **`vmaf_hip_device_count()` is count only when runtime answers.**
  `hipErrorNoDevice` is 0; any other failure is negative errno, and
  `vmaf_hip_list_devices()` / `vmaf_hip_state_init()` pass it on. Do not
  bring back "return 0 on error". `test_hip_device_selection` checks all of
  it against stubbed runtime on any host.
