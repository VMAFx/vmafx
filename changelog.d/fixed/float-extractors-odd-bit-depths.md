- **`float_psnr` and `ciede` score 9, 11, 13, 14 and 15 bits; the `float_*` extractors read them correctly (RC4 WP13 follow-up, ADR-2164).**
  `float_psnr` and `ciede` refused those depths (and, with CUDA, HIP and SYCL twins, so did the `float_psnr`
  twins); `picture_copy()` read them as 8-bit bytes, so `float_ssim`, `float_ms_ssim`, `float_adm`,
  `float_vif` and `float_motion` returned wrong scores without an error. `psnr_hvs` above 12 bits stays
  refused. The Metal `float_psnr` refuses the new depths by name until it has run on an Apple device.
