---
paths:
  - core/src/feature/sycl/float_motion_sycl.cpp
  - core/test/test_sycl_float_motion_parity.c
  - core/test/test_sycl_twin_option_parity.c
invariant: float_motion_sycl.cpp SAD = CPU order, bit for bit; emits through motion_clip() / motion_blend_clip(), motion3 included.
area: motion-psnr-and-moment
---
<!-- markdownlint-disable MD013 MD060 -->
# Float motion extractor and kernels

- **`float_motion_sycl.cpp` emits through `motion_clip()`** (ADR-1365).
  Every emitted `motion` / `motion2` (debug score, tail in `flush()`
  included) = `MIN(score * motion_fps_weight, motion_max_val)`, CPU
  `float_motion.c` order (min of two SADs first). `motion_force_zero`
  short-circuits `submit()` (no upload, no kernel) and `collect()` emits
  zeros (motion2, motion3, debug motion); before ADR-1365 it was declared
  and ignored.
- **`float_motion_sycl.cpp` emits `motion3`** (2026-10-03,
  `T-GPU-FLOAT-MOTION3-MISSING-2026-09-30`; CUDA #1637, HIP ADR-1404 same
  shape). Host only, no kernel. `motion3` = `motion_blend_clip()` =
  `MIN(motion_blend(score * mfw, mbf, mbo), mmxv)` (`motion_blend_tools.h`,
  CPU `float_motion.c`). `collect()`: frame 1 -> motion3 of frame 0 from
  first SAD alone (motion2 of 0 = 0 emitted at frame 0); frame >= 2 -> motion2 /
  motion3 of `index - 1` from `min(cur, prev)`. `flush()`: `frame_index > 1`
  -> tail motion2 / motion3 of last SAD at `frame_index - 1`; else motion3 =
  0 at 0 (one-frame run; CPU flush does same). `motion_blend_factor` (`mbf`)
  / `motion_blend_offset` (`mbo`) declared as CPU table (name, alias,
  default, range, flags); `test_twin_options_are_cpu_options` fails any twin
  option CPU lacks or declares differently. Not declared, so request falls
  back to CPU (ADR-1183): `motion_add_scale1`, `motion_add_uv`,
  `motion_filter_size` (HIP has them, ADR-1404). `motion_five_frame_window`
  = integer `motion` / `motion_v2` option only; CPU `float_motion` has none.
  Gate `FEATURE_METRICS["float_motion"]` lists `motion3` (both
  `cross_backend_parity_gate.py` and `cross_backend_vif_diff.py`); twin
  without it = cell ERROR `missing metrics`. Arc A380: Netflix pair, both
  checkerboards, BBB 4K, one frame, blend / weight / cap / force-zero sets:
  every output `==`. Guards: `test_sycl_twin_option_parity`
  (`test_float_motion_motion3`, `_with_weight_and_cap`, `_one_frame`,
  `_force_zero`; `==`).
- **`float_motion_sycl.cpp` SAD = CPU order, bit for bit (ADR-1409,
  ADR-1411).** `float_motion.c::compute_motion_simd()` = one fp32 running
  sum per row, one fp32 sum over rows, fp32 division. Twin:
  `launch_float_motion_row_sad()` = ONE work-item per row
  (`sycl::range<1>(height)`, sub-group size 16), `fm_row_sad()` = plain
  `for (j = 0; j < width; j++)` loop, readback `height` floats; host =
  `vmaf_float_motion_score_from_row_sads()` (`../float_motion_sad.h`). No
  group / sub-group / atomic reduction in TU, no host sum in
  `collect()`: any other shape differs in low bits (was 1.36e-4 on 1080p
  checkerboards). Blur kernel writes blur only; blur =
  `convolution_f32_c_s()` tap order, needs strict FP line (ADR-1367).
  CPU SAD order changes upstream -> change kernel + helper in same PR.
  Arc A380: Netflix pair 48 / 48, checkerboards 3 / 3, BBB 4K 200 / 200
  identical (`motion`, `motion2`), also 10 / 12 / 16 bit. Cost: row pass
  re-reads both blurred planes, 0.70 ms per 4K frame (old reduction 0.34):
  3.85 -> 4.23 ms / frame. Sub-group 8 measured fastest (16: 0.82, 32:
  1.12); `select_from_group` chain over 16 consecutive pixels 1.10,
  lane-uniform 16-wide chain 0.80: do not retry without new idea.
  Scratch-free (ADR-1395). `EXACT_TWINS` lists `float_motion`: `sycl`.
  Guards: `test_sycl_float_motion_parity` (+ `_large`, `==`, 8 / 10 / 12
  bit), `test_sycl_kernel_source_contract.py` (five planted regressions).

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `float_motion_sycl.cpp` | `float_motion.c` | `test_sycl_float_motion_parity.c` (bit-exact, every frame, 8 / 10 / 12 bit) | ADR-0946 (round 3), ADR-1411 |
