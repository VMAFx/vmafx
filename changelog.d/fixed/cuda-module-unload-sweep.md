# fix(cuda): unload PTX modules in 18 CUDA feature extractors' close paths

`cuModuleLoadData` reserves ~200-500 KB of GPU-resident module backing
storage per call that survives `cuStreamDestroy` and (on a primary
context) `cuCtxDestroy`. 18 CUDA feature extractors loaded the module
handle into a stack-local `CUmodule` and dropped it on the floor in
`init_fex_cuda`, leaking the module every time the extractor was
re-initialised — typical impact of one re-init per invocation in tools
like `vmaf-tune` that sweep many GOPs through fresh contexts.

Fix: move the `CUmodule` handle into each extractor's state struct and
add a guarded `cuModuleUnload` in `close_fex_cuda` (between
`cuStreamSynchronize` and `cuStreamDestroy` where the extractor owns
its stream, or before `vmaf_cuda_kernel_lifecycle_close` where the
lifecycle template owns it). Mirrors the canonical fix already in
`ssimulacra2_cuda.c` (ADR-0356). See
`core/src/cuda/AGENTS.md` "Lifecycle invariants" for the per-extractor
checklist this sweep discharges.

Affected extractors (one commit per file in the PR):
`float_psnr`, `float_vif`, `float_adm`, `float_motion`,
`integer_ms_ssim`, `integer_ciede`, `integer_motion_v2`,
`integer_adm` (4 modules), `integer_moment`, `ssim`, `integer_psnr`,
`integer_cambi`, `integer_psnr_hvs`, `integer_motion`,
`integer_ssim`, `integer_vif`, `speed_chroma`, `speed_temporal`.
