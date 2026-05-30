### Changed

- `core/src/log.c` converted to `core/src/log.cpp` (real C++23) and wired into
  the build as the second pilot under ADR-0708 (first was `dict.c`/`dict.cpp`
  per ADR-0727). The public C ABI (`vmaf_log`, `vmaf_set_log_level`) is
  preserved verbatim via `extern "C"` guards in `core/src/log.h`. Internal
  implementation now uses `std::clamp` for level bounds, `std::array<std::string_view, 4>`
  for the per-level label/colour tables, and `assert`-checked NUL-termination
  on the string_views before passing to `fprintf`. Behaviour, output format,
  and stderr destination are byte-identical to the prior C implementation.
  The orphan `test_log` executable (built but never `test()`-registered) is
  also wired into the `fast` suite as part of the same change, mirroring
  draft PR #315's orphan-test sweep.
