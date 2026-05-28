- **`--feature ssim --backend cuda` silently broken since introduction.**
  `integer_ssim/integer_ssim_score.cu` defined three `__global__`
  kernels (`integer_ssim_horiz_8bpc`, `integer_ssim_horiz_16bpc`,
  `integer_ssim_vert_combine`) without an `extern "C"` block. nvcc
  C++ name-mangling caused `cuModuleGetFunction` to return
  `CUDA_ERROR_NOT_FOUND`, so `init_fex_cuda` returned `-EINVAL` and
  the `ssim` feature was never computed. Fixed by wrapping all three
  entry points in `extern "C" { }`. A CI audit script
  (`scripts/dev/check-cuda-extern-c.sh`) and an `AGENTS.md` invariant
  note prevent recurrence. (ADR-0747; Research-0747; sweep of all 24
  `.cu` kernel files confirmed no other instances.)
