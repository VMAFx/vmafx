Convert `core/src/fex_ctx_vector.c` to C++23 (`fex_ctx_vector.cpp`), Wave 2 of the ADR-0708
internal C++ migration plan. Public C ABI preserved via `extern "C"` guards in the header.
Establishes the `extern "C"` + pre-`<atomic>` include pattern for any future C++ TU that
consumes `feature_extractor.h`. Fixes stale `test_ansnr_simd` meson reference left by the
ADR-0720 ansnr feature drop. (ADR-0723)
