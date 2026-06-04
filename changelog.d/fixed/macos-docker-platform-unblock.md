fix(build): add `<string_view>` include in vmaf.cpp; fix MinGW64 constinit mutex build failure

Apple Clang does not pull `std::string_view` transitively through `<string>` machinery;
Linux GCC/Clang do. Adding the explicit include unblocks the macOS Clang and
FFmpeg-macOS-clang CI jobs.

fix(cuda): replace fabricated `VmafCudaFunctions` type with correct `CudaFunctions` in
13 CUDA feature-extractor `close` callbacks

PR #516 introduced `cuModuleUnload` teardown calls using a non-existent type alias
`VmafCudaFunctions`. The actual type defined in `core/src/cuda/common.h` is
`CudaFunctions`. This caused build failures in any CUDA-enabled build (Docker,
Linux-GCC-all-backends, Build-Windows-MSVC+CUDA). Affected files:
float_adm_cuda.c, float_motion_cuda.c, float_psnr_cuda.c, float_vif_cuda.c,
integer_cambi_cuda.c, integer_ciede_cuda.c, integer_moment_cuda.c,
integer_motion_v2_cuda.c, integer_ms_ssim_cuda.c, integer_psnr_cuda.c,
integer_ssim_cuda.c, integer_vif_cuda.c, ssim_cuda.c.
