### C++23 Wave 2: project-wide `cpp_std=c++23` + `dict.c` → `dict.cpp` (ADR-0727)

- The Meson project default is now `cpp_std=c++23` (was `c++11`). New C++ source
  files in `core/src/` no longer need an isolated static library with
  `override_options` to use C++23 features.
- `core/src/dict.c` is converted to `core/src/dict.cpp` using `std::expected`,
  `std::string_view`, `std::unique_ptr`, and `[[nodiscard]]`. The public C ABI
  (`vmaf_dictionary_*`, `vmaf_feature_dictionary_*`) is unchanged.
- Build toolchain minimum floor raised to **gcc >= 13 / clang >= 16** for the
  C++ compilation units. The C units and public headers are unaffected. The CI
  matrix (gcc 14+, clang 22) and the `dev/Containerfile` already satisfy this
  requirement.
