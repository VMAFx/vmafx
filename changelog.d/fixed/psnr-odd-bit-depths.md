- **The `psnr` extractor scores 9, 11, 13, 14 and 15 bits (RC4 WP13).** It refused every depth but 8, 10,
  12 and 16 with `-EINVAL` (the CLI printed "problem reading pictures"), although the engine reads 8 to 16;
  the raw and y4m inputs of those depths now reach it. `float_psnr` and `ciede` still take 8, 10, 12 and
  16 only and `psnr_hvs` up to 12 (`docs/state.md`).
