- **`picture_compute_geometry` off-by-one for odd-height / odd-width YUV
  4:2:0 inputs (Research-0094, ASan heap-OOB in `ciede::scale_chroma_planes`).**
  `picture_compute_geometry` in `core/src/picture.c` computed chroma plane
  dimensions via plain right-shift (`h >> ss_ver`, `w >> ss_hor`), which is
  floor division. For odd luma dimensions, the YUV 4:2:0 standard requires
  ceiling division — `ceil(luma / 2) = (luma + 1) >> 1` — so that every luma
  sample is covered by at least one chroma sample. With the floor formula, a
  577 × 323 input produced chroma planes of 288 × 161 instead of the correct
  289 × 162, under-allocating by one row and one column. Any extractor that
  iterates `pic->h[1]` rows would then read one row past the chroma allocation,
  producing an ASan-detected heap out-of-bounds access.

  Fixed by replacing `h >> ss_ver` / `w >> ss_hor` with ceiling arithmetic
  `(h + (unsigned)ss_ver) >> ss_ver` / `(w + (unsigned)ss_hor) >> ss_hor`
  in `picture_compute_geometry`. The same fix is applied to the two geometry
  copies in `core/src/cuda/picture_cuda.c`
  (`vmaf_cuda_picture_alloc_pinned` and `vmaf_cuda_picture_alloc`) and to the
  local subsampling calculations in
  `core/src/feature/cuda/integer_psnr_cuda.c` and
  `core/src/feature/cuda/integer_psnr_hvs_cuda.c`. The `min_sse`
  psnr_max divisor in `core/src/feature/integer_psnr.c` is also corrected.

  Even-dimension inputs are unaffected: for even `h`, `(h + 0) >> 1 == h >> 1`.

  Verification: `ASAN_OPTIONS=halt_on_error=1 ./build-chroma-asan/tools/vmaf
  --reference /tmp/odd.yuv --distorted /tmp/odd.yuv --width 577 --height 323
  --pixel_format 420 --bitdepth 8 --feature ciede --threads 4` exits 0 with
  no ASan reports. New regression test `test_picture_odd_dim_chroma_ceiling`
  in `core/test/test_picture.c` asserts `pic.w[1] == 289`, `pic.h[1] == 162`
  for 577 × 323 YUV 4:2:0, and equivalents for 4:2:2 and 4:4:4.
  53 / 53 `meson test` pass.
