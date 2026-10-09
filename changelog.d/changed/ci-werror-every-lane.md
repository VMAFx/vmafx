- **Every CI lane that compiles C, C++, Rust or Go now fails on a warning
  ([ADR-2828](docs/adr/2828-ci-werror-every-lane.md)).** The 36 lanes that were not gated yet
  (golden, DNN, MCP smoke, coverage, sanitizers, the tidy, cppcheck and CodeQL builds, nightly,
  fuzz, Arc parity, the consumer comparison, Windows MSVC+SYCL, the Level Zero loader builds, the
  cgo package and the cargo steps) are gated, and every lane spells its switch on the build command
  so praetor's build-warnings gate reads it; the HISS-10 exception list is empty. Fixed on the way:
  two deprecated C runtime calls the Windows icx-cl build reported in test headers, four
  out-of-order initialisers clang reported in the Metal `integer_vif` and `float_ssim` extractors,
  and three `-Wmaybe-uninitialized` sites gcc reports when the static library is linked with LTO
  (`float_vif`, `speed`, `ssim`; no score changes). A static MSVC build's configure no longer prints
  Meson's pkg-config warning that `-lvmaf` / `-lvmafx` may not find `vmaf.lib` / `vmafx.lib`: the
  `.pc` files carry the same `Libs` through a `vmafx_libdir` variable.
