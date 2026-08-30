<!-- markdownlint-disable MD013 MD060 -->
# ADR-0001: Treat uncommitted benchmark result JSON as noise

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: workspace, git, testing

## Context

`testdata/netflix_benchmark_results.json` frequently appears dirty in the working tree after ad-hoc benchmark runs. Committing the file would inject noise into the history because the JSON changes on every run depending on hardware, backend, and timing. The fork needs a policy that keeps the file available as a snapshot artifact while preventing casual commits.

## Decision

We will treat any uncommitted `testdata/netflix_benchmark_results.json` as benchmark noise, not a commit. After any merge, the working-tree delta is stashed and dropped. Only formal, intentional regenerations update the tracked copy.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Commit every dirty result | Full history of runs | Creates meaningless churn; results depend on hardware | Noise drowns real changes |
| Add to `.gitignore` entirely | No risk of accidental commits | Loses the formal snapshot that golden-run updates rely on | The snapshot is a real artifact, only ad-hoc runs are noise |

This decision was a default — the alternative of committing noise was never seriously on the table.

## Consequences

- **Positive**: clean history; reviewers do not see benchmark JSON flapping on every PR.
- **Negative**: contributors must remember to stash before committing; easy to leak into a commit if not careful.
- **Neutral / follow-ups**: hard rule 5 in CLAUDE.md §12 documents the policy; `/run-netflix-bench` skill writes through this file for intentional updates.

## References

- Source: `Q1.1`
- Related ADRs: ADR-0009 (snapshot regeneration discipline)
