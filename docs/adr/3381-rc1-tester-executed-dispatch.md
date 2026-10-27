<!-- markdownlint-disable MD013 MD060 -->
# ADR-3381: The tester report states what executed (schema 4)

- **Status**: Accepted
- **Date**: 2026-10-27
- **Deciders**: Lusoris
- **Tags**: rc1-tester, tester, provenance, docs

## Context

The tester report derived the CPU dispatch level from `/proc/cpuinfo` flags
(`dispatch_flags`). That is the level the host could run, not the level the
library selected. A wrong flag table, or a library that ignores a level, goes
unnoticed (issue 2464). The library already writes its provenance record
(ADR-2073) into every JSON report, with the highest SIMD level in `simd`.

## Decision

Report schema `"4"` adds `executed_dispatch` and `library`. The default run
and the scalar-masked run of each dispatch fixture are read for the provenance
`simd` of the library; the host flags give only the expected level. A
default run whose level differs from the expected one, or a masked run that
is not `scalar`, is `mismatch` and fails the verdict (check
`executed_dispatch`). A library that reports nothing is `not_reported` and
also fails: no silent fallback to the host flags. `library` records the
library pair (`libvmafx.so.1`, `libvmaf.so.3` or `unknown`, read from the
binary's dynamic string table), the provenance digest and the conformance
mode. The library exposes no conformance mode and no per-extractor level yet,
so the mode is `not_reported`. Older reports stay valid. The host flag table
now matches the library's CPU rules (AVX2 needs FMA and BMI, `avx512icl` added).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep host flags only | No change | Cannot see what executed | The defect of issue 2464 |
| Parse CLI stderr for the level | No schema read | Second parser of an unstable text | The provenance record exists |
| Add a per-extractor level to the library | Exact per extractor | Library change, out of scope | Open question; the report field is ready for it |

## Consequences

- **Positive**: a dispatch bug shows in the tester report; both library pairs validate.
- **Negative**: reports from a library without provenance fail until it is rebuilt.
- **Neutral / follow-ups**: per-extractor level, library pair identity and conformance mode in the provenance record; Windows flag table lacks the same flags.

## References

- Issue 2464 (rc1-tester: report states what executed); ADR-2073; ADR-1492.
