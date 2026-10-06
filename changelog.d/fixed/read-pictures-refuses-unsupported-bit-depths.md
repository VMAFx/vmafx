- **The float extractors refuse 9, 11, 13, 14 and 15-bit pictures instead of
  scoring them wrongly.** `float_ssim`, `float_ms_ssim`, `float_adm`,
  `float_vif` and `float_motion`, on the CPU and on every GPU backend, scaled
  10, 12 and 16-bit samples only and read every other depth above 8 as 8-bit
  bytes, so they returned wrong scores for those depths without an error. They
  now fail at initialisation with `-EINVAL` and a log line naming the extractor
  and the depth. Other extractors keep their odd-depth support. The CLI and the
  FFmpeg filters never passed these depths; programs using the C API could.
