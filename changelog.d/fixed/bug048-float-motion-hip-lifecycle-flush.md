- Restored the HIP float-motion force-zero close callback and made its
  option-derived tail flush idempotent, preventing a feature-name dictionary
  leak and repeated-flush failure.
