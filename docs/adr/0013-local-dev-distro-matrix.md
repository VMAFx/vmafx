<!-- markdownlint-disable MD013 MD060 -->
# ADR-0013: Support full local dev distro matrix

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: build, docs, framework

## Context

Contributors develop on heterogeneous machines. Portability bugs (musl vs glibc, pkg-config variants, clang vs gcc defaults) rarely surface on a single reference distro. The popup offered a narrow ("Ubuntu only") vs full matrix choice.

## Decision

We will document and test-cover local development on: Ubuntu 24.04 + Arch + Fedora 40 + Alpine 3.20 + macOS (brew) + Windows (winget/choco).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Ubuntu only | Minimal docs cost | Catches nothing in production diversity | User chose wider coverage |
| Linux-only matrix | Skips macOS/Windows edge cases | Leaves real contributor machines unsupported | Same |
| Full matrix (chosen) | Catches portability issues before prod | More work up-front | Rationale: user favors coverage over speed |

Rationale note: user chose the most comprehensive option; catches musl vs glibc, pkg-config, clang vs gcc defaults that only surface in production environments.

## Consequences

- **Positive**: setup scripts and docs cover every major platform a contributor might be on.
- **Negative**: per-distro setup scripts must be maintained; CI runtime impact (partially mitigated by ADR-0015).
- **Neutral / follow-ups**: `scripts/setup/` hosts per-distro bootstrap; CI matrix in ADR-0015.

## References

- Source: `Q4.1`
- Related ADRs: ADR-0015
