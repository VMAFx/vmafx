# C++23 Wave 1: `opt.c` → `opt.cpp` (ADR-0721)

`core/src/opt.c` converted to `core/src/opt.cpp` using C++23 `std::optional<T>` for
the internal parse helpers (`parse_bool`, `parse_int`, `parse_double`). The public C
ABI (`vmaf_option_set`) is unchanged: same signature, same return contract, same
case-sensitive string comparison behaviour. `opt.h` now carries `extern "C"` guards so
all C callers compile without modification. Build: `opt_cpp23_lib` isolated static
library compiled at `cpp_std=c++23`, following the ADR-0708 playbook.

Also: removes stale `test_ansnr_simd` test registration from `core/test/meson.build`
(the source files were deleted in ADR-0720 / PR #38 but the meson stanza was not
cleaned up, causing `meson setup` to fail).
