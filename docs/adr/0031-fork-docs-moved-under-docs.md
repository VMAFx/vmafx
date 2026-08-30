<!-- markdownlint-disable MD013 MD060 -->
# ADR-0031: Fork-added docs live under docs/

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: docs, workspace

## Context

Fork-added Markdown files had accumulated at the repo root (e.g. `ROOT/BENCHMARKS.md`). External consumers should see only the canonical top-level surfaces (README, LICENSE, CLAUDE.md, AGENTS.md, CONTRIBUTING.md, SECURITY.md, Makefile, meson.build, Dockerfile); everything else belongs in `docs/`.

## Decision

Move `ROOT/BENCHMARKS.md` to `docs/benchmarks.md`. Repo root keeps only the surfaces users see first. Linked from `README.md#Documentation` and `docs/index.md#Development`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep at root | Discoverable from tree view | Dilutes root with fork-only docs | Rejected per family rationale |
| Move under `docs/` (chosen) | Matches doc taxonomy | Readers find docs via index | Correct location |

Rationale: same as ADR-0029 cleanup family.

## Consequences

- **Positive**: root stays focused on external-user-facing files.
- **Negative**: anyone with a bookmark to `BENCHMARKS.md` must update.
- **Neutral / follow-ups**: README and doc index point to the new location.

## References

- Source: `req` (user: "some project rood dirs should be cleaned up/moved as well")
- Related ADRs: ADR-0029
