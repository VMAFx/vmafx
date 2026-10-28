- **Picture conversion logs at DEBUG (Netflix/vmaf PR 1710).** A run whose
  pictures need no conversion logs one `picture conversion: none needed` line,
  and each conversion context logs its source and target (size, bit depth,
  range, primaries, transfer, matrix) when it is created; see
  [pictures](docs/api/pictures.md#converting-to-a-models-conversion-target).
