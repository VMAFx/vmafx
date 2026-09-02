<!-- markdownlint-disable MD013 MD060 -->
# ADR-0015: CI matrix Linux/macOS/Windows with sanitizers

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, testing, security

## Context

A C library with GPU, SIMD, and threading needs aggressive CI coverage to catch memory and concurrency bugs before release. Sanitizers (ASan, UBSan, TSan) are the standard defense; the only question is frequency.

## Decision

We will run CI on Linux + macOS + Windows; ASan + UBSan on every PR; TSan on a nightly cron.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Linux-only PR gate | Cheap | Misses platform bugs | User chose full coverage |
| All sanitizers on every PR | Strongest gate | TSan is slow + flaky on parallel test runners | Nightly TSan is the pragmatic compromise |
| Full matrix + ASan/UBSan/PR + TSan/nightly (chosen) | Tight per-PR + deep nightly | ~5× PR runtime vs single-platform | Rationale: user favors coverage over speed |

Rationale note: PR runtime ~5× single-platform but catches OS-specific bugs.

## Consequences

- **Positive**: OS-specific bugs surface on PR; TSan coverage nightly.
- **Negative**: longer PR wall time; more CI minutes consumed.
- **Neutral / follow-ups**: ADR-0037 encodes which gates are required-status.

## References

- Source: `Q4.3`
- Related ADRs: ADR-0037
