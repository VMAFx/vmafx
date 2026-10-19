- **`M_PI` and `M_E` come from `<math.h>` on every platform (Netflix/vmaf
  `4e150067b`).** The build defines `_USE_MATH_DEFINES` on Windows (MSVC,
  clang-cl, icx-cl and MinGW-w64), and the local copies of the constants
  (fourteen in the feature sources, the SYCL twins included, and four in
  tests) are gone. Scores are unchanged: the copies held the same double.
