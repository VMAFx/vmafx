- **`float_motion` on the HIP backend is bit-identical to the CPU, with every
  option.** The CPU extractor adds the absolute differences of a row into one
  `float`, the row sums into a second one, and divides in `float`; those
  running sums round at every step, so the score depends on the order of the
  additions. `float_motion_hip` added 16x16 blocks on the device and the
  blocks in `double` on the host, and was 3e-6 from the CPU on the Netflix
  576x324 pair, 1.4e-4 on 1080p checkerboards (above the 5e-5 cross-backend
  tolerance) and 2.2e-4 with `motion_add_scale1`. It now stores every absolute
  difference and adds each row in the CPU's order on the device, for the
  half-size term of `motion_add_scale1` and the chroma planes of
  `motion_add_uv` too, and `motion`, `motion2` and `motion3` equal
  `--backend cpu` at `--precision max` on a gfx1036: the Netflix pair at 8 and
  10 bits, both 1080p checkerboard pairs and BBB 3840x2160, with seven option
  sets (1617 of 1617 values; 237 before). The parity gate compares the CPU and
  HIP `float_motion` cells with tolerance 0. A 3840x2160 frame takes 19.8
  instead of 18.1 ms on that device, and 24.7 instead of 21.0 with
  `motion_add_scale1`. Re-run any stored HIP `float_motion` output
  ([ADR-1419](docs/adr/1419-hip-float-motion-cpu-float-sum.md),
  [HIP backend](docs/backends/hip/overview.md#float_motion_hip-options)).
