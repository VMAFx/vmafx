<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1040: Move `integer_ssim_moments_t` to a shared header for non-x86 portability

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `build`, `simd`, `portability`

## Context

`integer_ssim_moments_t` — the accumulation buffer struct for the integer SSIM
feature extractor — was introduced in commit `c80a3763b8` (ADR-1024, r6 scoring
guards). The typedef was placed in `core/src/feature/x86/integer_ssim_avx2.h`,
the AVX2-specific header. `integer_ssim.c` includes that header conditionally
under `#if ARCH_X86`, so the type was available on Linux/Windows x86-64 builds.

However, the function pointer typedefs (`ssim_accum_row_fn_8`,
`ssim_accum_row_fn_16`) and the scalar wrapper functions (`accum_row_scalar_8`,
`accum_row_scalar_16`) in `integer_ssim.c` all reference
`integer_ssim_moments_t` *unconditionally* — they are needed even on non-x86
platforms to provide the scalar fallback path. On macOS arm64 (Apple Silicon)
and Windows arm64, `ARCH_X86` is not defined, so the x86 header is never
included, and Clang reports "unknown type name" for all eight references to the
type in that translation unit.

## Decision

Move the `integer_ssim_moments_t` typedef into a new platform-neutral header
`core/src/feature/integer_ssim.h`. Include this header unconditionally in
`integer_ssim.c`. Update `x86/integer_ssim_avx2.h` to include the shared header
(via `../integer_ssim.h`) instead of re-defining the struct, so that the AVX2
header remains self-contained for callers that include it directly (e.g., the
SIMD parity test).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Guard the function pointer typedefs and scalar wrappers with `#if ARCH_X86` | Zero new files | The scalar path is needed on all platforms; gating it on x86 would break non-x86 builds at link time (unresolved `s->accum8` / `s->accum16` symbols) | Defeats the purpose — the scalar fallback must be unconditional |
| Define a local typedef in `integer_ssim.c` before the conditional include | Zero new files; single TU | Creates two definitions of the same type in a single TU when `ARCH_X86` is set, which violates the ODR-equivalent constraint for typedef re-declarations in C | Technically legal in C only if both definitions are identical, but fragile; a layout change in the x86 header would silently break x86 builds |
| Promote the struct into a public libvmaf header under `core/include/` | Visible to embedders | The struct is an internal accumulation buffer; exposing it in the public API adds maintenance burden and ABI obligations for no user-visible benefit | Overscoped |

## Consequences

- **Positive**: `integer_ssim.c` compiles cleanly on all supported platforms
  (Linux x86-64, macOS arm64, Windows arm64, any future riscv64 port).
  No functional change — the scalar and AVX2 paths are identical to pre-fix.
- **Negative**: One additional header file in the tree. Negligible.
- **Neutral / follow-ups**: Any future NEON or SVE2 `integer_ssim` accelerator
  should include `integer_ssim.h` from the `arm64/` subdirectory using
  `../integer_ssim.h`, mirroring the pattern set in `x86/integer_ssim_avx2.h`.

## References

- Introducing commit: `c80a3763b8` (ADR-1024, r6 per-metric scoring guards).
- ADR-0784: integer-ssim-avx2 design (layout invariant — six consecutive `int64_t` fields).
- Files changed: `core/src/feature/integer_ssim.h` (new),
  `core/src/feature/integer_ssim.c` (add include),
  `core/src/feature/x86/integer_ssim_avx2.h` (replace typedef with include).
