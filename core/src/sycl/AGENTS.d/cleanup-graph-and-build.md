---
paths:
  - core/src/sycl/common.cpp
  - core/src/meson.build
invariant: Goto-free shared-frame cleanup, sticky graph_wait error handling, graph_register order, ADRs, and SYCL builds.
---
<!-- markdownlint-disable MD013 -->
# Shared-frame cleanup, graph wait/register, and SYCL build

## Rebase-sensitive invariants per kernel

- **`common.cpp` cleanup + helper boundaries (HISS-21 burn-down).**
  `sycl_shared_frame_release()` is single cleanup owner for shared
  it replaced `fail:` label that `vmaf_sycl_shared_frame_init` used
  before HISS-01 burn-down.
  Body is label block verbatim. Loop frees `shared_ref_buf[i]` before
  `shared_dis_buf[i]` for `i = 0` then `i = 1`, guarded by null check;
  nulls four slots. Every error exit frees same buffers in same order;
  partially-allocated states rely on null checks. Helper is idempotent.
  alongside it (`sycl_resolve_device`, `sycl_log_fp64_note`,
  `sycl_profiling_enabled`, `sycl_queue_props`, `sycl_enqueue_plane_upload`,
  `sycl_any_extractor_wants_graph`, `sycl_run_compute_phase`,
  `sycl_apply_input_barriers`, `sycl_enqueue_all_phases`) exist to hold their
  callers inside HISS-04 60-LOC bound. **On rebase**: do not reintroduce
  `goto fail` or early return between allocation and release call;
  do not move these helpers to another TU or behind function pointer (that
  changes inlining and was not what burn-down verified); keep
  `sycl_resolve_device` and `sycl_enqueue_all_phases` called from *inside*
  their caller's existing `try` so `sycl::exception` never crosses
  `extern "C"` frame; preserve in-order enqueue order (ref before dis in
  `vmaf_sycl_shared_frame_upload`; graph recording, then `pre_fn`, then
  compute, then `post_fn` in `sycl_enqueue_all_phases`); and keep
  `sycl_enqueue_plane_upload`'s original `static_cast<unsigned>` narrowing on
  stride comparison — widening it to `size_t` changes which pictures take
  bulk-memcpy path.

- **`vmaf_sycl_graph_wait()` counts only successful wait
  (T-SYCL-GRAPH-WAIT-ERROR-DROPPED-2026-09-29).** Idempotent per frame, but
  `graph_waited_frame = frame` is set only after `wait_and_throw()` returns:
  failed wait leaves frame unwaited, so every collector's call waits again
  and lost device (`UR_RESULT_ERROR_DEVICE_LOST` is sticky) fails it too.
  Graph collectors (`adm`, `vif`, `motion`, `psnr`, `float_moment`) return
  non-zero result before touching host buffers. Before: first caller failed,
  rest got 0 and scored stale buffers. No new state member on purpose:
  one more public member of `VmafSyclState` is one more
  `misc-non-private-member-variables-in-classes` finding in `common.cpp`.
  **On rebase**: keep assignment after wait, and every collector's
  `if (wait_err) return wait_err;`.

- **`vmaf_sycl_graph_register` allocation order (ADR-0982 / BUG-048 Sec A3).**
  In `vmaf_sycl_graph_register()` (`common.cpp`), ensure `state->combined_queue`
  is allocated (via `std::make_unique<sycl::queue>`) *before* incrementing
  `state->num_graph_extractors` or populating extractor entry. If queue
  creation throws or fails, `num_graph_extractors` must not be left dirty with
  uninitialized entry in `graph_extractors[]`. On rebase: preserve this
  allocation-before-registration sequence.

- [ADR-0002](../../../../docs/adr/0002-merge-path-master-default.md) —
  sycl branch → master merge history.
- [ADR-0016](../../../../docs/adr/0016-sycl-to-master-merge-conflict-policy.md)
  — merge-conflict policy.
- [ADR-0022](../../../../docs/adr/0022-inference-runtime-onnx.md) —
  OpenVINO EP mapping for SYCL.
- [ADR-0027](../../../../docs/adr/0027-non-conservative-image-pins.md) —
  experimental SYCL flags.
- [ADR-0220](../../../../docs/adr/0220-sycl-fp64-fallback.md) — SYCL
  feature kernels are unconditionally fp64-free (T7-17).
- [ADR-0335](../../../../docs/adr/0335-adaptivecpp-second-sycl-toolchain.md)
  — AdaptiveCpp added as second SYCL toolchain alongside icpx;
  Intel-specific kernel attributes routed through
  `feature/sycl/sycl_compat.h`.
- [ADR-0982](../../../../docs/adr/0982-gpu-runtime-bug-audit-round-26.md) —
  GPU runtime bug audit — round 26 (init/teardown leak sweep).
- [ADR-1121](../../../../docs/adr/1121-sycl-qsv-zerocopy-p010-normalization.md)
  — QSV zero-copy P010/P012 MSB→LSB normalization in VA import +
  separate-per-decoder-session decode contract.

## Build

```bash
meson setup build core -Denable_cuda=false -Denable_sycl=true
ninja -C build
```

Requires oneAPI (`source /opt/intel/oneapi/setvars.sh`) or equivalent
DPC++ toolchain with `icpx` on PATH.

Alternative: AdaptiveCpp (open-source, ADR-0335).

```bash
meson setup build-acpp core -Denable_cuda=false -Denable_sycl=true \
    -Dsycl_compiler=acpp -Dsycl_acpp_targets=generic
ninja -C build-acpp
```

See
[`docs/development/sycl-toolchains.md`](../../../../docs/development/sycl-toolchains.md)
for per-toolchain capability matrix and numerical conformance notes.
