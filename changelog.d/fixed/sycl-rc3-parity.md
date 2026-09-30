### Fixed
- **SYCL**: Fixed a bug where SSIM and MS-SSIM extractors emitted `+inf` for identical frames when `enable_db` was enabled by removing the `exactly 1` shortcut.
- **SYCL**: Fixed a bug where the `psnr_sycl` extractor produced incorrectly scaled scores with `--subsample` by adding the missing `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag.
- **SYCL**: Fixed a bug where `motion_v2_sycl` diverged from the CPU due to incorrect application of `fps_weight` and max clipping.
- **SYCL**: Fixed out-of-bounds read/write errors in `cambi_sycl` on short frames (e.g. 1920x64, 1920x128).
