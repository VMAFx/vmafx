- **`motion_cuda` computes the CPU `motion` arithmetic.** The CUDA twin blurred
  each frame and differenced the blurred frames, while the CPU (since the
  upstream pipelined-motion port) blurs the frame difference and rounds after
  each filter pass; the two orders round differently (the SYCL twin with the
  same order was up to 2.0e-4 off on 17x17 frames and 1.3e-5 on the Netflix
  576x324 pair). `motion_cuda` now runs the kernel `motion_v2_cuda` already
  used, whose arithmetic is the CPU's, and its debug `integer_motion` score
  is the CPU's (weighted by `motion_fps_weight`, capped at `motion_max_val`).
  Each frame is ordered against the previous one on the device instead of by
  the engine's context barrier, and the eight-frame batch readback waits once
  instead of twice. `motion_v2_cuda`'s SAD is unchanged. On an RTX 4090
  `integer_motion2` / `integer_motion3` now equal the CPU's on the Netflix
  pair and on 50 frames of a 3840x2160 clip, where they were 1.26e-5 and
  6.9e-5 off (ADR-1372; `docs/state.md`,
  `T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29`;
  [CUDA backend](docs/backends/cuda/overview.md#cpu-parity-motion-options-and-tiny-frames-2026-09-30)).
- **CUDA integer ADM and VIF guard tiny frames like their SYCL twins.** The
  integer ADM DWT kernels take their row and tap arithmetic from a header a
  device-free test replays for every plane height: from the 17-row ADM minimum
  up no load leaves the plane, and the scale-0 load is clamped into the plane
  below it. `vif_cuda` needs 16 pixels in each dimension; model dispatch and
  `--backend cuda --feature vif` compute smaller frames with the CPU `vif`, and
  `--feature vif_cuda` on such a frame fails `init()` instead of returning
  scores from clamped taps (ADR-1374).
