### Fixed: USM leak on init failure in all SYCL feature extractors (SY-2a)

All 18 `init_fex_sycl` / `init_chroma_sycl` / `init_temporal_sycl` functions in
`core/src/feature/sycl/` now call the corresponding `close_fex_*` function on every
early-return failure path after USM allocation begins. Previously, any intermediate
allocation failure returned `-ENOMEM` without freeing already-allocated host and device
USM pointers, causing a persistent USM leak for the lifetime of the SYCL context.

Affected files: `float_adm_sycl.cpp`, `float_vif_sycl.cpp`, `float_motion_sycl.cpp`,
`float_psnr_sycl.cpp`, `integer_adm_sycl.cpp`, `integer_cambi_sycl.cpp`,
`integer_ciede_sycl.cpp`, `integer_moment_sycl.cpp`, `integer_motion_sycl.cpp`,
`integer_motion_v2_sycl.cpp`, `integer_ms_ssim_sycl.cpp`, `integer_psnr_hvs_sycl.cpp`,
`integer_psnr_sycl.cpp`, `integer_ssim_sycl.cpp` (both ssim and issim variants),
`integer_vif_sycl.cpp`, `speed_chroma_sycl.cpp`, `speed_temporal_sycl.cpp`,
`ssimulacra2_sycl.cpp`.

Also covers post-allocation failure paths: `std::malloc` failures for host-side LUT
buffers in `integer_adm_sycl.cpp` and `integer_vif_sycl.cpp`, `feature_name_dict`
allocation failures, and `vmaf_sycl_graph_register` failures.

Pattern matches CUDA PR #94. No behaviour change on the success path.
