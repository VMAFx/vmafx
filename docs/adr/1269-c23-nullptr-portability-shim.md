# ADR-1269: Use one portable null-pointer token in C translation units

- **Status**: Accepted
- **Date**: 2026-09-20
- **Deciders**: Lusoris
- **Tags**: c23, portability, msvc, lint, ci, code-quality

## Context

ADR-1267 requires the whole configured tree to reach zero without disabled
diagnostic categories or analyzer suppressions. The configured lint drivers
still appended `--checks=-modernize-use-nullptr` for every `.c` file, while 198
C/C++ test files and many production files carried file-wide
`NOLINTBEGIN(modernize-use-nullptr)` brackets. That double exclusion hid 596
raw `NULL` findings in the current inventory.

C23 standardises `nullptr`, and the required GCC, Clang, Intel LLVM, CUDA and
HIP frontends accept it in their C23/C2x modes. MSVC's `/std:clatest` remains a
required build lane but Microsoft does not document C `nullptr` support. Using
the keyword directly everywhere would therefore replace lint debt with an
unverified Windows build break; retaining the exclusions would violate the
maintainer's explicit zero-warning decision.

## Decision

Add an internal `VMAF_NULLPTR` token. It expands to the C23 `nullptr` keyword
when the active C frontend advertises the final C23 standard or a known C2x
implementation with `nullptr`; it expands to integer null-pointer constant `0`
on older C frontends, including MSVC C. C++ continues to spell `nullptr`
directly.

Replace raw `NULL` tokens in first-party and shipped vendored C translation
units with `VMAF_NULLPTR`, remove all corresponding `NOLINT` brackets, and
remove the per-C `modernize-use-nullptr` disable from both configured lint
drivers. Compile fixtures exercise the C23 and fallback branches. This
supersedes ADR-1138's suppression-based decision.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep raw `NULL` plus per-file suppressions | No source-token churn; known MSVC behavior | Hides hundreds of configured findings and requires a disabled C diagnostic category | Rejected by ADR-1267 and the maintainer's explicit direction |
| Use bare C23 `nullptr` in every C file | Standard token; no abstraction | Current required MSVC C support is undocumented and cannot be proven on the Linux development host | Rejected until the Windows lane itself proves native support |
| Drop or replace the MSVC lane | Makes native C23 support selectable | Loses an independently required compiler/ABI lane and expands this cleanup into a toolchain migration | Disproportionate and outside the warning fix |
| Use `VMAF_NULLPTR` with a tested fallback | Zero diagnostics without weakening a gate; preserves required compilers | Adds one project token and mechanical source churn | Chosen |

## Consequences

- **Positive**: C and C++ are both measured by the same enabled
  `modernize-use-nullptr` profile with no `NOLINT` or origin exception.
- **Positive**: required MSVC builds retain a standards-valid integer null
  pointer constant until native C23 `nullptr` is demonstrably available.
- **Negative**: upstream and re-vendored C sources require a mechanical
  `NULL` to `VMAF_NULLPTR` reconciliation when synced.
- **Neutral / follow-ups**: once the required MSVC lane proves native C
  `nullptr`, a later ADR may retire the compatibility token in favor of the
  keyword. Until then, its compile fixture is part of the portability gate.

## References

- Source (`req`, verbatim, 2026-09-20): "there is no on touch rule anymore, no
  fucking warning or error is just ignored because of being og netflix code,
  fix them all ffs".
- [ADR-1267](1267-whole-tree-zero-debt-completion.md) — zero is the completion
  state; origin and permanent baselines are not exceptions.
- [ADR-1138](1138-c-translation-units-keep-null.md) — superseded
  suppression-based decision.
- [Research-2069](../research/2069-c23-nullptr-zero-warning-portability.md) —
  compiler evidence and migration contract.
- [Microsoft `/std` option reference](https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version)
  — `/std:clatest` enables implemented draft-C features but does not enumerate
  C `nullptr` support.
