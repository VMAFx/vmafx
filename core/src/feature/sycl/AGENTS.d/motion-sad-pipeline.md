---
paths:
  - core/src/feature/sycl/integer_motion_pipeline_sycl.*
  - core/test/test_sycl_motion*
invariant: Motion SAD = one shared kernel, difference first; sum |blur(prev - cur)|.
---
<!-- markdownlint-disable MD013 MD060 -->
# Motion SAD pipeline shared kernel

- **Motion SAD = one shared kernel, difference first
  (T-SYCL-MOTION-TINY-FRAME-PARITY-2026-09-29).** `motion_sycl` and
  `motion_v2_sycl` both call `motion_sycl_pipeline::enqueue_sad()`
  (`integer_motion_pipeline_sycl.{h,cpp}`): `sum |blur(prev - cur)|`,
  vertical pass rounded `>> bpc`, horizontal `>> 16`, reflect-101 borders =
  CPU `integer_motion.c` / `integer_motion_v2.c` `motion_score_pipeline_*`
  since Netflix a4a1492d port (PR #532). Bit-exact; gate
  `test_sycl_motion_tiny_frames` compares with `==` (3x3 .. 1283x723, 8/10/16
  bit). `blur(cur) - blur(prev)` rounds twice per pixel -> 2e-4 off at 17x17;
  never reintroduce it. `prev - cur` order load-bearing (arithmetic shift
  floors negatives). Vertical sum int32 up to 15 bpc, int64 at 16 (host picks
  `submit_sad<Acc>` instance). Kernel lives only in pipeline TU
  (Research-2090 name collision); extractor TUs hold none. `motion_sycl` keeps
  raw luma of previous frame in `d_raw_y[2]` (device memcpy from
  shared frame after kernel), because shared frame buffers are
  overwritten by next upload. Measured cost vs old per-frame blur
  (4K micro-benchmark, kernel + copy): about +11% on B580 and UHD 770.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_motion_pipeline_sycl.cpp` (motion + motion_v2 SAD) | `integer_motion.c`, `integer_motion_v2.c` | `test_sycl_motion_tiny_frames.c` (bit-exact, 3x3 .. 1283x723) | T-SYCL-MOTION-TINY-FRAME-PARITY-2026-09-29 |
