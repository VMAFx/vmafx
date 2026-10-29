- **Device twins refuse unmeasured odd bit depths
  ([ADR-3383](../docs/adr/3383-odd-depth-twin-allow-list.md), #2476).** A CUDA,
  SYCL, HIP or Metal extractor at 9, 11, 13, 14 or 15 bits now returns `-EINVAL`
  by name unless its parity test carries that depth: the CUDA, SYCL and HIP
  `float_psnr` and `ciede` twins up to 15 bits, the `psnr_hvs` twins up to 11
  bits. Before, twins outside the five `picture_copy()` families were accepted
  unmeasured, and the `float_moment` twins returned unscaled moments there.
