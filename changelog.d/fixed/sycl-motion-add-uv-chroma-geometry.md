- **`motion_sycl` sizes chroma planes correctly for 4:2:2 and 4:4:4 input.**
  With `motion_add_uv=true`, `motion_configure_chroma()` previously assumed
  4:2:0 subsampling (`chroma_w = (w + 1) >> 1`, `chroma_h = (h + 1) >> 1`)
  for all input formats, causing `motion_stage_chroma()` on 4:2:2 and 4:4:4
  input to stage only a sub-rectangle of each chroma plane and normalize the
  SAD by an incorrect area. Chroma dimensions are now derived via
  `vmaf_chroma_extent()` from `picture_geometry.h` according to the pixel
  format, and `test_sycl_motion_add_uv_parity` verifies parity with the
  fixed-point oracle across 4:2:0, 4:2:2, and 4:4:4.
