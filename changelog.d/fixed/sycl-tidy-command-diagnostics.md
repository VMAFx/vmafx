# Changelog fragment

- **SYCL clang-tidy now preserves every diagnostic option.** Synthetic
  compile commands still remove Intel device-only arguments and translate the
  strict floating-point spelling, but no longer discard `-pedantic` or other
  warning flags to make analyzer output quieter.
