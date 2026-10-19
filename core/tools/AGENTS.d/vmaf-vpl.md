---
paths:
  - core/tools/vmaf_vpl.c
  - core/tools/vmaf_vpl_core.h
  - core/tools/vmaf_vpl_core.c
invariant: vpl_decode_frame bounded by VPL_DECODE_MAX_ATTEMPTS; decoupled classification; warning frames delivered.
---
# VPL decode to SYCL pipeline

- `vmaf_vpl.c` / `vmaf_vpl_core.h` — VPL decode -> SYCL pipeline
  (fork-local, not upstream).
  - **`vpl_decode_frame` retries under `VPL_DECODE_MAX_ATTEMPTS`.**
    ceiling is *derived*: `VPL_SYNC_TIMEOUT_MS` / `VPL_DECODE_RETRY_US`, i.e.
    60 s timeout same function already hands
    `MFXVideoCORE_SyncOperation()` at 1 ms back-off it already used.
    Change one of three macros and other two must still describe
    same wall clock. Never restore bare `for (;;)`, and never widen
    ceiling without re-deriving it from measured busy-loop distribution
    ([ADR-1287](../../../docs/adr/1287-cli-tool-unbounded-loop-ceilings.md)).
    Status classification and frame loop execution decoupled in
    `vmaf_vpl_core.h` and `vmaf_vpl_core.c`
    ([ADR-1900](../../../docs/adr/1900-vpl-decode-ceiling-contract.md)).
    Frames published alongside positive warning status codes
    (`sts > 0 && sync != NULL`) must be delivered
    (`VPL_DECODE_ACTION_FRAME_READY`), not dropped. Transient
    `MFX_WRN_ALLOC_TIMEOUT_EXPIRED` retries alongside `MFX_WRN_DEVICE_BUSY`.
    Ceiling contract and frame ordering pinned by device-free
    contract tests in `core/tools/test/test_vmaf_vpl_decode_ceiling.c` and
    hardware smoke tests in `core/tools/test/test_vmaf_vpl_hardware_smoke.sh`.
  - **`VplFallbackState` flags are load-bearing, not defensive.**
    `vpl_fallback_release()` reads `have_ref_img` / `have_dis_img` /
    `have_ref_map` / `have_dis_map` / `have_ref_pic` / `have_dis_pic` to
    release exactly what acquisition stages managed to take, in order
    retired `cleanup:` label used. Setting flag without matching
    release branch (or vice versa) leaks or double-frees; this is what
    replaced `clang-analyzer-deadcode.DeadStores` NOLINT that used to sit
    on `have_dis_pic`.
