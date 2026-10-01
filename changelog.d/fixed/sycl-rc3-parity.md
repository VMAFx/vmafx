### Fixed
- **SYCL**: Fixed identical/flat-frame handling in `float_ssim_sycl` and `integer_ssim_sycl` by implementing the CPU's exact arithmetic without identical-window shortcuts, grouping integer terms as `((w*a)*b)/den`, and preserving ADR-1370 fp32 frame-mean rounding.
- **SYCL**: Fixed a bug where `psnr_sycl` produced incorrectly scaled scores under `--subsample` by adding the missing `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag.
- **SYCL**: Fixed a bug where `motion_v2_sycl` diverged from the CPU by applying `motion_fps_weight` and the `motion_max_val` cap in `collect()` and emitting scores for one-frame inputs in `flush()`.
