- **ROI high-bit-depth input:** Keep the brightest 10-, 12- and 16-bit
  luma samples white when converting to the saliency model's 8-bit input;
  rounded values now saturate instead of wrapping to black. Validate
  private reader and placeholder extents before accessing their buffers,
  with exhaustive conversion and sanitizer boundary regressions.
