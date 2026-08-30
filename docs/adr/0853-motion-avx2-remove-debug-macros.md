<!-- markdownlint-disable MD060 -->
# ADR-0853: Remove dead debug-print macros from motion_avx2.c

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `simd`, `lint`, `avx2`, `cleanup`, `fork-local`

## Context

`core/src/feature/x86/motion_avx2.c` contained 14 `#define` blocks
(`print_128_16`, `print_128_x`, `print_128_32`, `print_128_ps`,
`print_128_pd`, `print_128_32u`, `print_128_64`, and their `print_256_*`
counterparts, lines 155–240) that wrapped `printf` calls for inspecting
SSE/AVX register contents during development. These macros:

- Were never invoked anywhere in the translation unit or the broader codebase.
- Had no corresponding `#include <stdio.h>`, making them unusable without
  triggering an implicit-declaration warning.
- Were flagged by the AVX-512 SIMD audit as dead code.
- Violated ADR-0141 (touched-file lint-clean rule) for any PR that modifies
  this file.

The decision to delete rather than `#ifdef DEBUG`-guard them follows the
fork's principle that dead code is always a liability (readability, future
misuse risk, lint noise).

## Decision

We will delete lines 155–240 of `core/src/feature/x86/motion_avx2.c`
unconditionally. No replacement mechanism is needed; developers who need
SIMD register inspection during debugging can use a debugger watch or a
local temporary `printf` that never reaches a commit.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `#ifdef DEBUG` guard | Preserves macros for future debug use | Adds conditional compilation noise; dead code stays in tree | The macros have not been used since the file was authored; the cost of keeping them exceeds any conceivable future benefit |
| Keep as-is with a `// NOLINT` | Silences lint | Violates ADR-0141 (NOLINT requires a load-bearing invariant citation, which dead debug macros do not have) | Against project policy |

## Consequences

- **Positive**: 86 lines of dead code removed; file passes clang-tidy
  `readability-function-cognitive-complexity` and `bugprone-*` checks without
  suppression; no `#include <stdio.h>` pollution.
- **Negative**: Developers lose a ready-made AVX2 printf helper. Acceptable
  because the helper was already broken (missing stdio include) and would
  need reconstruction anyway.
- **Neutral / follow-ups**: None. The `motion_avx2.c` AVX-512 sibling
  (`motion_avx512.c`) should be audited for analogous dead macros in a
  follow-on pass.

## References

- ADR-0141: touched-file lint-clean rule.
- ADR-0278: NOLINT citation closeout (establishes that NOLINT without a
  load-bearing invariant is itself a violation).
- AVX-512 audit (2026-05-29) that flagged these macros.
- req: "Remove 28 dead `print_128_*` / `print_256_*` debug macros from
  `core/src/feature/x86/motion_avx2.c` lines 155-240 (flagged by AVX-512
  audit; violates ADR-0141 touched-file lint-clean rule)."
