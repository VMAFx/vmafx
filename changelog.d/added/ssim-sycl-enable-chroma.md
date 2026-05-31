- `float_ssim_sycl`: add `enable_chroma` option (default `false`) and `n_planes`
  field to `SsimStateSycl`, mirroring the CUDA twin (PR #950). The v1 kernel
  is luma-only; `n_planes` is clamped to 1. Multi-plane dispatch is deferred
  to v2.
