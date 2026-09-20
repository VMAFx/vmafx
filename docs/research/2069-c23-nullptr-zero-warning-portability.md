# Research-2069: C23 null-pointer zero-warning portability

## Question

How can the whole-tree cleanup enable `modernize-use-nullptr` for C without
discarding the required MSVC C lane or retaining analyzer suppressions?

## Evidence

- The 2026-09-20 inventory contains 596 `modernize-use-nullptr` suppression
  entries. `scripts/ci/tidy-ratchet.py` and
  `scripts/ci/lint-configured.py` additionally disable the check for every
  `.c` input, so the file markers and driver both hide the same class.
- The current GCC 16 and Clang 22 host frontends compile
  `int *p = nullptr;` under `-std=c23` and advertise
  `__STDC_VERSION__ == 202311L`.
- The prior cross-toolchain audit in ADR-1138 established GCC 13 C2x and Clang
  16+ support, but found no documented MSVC C `nullptr` contract. Microsoft's
  current `/std` documentation still describes `/std:clatest` generically as
  implemented draft-C features rather than promising that keyword.
- Converting warnings to `NOLINT`, disabling the check for C, or treating the
  Netflix/vendored spelling as immutable all contradict ADR-1267.

## Result

Use `VMAF_NULLPTR` from `core/src/vmaf_nullptr.h`. Its native branch expands to
`nullptr`; its compatibility branch expands to `0`, which remains a valid C
null pointer constant. Only C sources use the compatibility token; C++ keeps
the language keyword. A compile fixture must force both branches so a future
toolchain change cannot silently rot the fallback.

The configured drivers will run `modernize-use-nullptr` unchanged on C and C++.
The migration is complete only when the whole-tree token scan contains no
`NOLINT` and configured clang-tidy reports zero, not when the prior baseline is
merely reduced.

## References

- [ADR-1267](../adr/1267-whole-tree-zero-debt-completion.md)
- [ADR-1269](../adr/1269-c23-nullptr-portability-shim.md)
- [Microsoft `/std` option reference](https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version)
- req: "no warning or error is just ignored because of being og netflix code"
