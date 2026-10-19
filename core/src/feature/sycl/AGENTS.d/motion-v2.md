---
paths:
  - core/src/feature/sycl/integer_motion_v2_sycl.cpp
  - core/test/test_sycl_motion_v2_parity.c
invariant: integer_motion_v2_sycl.cpp reads shared frame; flush derives motion2_v2 / motion3_v2 via CPU window function.
---
<!-- markdownlint-disable MD013 MD060 -->
# motion3_v2 cross-twin invariant (ADR-1108)

- `integer_motion_v2_sycl` emits `motion2_v2` / `motion3_v2` host-side in
  flush through CPU's own `vmaf_motion_window_flush()`
  (`core/src/feature/motion_window.h`, ADR-1478, ADR-1491): blend,
  `motion_max_val` clip, `stamp_value` seed for `i < min_idx`, optional
  moving average, three-frame or five-frame window. Twin holds no copy of
  that arithmetic (CUDA / HIP twins same; Metal still carries its copy and
  must be kept in step by hand). Guards: `test_sycl_motion_v2_parity`,
  `test_sycl_motion_five_frame_window`, `test_sycl_exact_twins`.

- **`integer_motion_v2_sycl.cpp` reads shared frame** (ADR-1369). `cur`
  = `vmaf_sycl_get_shared_plane(state, 1, 0)` behind
  `vmaf_sycl_queue_after_upload()`; ADR-1371 pipeline's `cur_copy` keeps it
  in `d_pix[index % ring]` as later frame's `prev` (`enqueue_copy` on
  frames without one; `ring` 2, or 3 with `motion_five_frame_window`).
  No host copy, no private upload; kernel stays in
  `integer_motion_pipeline_sycl.cpp`.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_motion_v2_sycl.cpp` | `integer_motion_v2.c` | `test_sycl_motion_v2_parity.c` | ADR-0884 (round 2) |

| Kernel TU | Parity test | ADR |
|---|---|---|
| `integer_motion_v2_sycl.cpp` | `test_sycl_motion_v2_parity.c` | ADR-0884 |
