---
paths:
  - core/src/feature/sycl/integer_motion_sycl.cpp
  - core/test/test_sycl_motion*
invariant: motion_sycl output set = CPU motion, and motion_force_zero lives in submit / collect.
---
<!-- markdownlint-disable MD013 MD060 -->
# Parity invariant — motion3 CPU and SYCL moving-average paths

`integer_motion.c` (CPU) and `integer_motion_sycl.cpp` (SYCL) both implement
motion3 post-process as host-side moving average over blended motion2
scores. Both paths **must stay in numerical parity at places=4** (delta
≤ 1e-4, per ADR-0214). Gate enforced by
`core/test/test_sycl_motion3_parity.c`. Any change to blend formula
(`motion_blend()`), moving-average guard condition, or `motion_max_val`
clipping must mirror across both files. Same for CUDA / Vulkan /
HIP / Metal motion twins listed in Twin-update table above — same PR.

- **`motion_sycl` output set = CPU `motion`, and `motion_force_zero`
  lives in submit / collect** (`T-GPU-MOTION-SAD-SCORE-NOT-EMITTED-2026-10-02`,
  `T-SYCL-MOTION-FORCE-ZERO-IGNORED-2026-10-02`).
  `motion_append_sad_score()` appends
  `VMAF_integer_feature_motion_sad_score` EVERY frame (0 at frame 0, else
  `MIN(sad * motion_fps_weight, motion_max_val)`), same value as
  `motion_score` only with `debug`; `provided_features` lists SAD score
  first, as `integer_motion.c` does. libvmaf drives SYCL extractor through
  `submit` / `collect`, never `extract`: option handled only in
  `extract_fex_sycl()` is ignored (that was force-zero bug). Under
  `motion_force_zero`: init returns before any device allocation and before
  `vmaf_sycl_graph_register()` (registered extractor must call
  `vmaf_sycl_graph_submit()` every frame, unregistered one must not);
  `submit` only marks frame pending, `collect` =
  `motion_append_forced_zero()` (SAD, motion2, motion3 = 0, + motion with
  debug); `flush` returns 1 without appending. On rebase: new CPU output
  or emit site in `integer_motion.c::extract` -> same change here, same PR.
  Guards: `test_sycl_motion_sad_score` (11 frames, 8 / 10 bit, default /
  debug / force zero / weight + cap, `==` on every output, and no output
  CPU lacks), `test_sycl_exact_twins`, gate cells `motion` / `motion_debug`
  (SAD score in `FEATURE_METRICS`).
- **`integer_motion_sycl.cpp::motion3_postprocess_*` honours
  motion3 GPU contract** (ADR-0219). Applies CPU's host-side
  post-process to motion2 with no device-side state.
  `motion_five_frame_window=true` is computed by twin, through
  CPU's window function at flush
  ([motion-five-frame-window](motion-five-frame-window.md), ADR-1491). See [../../AGENTS.md §"motion3_score GPU contract"](../../../AGENTS.md).
- **`motion_fps_weight` cross-backend parity** — see canonical
  invariant note in [`../cuda/AGENTS.md`](../../cuda/AGENTS.md).
  `integer_motion_v2_sycl.cpp` and `float_motion_sycl.cpp` both carry
  `motion_fps_weight` option, apply it in `flush()` /
  `collect()` exactly as documented there. Any future change to
  weight application math must span all motion-family GPU twins in
  same PR. Since ADR-1365 `float_motion_sycl.cpp` spells it CPU
  way, `motion_clip(min(prev, cur))` = min, then weight, then
  `motion_max_val` cap; for weight >= 0 bit-identical to
  weight-before-min note describes. v1 `integer_motion_sycl.cpp`
  twin covered by same canonical note's **applied exactly once**
  clause (ADR-1216): `motion3_postprocess_sycl()` must not re-apply
  weight its callers already applied.

- [ADR-0219](../../../../../docs/adr/0219-motion3-gpu-contract.md) —
  motion3 GPU contract.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_motion_sycl.cpp` (motion3) | `integer_motion.c` | `test_sycl_motion3_parity.c` | ADR-0219 |

| Kernel TU | Parity test | ADR |
|---|---|---|
| `integer_motion_sycl.cpp` | `test_sycl_motion3_parity.c` | [ADR-0219](../../../../../docs/adr/0219-motion3-gpu-contract.md) |
