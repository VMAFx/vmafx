- **build(meson):** Remove the redundant per-target
  `override_options : ['cpp_std=...']` entries (and the `libvmaf_cpu_cpp_std`
  token variable) from `core/src/meson.build`, `core/test/meson.build`,
  `core/tools/meson.build` and `core/test/fuzz/meson.build` — epic #1241
  leftover. The C++ standard is project-wide since ADR-1003 / ADR-1056
  (`add_project_arguments` in `core/meson.build`), and meson emits that flag
  after any per-target `cpp_std=` option, so the 14 overrides never changed
  the standard a TU was compiled at (`-std=c++23 ... -std=c++26`, last flag
  wins). `meson introspect --buildoptions` is option-for-option identical
  before and after for the CPU configure (96 options, no value or metadata
  delta); the only compile-command delta is the dropped leading
  `-std=c++23` / `-std=c++20` token on the 15 formerly overridden TUs. The two
  `b_lto=false` overrides (AVX-512 symbol visibility, macOS `test_output`)
  are real and stay.
