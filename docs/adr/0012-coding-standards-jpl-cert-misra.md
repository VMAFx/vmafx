<!-- markdownlint-disable MD013 MD060 -->
# ADR-0012: Coding standards stack JPL + CERT + MISRA

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: lint, docs, license

## Context

A media-quality library with GPU and SIMD paths is memory-hazard dense. The initial plan listed only NASA/JPL Power of 10. The user explicitly expanded the stack ("we should also add jpl coding guidelines"), because Power of 10 is only Rule 1 of the broader JPL Institutional Coding Standard — the full 31 rules codify compiler strictness levels, banned functions, and verification requirements that Power of 10 alone does not.

## Decision

We will adopt the coding-standards stack: NASA/JPL Power of 10 + JPL Institutional Coding Standard for the C Programming Language (full 31 rules, applicable subset codified in `.clang-tidy`) + SEI CERT C & C++ + MISRA C:2012 (informative only).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Power of 10 only | Small, memorable | Misses banned-functions list, compiler strictness levels, verification guidance | Rejected — user explicitly expanded |
| MISRA C:2012 as mandatory | Comprehensive | Heavyweight; many rules impractical in video codec code | Kept as informative |
| Full JPL + CERT + MISRA informative (chosen) | Layered; mandatory core + advisory breadth | Larger rulebook to teach | Matches the user's explicit request |

Rationale note: JPL Institutional Coding Standard is the superset that Power of 10 is Rule 1 of — full 31 rules codify things Power of 10 alone doesn't.

## Consequences

- **Positive**: banned-functions enforcement (`gets`, `strcpy`, `sprintf`, `rand`, `system`); non-void return values checked.
- **Negative**: more `.clang-tidy` checks; contributor learning curve.
- **Neutral / follow-ups**: `docs/principles.md` codifies the full stack; `/lint-all` runs the enforceable subset.

## References

- Source: `req` (user: "we should also add jpl coding guidelines")
- Related ADRs: ADR-0005
