- **`float_ssim_sycl` combined formula residual eliminated.**
  Arithmetic alignment from PR #1645 (`core/src/feature/sycl/integer_ssim_sycl.cpp`,
  evaluating exact per-pixel $l \cdot c \cdot s$ in fp32 pairs, fixed-point work-group
  sums, and double host reduction) eliminated the residual against the CPU reference.
  Measured on an Intel Arc A380 under the Linux `xe` driver: max absolute difference
  against `--backend cpu` is 0.000e+00 across all 48 frames of Netflix 576x324 and
  all frames of BBB 3840x2160 at both auto scale and explicit `scale=1`.
  `test_sycl_twin_option_parity` passes 13/13 with exact match on flat identical frames.
