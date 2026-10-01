- **`float_ms_ssim_sycl` computes the CPU's arithmetic.** The SYCL twin
  added the decimate taps in two roundings where the CPU fuses them, kept the
  Gaussian window sums and the luminance, contrast and structure terms in
  fp32 where the CPU uses `double`, and combined unrounded per-scale means.
  Measured on an Arc A380 it matched the CPU on none of 104 frames (6.9e-8 on
  the Netflix 576x324 pair, up to 2.98e-6 on 1080p checkerboards, 1.23e-6 at
  3840x2160). It now follows the reference operation for operation, with the
  CPU's `double` values carried as exact pairs of floats and the frame sums
  in 64-bit fixed point
  ([ADR-1414](docs/adr/1414-sycl-float-ms-ssim-cpu-arithmetic.md), after
  ADR-1403 for CUDA). Every per-scale mean of every measured frame equals
  the CPU's, `enable_lcs` and `enable_chroma` outputs included; the score
  equals a GCC build's on 253 of 254 frames, the other differing by 1.1e-16
  through the host `pow()` of an Intel-compiler build. The parity gate
  compares the twin with tolerance 0. A 3840x2160 frame takes 42.6 ms on the
  A380 against 31.4 before. Stored `float_ms_ssim_sycl` outputs change in
  their low digits by at most the differences above. The HIP and Metal twins
  keep the old arithmetic.
