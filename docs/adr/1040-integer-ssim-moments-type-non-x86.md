<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1040: Promote `integer_ssim_moments_t` to shared header (macOS / Windows arm64 build fix)

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `build`, `simd`, `arm64`, `macos`, `windows`, `integer-ssim`, `fork-local`

## Context

`integer_ssim_moments_t` was defined only inside `#if ARCH_X86` in
`core/src/feature/x86/integer_ssim_avx2.h`. The type was used unconditionally
in `integer_ssim.c` for function-pointer typedefs and scalar wrappers (e.g.
`integer_ssim_score`). On macOS arm64 and Windows arm64 builds the x86 header
is not included, so the compiler saw 8 "unknown type name `integer_ssim_moments_t`"
errors and the build failed entirely.

The breakage was introduced when ADR-0784 added the AVX2 integer SSIM horizontal
moment accumulation path and the type was declared in the x86-only header rather
than a shared location.

## Decision

Promote the `integer_ssim_moments_t` typedef to a new shared header
`core/src/feature/integer_ssim.h` that is included unconditionally in
`integer_ssim.c`. The x86 header (`integer_ssim_avx2.h`) pulls from the shared
header rather than re-declaring the type.

This is the minimum-surface fix: no logic changes, no ABI change, no API change.

## Alternatives considered

- **Guard the type usage with `#if ARCH_X86`** — rejected; the scalar wrapper
  functions that use the type must be available on every platform so the CPU
  dispatch table is always populated correctly.
- **Move the entire integer SSIM implementation out of the x86 header** — out of
  scope for a build-fix PR; deferred to a future refactor.

## Consequences

macOS arm64 (`macos-15-arm64`) and Windows arm64 CI build cleanly. The fix is
transparent to any other platform — the shared header is included everywhere
`integer_ssim.c` is compiled, so no duplicate-symbol risk.

## References

- PR #654 (commit `695d29626`) — implementation
- T-MACOS-SIGSEGV-UNRESOLVED-2026-05-19 — bug tracker row in `docs/state.md`
- T-INTEGER-SSIM-MOMENTS-TYPE-NON-X86-2026-06-04 — bug tracker row
- [ADR-0784](0784-integer-ssim-avx2.md) — introduced the AVX2 integer SSIM path
  that placed the type in the x86-only header
