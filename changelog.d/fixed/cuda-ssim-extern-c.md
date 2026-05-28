## Fixed: `--feature ssim --backend cuda` P0 crash (`CUDA_ERROR_NOT_FOUND`)

`core/src/feature/cuda/integer_ssim/integer_ssim_score.cu` was compiled as a
C++ translation unit without `extern "C"` guards on its three `__global__`
entry points. The C++ name-mangling made those symbols unfindable by the host
glue's `cuModuleGetFunction` calls, producing `CUDA_ERROR_NOT_FOUND (500)` at
runtime and making the integer SSIM CUDA path completely non-functional since
it was introduced in ADR-0564. Fixed by adding the `extern "C"` block,
consistent with every other `.cu` file in the same directory. Surfaced by the
PR #77 `ncu` profile. ADR-0745.
