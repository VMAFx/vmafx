Fix CUDA stream and event leaks on init error paths in `vmaf_cuda_picture_alloc`
(`picture_cuda.c`), `integer_vif_cuda`, `integer_adm_cuda`, `integer_motion_cuda`,
and `ssimulacra2_cuda` init functions. Previously, a single shared `fail` label
only popped the CUDA context without calling `cuStreamDestroy`, `cuEventDestroy`,
or `cuModuleUnload` on handles already created above the failing step. Replaced
with graduated cleanup label chains. No score change; `compute-sanitizer
--tool memcheck` reports no leaked handles after this fix. ADR-1090.
