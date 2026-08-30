# ADR-0915: Enable clang-tidy `modernize-*` family with curated opt-outs

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `lint`, `ci`, `cpp`, `quality-gate`

## Context

Before this change `.clang-tidy` enabled only two checks from the
`modernize-*` family (`modernize-use-nullptr`, `modernize-use-override`).
The remaining ~25 checks in the family encode well-understood C++17/23
idioms (`<cstdio>` vs `<stdio.h>`, `nullptr` everywhere, `auto` for
explicit casts, range-based `for`, `emplace_back`, …). Several of these
correspond to SEI CERT C++ rules and to the C++23 migration cadence
captured in ADR-0725 (Wave 1 of the C → C++23 migration that landed
`log.cpp`, `dict.cpp`, and friends as pre-built reference
implementations).

Leaving the family disabled meant new fork-added `.cpp` translation
units could land with deprecated C headers, raw `NULL`, and missed
modernisation opportunities indefinitely. Enabling the family wholesale
without curation would have flagged ~25 cosmetic findings
(`modernize-use-trailing-return-type` on every function signature) and
~5 findings that fight the public C ABI (`modernize-avoid-c-arrays`
on `extern "C"` struct fields).

## Decision

Enable the full `modernize-*` family in `.clang-tidy`, with seven
explicit per-check disables for the high-noise / low-value subset
documented in *Alternatives considered* below. Discharge every
finding the new configuration emits on the CPU-built `.cpp`
translation units (`core/src/feature/feature_collector.cpp`,
`core/src/metadata_handler.cpp`) in the same PR. The seven disables
fall into two clusters: four C++-style preferences
(`-modernize-use-trailing-return-type`, `-modernize-use-auto`,
`-modernize-avoid-c-arrays`, `-modernize-use-nodiscard`) and three
checks that fire on legitimate C source when clang-tidy parses
`.c` files in the same lint run
(`-modernize-avoid-c-style-cast`, `-modernize-macro-to-enum`,
`-modernize-avoid-variadic-functions`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Enable `modernize-*` wholesale, no opt-outs | Most aggressive modernisation | ~25 cosmetic warnings on every function signature; ~5 false positives on public C ABI; 805 `avoid-c-style-cast` + 252 `macro-to-enum` on legitimate C files | Noise drowns real findings; touched-file rule (CLAUDE §12 r12) would force every PR to refactor unrelated signatures |
| Keep only the existing two-check whitelist | Zero new lint work | No coverage of `modernize-deprecated-headers`, `modernize-use-emplace`, `modernize-loop-convert`, `modernize-use-using`, … | Misses the most valuable members — the ones that catch real C-isms in newly-migrated `.cpp` files |
| Family minus seven explicit opt-outs (chosen) | Catches the high-value checks (nullptr, deprecated-headers, emplace, loop-convert, use-using, …) without drowning in noise | One-time touched-file sweep on existing `.cpp` files; small per-check disable list to keep current as new modernize checks land in clang-tidy | Picked: best signal-to-noise ratio. The seven disables split into four C++-style preferences and three C-source-noise mitigations |

## Consequences

- **Positive**: every new `.cpp` translation unit in `core/src/`
  automatically gets caught for raw `NULL`, deprecated `<stdio.h>`-style
  headers, missed `emplace_back`, classic `for` loops over containers,
  `#define`'d integral constants, and other recurring C-isms. The C →
  C++23 migration (ADR-0725) gains a permanent ratchet that prevents
  regressions back to C-style idioms in the migrated files.
- **Negative**: contributors who prefer trailing return types won't
  get a suggestion when they add a `.cpp` file that mixes leading and
  trailing styles. We treat that as a style choice rather than a
  correctness issue.
- **Neutral / follow-ups**: the 21 remaining
  `modernize-use-trailing-return-type` findings on
  `feature_collector.cpp` and `metadata_handler.cpp` are intentionally
  unaddressed — they are a separate stylistic decision (do we adopt
  trailing return types fork-wide?) that warrants its own ADR if and
  when the team decides to flip it.

## References

- Source: `req` (user direction — enable `modernize-*` checks and
  fix the top 15 findings on fork-added C/C++ code).
- [ADR-0725](0725-cpp23-pilot-log-v2.md) — C → C++23
  migration wave 1 (the migrated files this ADR ratchets against).
- [ADR-0278](0278-nolint-citation-closeout.md) — every NOLINT must
  cite an ADR; no NOLINTs are introduced by this sweep.
- [docs/principles.md §2](../principles.md) — coding-standards
  philosophy.
- Related research digest:
  `docs/research/clang-tidy-modernize-sweep-2026-05-31.md`.
