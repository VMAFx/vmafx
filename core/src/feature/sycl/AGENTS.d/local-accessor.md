---
paths:
  - core/src/feature/sycl/sycl_compat.h
  - core/src/feature/sycl/integer_ssim_sycl.cpp
invariant: Stencil/convolution SYCL kernels MUST use local_accessor for tap access.
area: runtime-and-kernels
---
<!-- markdownlint-disable MD013 MD060 -->
# Local memory access for stencil kernels

- **Stencil/convolution SYCL kernels MUST use `local_accessor` for tap
  reuse** (ADR-0458 / SY-2). Separable filter (Gaussian, box,
  motion-blur) with more than 3 taps **must** stage required input
  region into shared local memory (SLM) via
  `local_accessor` + cooperative tile-load loop + barrier — follow
  pattern in `float_vif_sycl.cpp`, `float_motion_sycl.cpp`, and (post
  ADR-0458) `integer_ssim_sycl.cpp`.
  Bare `parallel_for<range<N>>` reading global memory for every tap =
  lint violation for convolution kernels — use `nd_range` instead.
