- CUDA backend, first HISS-21 batch (`core/src/cuda/common.c`,
  `core/src/cuda/picture_cuda.c` and the ciede / moment / psnr /
  float_psnr / float_motion / integer_ssim / motion_v2 / ssim feature
  wrappers): every explicit `goto` in these translation units is gone and
  their oversized setup and dispatch functions are split into named
  `static` helpers (HISS-01 / HISS-04, NASA JPL Power-of-10 rules 1
  and 4). Each cleanup label became one `*_unwind` helper holding the
  label's statements verbatim, so every exit path still releases the same
  resources in the same order; the fall-through cascades
  (`free_rb_wgt` -> `free_rb_ssim` -> `free_bufs` in `ssim_cuda.c`,
  `free_priv` -> `free_data` -> `fail_no_data` and the ADR-1090 graduated
  ladder in `picture_cuda.c`, `fail` -> `fail_after_stream` in
  `common.c`) became one helper with an explicit stage argument. Feature
  scores are unchanged: no arithmetic expression was split across a helper
  boundary and no reduction changed accumulation order. `CHECK_CUDA_GOTO`
  and the labels it targets are untouched.
