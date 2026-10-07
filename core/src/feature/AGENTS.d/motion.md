---
paths:
  - core/src/feature/motion.c
  - core/src/feature/float_motion.c
  - core/src/feature/integer_motion.c
invariant: Motion plane structures, upstream options, mirror implementations, and chroma min dims.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Motion Plane Layouts, Mirror Bodies, and Min Dims

- **`float_motion.c` planes**: `MotionState.plane[0..2]` are Y, U, V; U and V
  exist only with `motion_add_uv`, so `motion_free_planes` (only teardown)
  must keep `motion_add_uv` guard. `motion_chroma_heights` rejects
  chroma-less formats **before** any allocation. `motion_score_pair` adds
  Y, then U, then V — `double` add order is load-bearing for
  `motion_add_uv` parity with CUDA / SYCL twins. `motion_clip` /
  `motion_blend_clip` are only places `motion_fps_weight` /
  `motion_max_val` clip is applied.

- **`integer_motion.c` five-frame window = upstream, statement for statement**
  (Netflix `a2b59b77` / `a4a1492d`, ADR-1478): `motion_five_frame_window=true`
  -> SAD of frame n against `fex->prev_prev_ref` (n-2), `min_idx = stride = 2`
  in `flush()` (`motion_flush_one()`). Scores equal Netflix `9e48141b` bit for
  bit (2232 harness runs, 95130 values, scalar / AVX2 / default dispatch,
  0 / 1 / 4 threads). Shipped `model/vmaf_v1.0.16_hfr/*.json` set option:
  `-ENOTSUP` in `init()` (ADR-0337 / ADR-0994, removed) makes four models
  unscorable and 13 Netflix golden tests fail. Upstream sync touching
  `extract()` / `flush()` -> take upstream arithmetic, keep fork helpers
  (`motion_select_pipeline()`, `motion_flush_one()`), mirror into
  `integer_motion_v2.c`. No frame n-2 at index >= 2 -> `-EINVAL`.
  `reads_prev_prev_ref()` (both extractors) answers option: framework
  keeps n-2, and preallocated pool must hold >= 4 pictures, only then
  (fork deviation from upstream's unconditional window, no score moves; see
  `core/src/AGENTS.d/picture-ownership-and-dispatch.md`). Do not drop
  hook or make it answer true unconditionally. GPU twins:
  `motion_cuda`, `motion_sycl`, `motion_hip` compute window (frame n-2
  on device, `vmaf_motion_window_flush()` at flush, ADR-1491); change to
  `vmaf_motion_window_flush()` / `motion_flush_one()` reaches them through
  call, change to `extract()`'s min_idx / prev selection changes their
  `ring` / `depth` in same PR. `integer_motion_metal` does not declare
  option -> model / `--feature motion` dispatch runs this extractor.
  Guards: `core/test/test_motion_five_frame_window.c`,
  `test_integer_motion_coverage`, golden
  `test_run_vmaf_integer_fextractor_motion_five_frame_window*` (9) +
  `vmaf_v1_quality_runner_test.py` hfr (4).

- **Upstream ports**: `feature/motion` options from `b949cebf`
  (T-NEW-1) MERGED via PR #197 (2026-04-29). `feature/speed`
  port from `d3647c73` (`speed_chroma` + `speed_temporal`) is
  PR #213 (open). 32-bit ADM/cpu fallbacks (`8a289703` +
  `1b6c3886`) are PR #212 (open).
