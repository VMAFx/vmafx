- **`float_psnr` and `ciede` score 9, 11, 13, 14 and 15 bits; the CPU `float_*` extractors read them correctly (ADR-2164).**
  `float_psnr` and `ciede` refused those depths (and, with CUDA, HIP and SYCL twins, so did the `float_psnr`
  twins); `picture_copy()` read them as 8-bit bytes, so `float_ssim`, `float_ms_ssim`, `float_adm`,
  `float_vif` and `float_motion` returned wrong scores without an error. `psnr_hvs` above 12 bits stays
  refused. The Metal `float_psnr` refuses the new depths by name until it has run on an Apple device, and the device
  twins of `float_ssim`, `float_ms_ssim`, `float_adm`, `float_vif` and `float_motion` refuse them until
  they are compared with the CPU there.
