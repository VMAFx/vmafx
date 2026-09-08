# Research-2051: CAMBI AVX2 parameter shadowing

Exhaustive Cppcheck at `2143174c7bf933f97aec610f423c559a885b8e9a` reports five
`reciprocal_lut` arguments shadowing the global LUT supplied by `cambi.h`.
Rename those local parameter bindings to `reciprocals`; the three outer call
sites still pass the original global LUT. This does not alter a function type,
callback, gather width, lane ordering, arithmetic expression or backend.
The existing ADR-1138 C `NULL` bracket is applied without moving assertion
line numbers; no new diagnostic exemption is introduced.

## Alternatives considered

| Choice | Result |
| --- | --- |
| Rename the five local parameter bindings | Selected: smallest correction; preserves every expression and caller. |
| Rename the shared global or change header/API types | Rejected: unnecessary shared scope for a local shadowing diagnostic. |
| Suppress `shadowVariable` | Rejected: no load-bearing reason prevents the local rename. |

## Validation and limits

Only eleven identifier tokens change among 4,941 source tokens, restricted to
those five function scopes. All global LUT references and every other token
match after excluding comments/whitespace. Headers, test sources and Meson
registration remain byte-identical.

The existing `test_cambi` and `test_cambi_simd` pass before and after in a private
GCC 15.2 CPU release build with assembly and default LTO. All executable and
read-only constant sections match byte-for-byte in both real test executables
(309,135 and 240,327 bytes respectively). A materialized native object compiled
with the same command except `-fno-lto` also matches across all code/constant
sections (34,546 bytes). This object comparison is separate from the actual
release/LTO test run; LTO intermediate bytes are retained, not claimed identical.

Strict clang-tidy 22.1.8 measures zero warnings, compiler failures and uncited
annotations. The guarded scoped writer runs and leaves the existing zero debt
baseline byte-identical. Exhaustive Cppcheck with the actual CAMBI caller removes
all five shadowing findings and reports no owned-file finding. Its reduced
project exits one on an unused inline helper in untouched `feature_collector.h`;
this is not a full-tree lint pass.

Reproduce with `meson test -C build --print-errorlogs test_cambi test_cambi_simd`.
Exact commands, original source, native objects/binaries, section hashes and
normal-hook receipts are retained locally in
`.workingdir2/evidence/cambi-avx2-native-lint-2026-09-08/`.
No new test or golden assertion changes. No complete RC1, Windows/GPU or
sanitizer acceptance is claimed. No user-guide update is needed for local
parameter names with identical generated behavior. Existing x86 CAMBI
invariants cover this change; no new rebase-sensitive invariant is introduced.

## References

- [SIMD invariants](../../core/src/feature/x86/AGENTS.md)
- [CAMBI feature invariants](../../core/src/feature/AGENTS.md)
- req: "get the codebase clean" while preserving numerical behavior.
