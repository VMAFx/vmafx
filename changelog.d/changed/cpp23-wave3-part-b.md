### C++23 Wave 3 Part B — psnr_tools, luminance_tools, mkdirp (ADR-0731)

Three small feature utility translation units are converted from C to C++23:

- `core/src/feature/psnr_tools.cpp`: format-to-PSNR-constant dispatch replaced with a
  compile-time `constexpr std::array` table; `std::string_view` comparison eliminates
  repeated `strcmp` / `strlen` traversals.
- `core/src/feature/luminance_tools.cpp`: `MAX()` macro replaced with `std::max` /
  `std::clamp`; BT.1886 / PQ constants become `constexpr`; EOTF dispatch uses
  `std::string_view`; internal helpers gain `[[nodiscard]]`.
- `core/src/feature/mkdirp.cpp`: `goto fail` cleanup pattern replaced with RAII
  `std::string` path management, eliminating the need for paired `free()` calls on
  every exit path.

Each TU is compiled as an isolated `static_library` with `override_options:
['cpp_std=c++23']` per the ADR-0708 playbook, so the dialect override is strictly
scoped. The `override_options` can be dropped once PR #48
(project-wide `cpp_std=c++23`) merges.

Also removes stale `test_ansnr_simd` entries from `core/test/meson.build` left over
from the ansnr feature drop (commit `70ed8b3`).
