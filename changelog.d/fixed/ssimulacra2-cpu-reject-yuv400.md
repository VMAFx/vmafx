- **The CPU `ssimulacra2` extractor no longer crashes on 4:0:0 input.** Its
  init ignored the pixel format, so a luma-only picture passed to
  `vmaf_read_pictures()` through the C API reached the colour conversion, which
  read the missing U plane through a NULL pointer (a segmentation fault in
  `ssimulacra2_picture_to_linear_rgb_avx512` on an AVX-512 host). Init now
  refuses 4:0:0 with `-EINVAL` and an error message, as `ssimulacra2_sycl`
  does. The `vmaf` CLI was not affected: it rejects `-p 400` and converts Y4M
  `mono` input to 4:2:0. See [SSIMULACRA 2](docs/metrics/ssimulacra2.md).
