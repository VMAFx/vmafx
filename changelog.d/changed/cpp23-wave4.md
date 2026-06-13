# C++23 Wave 4 — output writers (ADR-0733)

Converted `core/src/output.c` → `output.cpp` as the Wave 4 instalment of the
ADR-0708 C++23 internals migration. All four format writers (XML, JSON, CSV,
MicroDVD subtitle) are now compiled under `-std=c++23` via an isolated
`static_library` in `core/src/meson.build`.

C++23 idioms applied:

- **`LocaleGuard` RAII**: `vmaf_thread_locale_pop` is now called on destruction,
  guaranteeing cleanup on all exit paths without requiring explicit `goto cleanup`
  patterns.
- **`std::string_view`**: score-format string parameter threaded through all
  internal helpers, eliminating repeated implicit `strlen` calls.
- **`[[nodiscard]]`**: all four public entry points (`vmaf_write_output_xml`,
  `vmaf_write_output_json`, `vmaf_write_output_csv`, `vmaf_write_output_sub`)
  now produce a compile-time diagnostic if callers discard the return value.
- **`constexpr`**: `pool_method_name` and `DEFAULT_SCORE_FORMAT` promoted to
  `constexpr`.
- **`static_cast<VmafPoolingMethod>`**: explicit enum casts replace implicit
  `unsigned → enum` conversions that were valid in C but ill-formed in C++.
- **`nullptr`**: replaces `NULL` in all guard expressions.

Public C ABI and output bytes are preserved exactly (verified via `test_output`
and CLI smoke test against the Netflix golden YUV pair).

Also removes stale `test_ansnr_simd` meson.build entries that blocked
`meson setup` after the ansnr feature drop in ADR-0720.
