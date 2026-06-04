<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1041: Fix CI RED — Go metal option type + Rust AVX-512 test guard

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `ci`, `build`, `go`, `rust`, `avx512`, `metal`

## Context

Two workflows were confirmed RED on master after the R8 unblock train:

**1. Go CI** (`go-ci.yml` line 55): `-Denable_metal=false` is passed to meson,
but `enable_metal` is a `feature`-type option (valid values: `enabled`,
`disabled`, `auto`). Meson rejects `false` as not one of the allowed string
choices, aborting the libvmaf build step that the Go CI depends on to generate
CGo bindings.

**2. Rust CI** (`rust-ci.yml`): builds with `-Denable_avx512=false`, so
`platform_specific_cpu_objects` in `core/src/meson.build` contains no AVX-512
object archives. However, `core/test/meson.build` unconditionally compiled
`test_motion_avx512_parity` on any x86/x86_64 host, linking it against
`platform_specific_cpu_objects`. With the AVX-512 objects absent, the linker
reported unresolved symbols for the AVX-512 kernel functions, causing the
Rust CI build to fail.

## Decision

1. Change `-Denable_metal=false` to `-Denable_metal=disabled` in `go-ci.yml`.
   `disabled` is the correct meson `feature` string that suppresses a backend.

2. Add `and is_avx512_enabled` to both the `executable()` definition guard and
   the `test()` registration guard for `test_motion_avx512_parity` in
   `core/test/meson.build`. `is_avx512_enabled` is defined in
   `core/src/meson.build` (line 69), which runs before `core/test/meson.build`
   via the `subdir('src')` then `subdir('test')` call order in `core/meson.build`.

## Alternatives considered

- **Runtime skip only**: The test already calls `simd_test_have_avx512()` at
  runtime to skip on hosts without the instruction set. This does not help
  for the linker failure at build time when the kernel object archives are absent.
- **Stub AVX-512 objects when disabled**: Would complicate the build
  significantly for no benefit; the cleaner fix is not to build the test.

## References

- R8 CI analysis: `r8-build-matrix-completeness` — go-ci metal flag,
  rust-ci AVX-512
- `core/meson_options.txt` line 21: `option('enable_avx512', ...)`
- `core/src/meson.build` line 69:
  `is_avx512_enabled = get_option('enable_avx512') == true`
- `core/src/meson.build` line 813: existing AVX-512 guard pattern
