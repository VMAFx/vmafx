- **float_psnr_metal**: add `enable_chroma` option (default `false`); when set to
  `true`, Cb and Cr planes are dispatched and included in the aggregate MSE
  (equal-weight average), matching the chroma-guard pattern from psnr_vulkan.c.
