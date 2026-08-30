### CUDA extractor `cuModuleUnload` teardown (16 extractors)

All 16 CUDA feature extractors that called `cuModuleLoadData` in their
`init_fex_cuda` callback but omitted the matching `cuModuleUnload` in
`close_fex_cuda` have been fixed. The GPU-resident PTX module backing
store (200-500 KB per module) leaked on every `vmaf_close()` cycle.

The fix promotes each `CUmodule` handle from a local variable in `init`
to a state-struct field, then calls `cuModuleUnload` in `close` guarded
by a null check so partial-init failure paths remain safe.

No score change — teardown only. Prerequisite for the planned
persistent-process optimisation that amortises CUDA context init cost
across clips in the CHUG pipeline (~70 min savings at scale).

Affected extractors: `float_adm`, `float_ansnr`, `float_motion`,
`float_psnr`, `float_vif`, `integer_adm` (4 modules), `integer_cambi`,
`integer_ciede`, `integer_moment`, `integer_motion`, `integer_motion_v2`,
`integer_ms_ssim`, `integer_psnr`, `integer_psnr_hvs`, `integer_ssim`,
`integer_vif`.
