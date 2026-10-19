---
paths:
  - core/src/feature/sycl/integer_motion_sycl.cpp
  - core/test/test_sycl_motion_add_uv_parity.c
invariant: integer_motion_sycl.cpp::motion_add_uv GPU contract; queue-sync invariant, no host wait in submit.
---
<!-- markdownlint-disable MD013 MD060 -->
# Motion add UV chroma extension

- **`integer_motion_sycl.cpp::motion_add_uv` GPU contract** (ADR-0989).
  When `motion_add_uv=true`, `submit_fex_sycl` packs reference U and V
  planes into pinned host staging (`h_stage_u` / `h_stage_v`);
  `motion_pre_graph` copies them H2D on combined queue into
  `d_ref_u[cur_slot]` / `d_ref_v[cur_slot]`; `enqueue_motion_work` runs
  shared SAD kernel on `d_ref_*[1 - cur_slot]` - `d_ref_*[cur_slot]`,
  accumulating into `d_sad_u` / `d_sad_v`.
  `collect_fex_sycl` sums Y + U + V contributions, each normalized by
  respective plane area (`chroma_w × chroma_h` for UV in YUV420P).
  numerical gate is scalar fixed-point oracle in
  `test_sycl_motion_add_uv_parity.c` (ADR-1326), including its required
  960x540 variant. `float_motion(motion_add_uv=true)` has same semantic
  option but different coefficients and float reduction order, so it is not
  kernel's numerical oracle.
  CUDA, Vulkan, HIP, and Metal twins expose option but return
  `-ENOTSUP` with `WARNING` until their kernel ports land. On rebase:
  if upstream Netflix adds `motion_add_uv` to `integer_motion.c`, verify
  per-plane normalization formula stays consistent.
  **Queue-sync invariant (T-SYCL-MOTION-ADD-UV-SUBMIT-WAIT-2026-09-29,
  supersedes ADR-1034 primary-queue wait)**: no host wait in `submit()`.
  UV H2D rides in-order combined queue in `pre_fn`, ahead of kernels
  (graph replay is fenced by `ext_oneapi_submit_barrier()`). Staging is
  safe to refill in next `submit()`: graph fires on last
  extractor's submit, after staging, and this extractor's `collect()` of
  previous frame (`vmaf_sycl_graph_wait`) drained copy that read it. Do
  not move UV copies back to `vmaf_sycl_memcpy_h2d_async` (primary queue)
  — that needs host wait again.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_motion_sycl.cpp` (motion_add_uv) | `float_motion.c` | `test_sycl_motion_add_uv_parity.c` | ADR-0989 |

| Kernel TU | Parity test | ADR |
|---|---|---|
| `integer_motion_sycl.cpp` (motion_add_uv) | `test_sycl_motion_add_uv_parity.c` | [ADR-0989](../../../../../docs/adr/0989-sycl-motion-add-uv.md) |
