- **`float_ssim` runs on the CUDA device at every scale and equals the CPU's
  score (ADR-1399).** `float_ssim_cuda` computed scale 1 only, so at
  1920x1080 and 3840x2160 `--backend cuda --feature float_ssim` and models
  computed the feature on the CPU and printed a fallback warning. The twin now
  reduces both pictures on the device the way the CPU does (the automatic
  scale and every `scale` from 1 to 10) and adds its two Gaussian passes in
  double precision like the CPU. On an RTX 4090 every measured frame equals
  `--backend cpu` at `--precision max` (576x324, 1920x1080 and 3840x2160; 8,
  10, 12 and 16 bits; `enable_lcs`, `enable_db` and `clip_db` included), where
  the twin used to be 1 to 3 units in the last fp32 place off on every frame.
  A 3840x2160 frame takes 3.0 ms through the CLI instead of 18.9 ms with the
  CPU fallback (11.0 ms for the CPU extractor on 16 threads). An explicit
  `scale=1` on a large picture is slower than before, 3.9 ms instead of 3.1 ms
  per 3840x2160 frame, because the double-precision sums then cover the full
  picture. `--feature float_ssim_cuda` no longer fails at sizes that decimate;
  it fails only when the reduced picture is smaller than 11x11. See the
  [CUDA guide](docs/backends/cuda/overview.md#float_ssim-runs-on-the-device-at-every-scale-adr-1399-2026-10-01).
