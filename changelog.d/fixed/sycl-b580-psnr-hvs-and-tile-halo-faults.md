- **SYCL: `psnr_hvs_sycl` no longer crashes on Intel Arc B580 (Xe2).** The Intel
  GPU compiler (IGC 2.41.5) crashed the host process with SIGSEGV while
  compiling the kernel at SIMD32 for Xe2, triggered by the 8x8 DCT running in
  one work-item's private memory. The DCT now runs in local memory, split
  across work-items. Scores are bit-identical on devices where the old kernel
  ran, and a 4K frame takes 130 ms instead of 208 ms on a UHD 770
  (`T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29`).
- **SYCL: small frames no longer lose the device.** `adm_sycl`, `vif_sycl`,
  `motion_sycl`, `motion_v2_sycl`, `float_motion_sycl` and `float_vif_sycl`
  loaded their local-memory tiles, padding lanes included, with a single edge
  reflection. On small planes that read outside the buffer and raised
  `UR_RESULT_ERROR_DEVICE_LOST` on an Arc B580 and a UHD 770: `adm_sycl` for
  frames 64 rows high or less, `vif_sycl` up to at least 96x64 and
  `motion_sycl` up to 33x33 on the B580. The loads now stay inside the plane;
  scores for larger frames are unchanged
  (`T-SYCL-TILE-HALO-OOB-READ-2026-09-29`).
- **SYCL: `vif_sycl` hands frames below 16 pixels to the CPU.** Its filters
  reach more than one reflection outside planes smaller than 16 pixels in
  either dimension, which lost the device at 8x8. Model dispatch now computes
  such frames with the CPU `vif` extractor, bit-identical to a CPU run; a
  direct `--feature vif_sycl` request fails with an error naming `vif`
  (`T-INTEGER-VIF-TINY-FRAME-GUARD-2026-09-29`).
- **SYCL: `vif_sycl` scores are correct for odd widths.** When the width at
  any scale was odd (for example 854x480, 1366x768 or 853x480), scales 1-3 read
  the downsampled plane at the wrong row stride and drifted from the CPU by up
  to 1.2e-3 at those sizes and 3.3e-2 on tiny frames, which also moved the
  VMAF score. They now match the CPU within 1e-6 at those sizes
  (`T-SYCL-VIF-ODD-WIDTH-RD-STRIDE-2026-09-29`).
- **SYCL: a device fault now fails the frame.** After a failed graph wait the
  `adm_sycl`, `vif_sycl`, `motion_sycl`, `psnr_sycl` and `float_moment_sycl`
  extractors emitted scores for that frame from stale buffers (about 1.0 for
  every ADM scale) and `vmaf_read_pictures()` returned 0; only a later frame's
  upload failed. They now return `-EIO` for the faulted frame
  (`T-SYCL-GRAPH-WAIT-ERROR-DROPPED-2026-09-29`).
