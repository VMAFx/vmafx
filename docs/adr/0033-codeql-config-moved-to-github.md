<!-- markdownlint-disable MD013 MD060 -->
# ADR-0033: Relocate CodeQL config to .github/

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: security, ci, github

## Context

`ROOT/codeql-config.yml` sat at the repo root but was orphaned — nothing referenced it, so it had no effect on the CodeQL scans. GitHub-specific configs conventionally live under `.github/`.

## Decision

Move `ROOT/codeql-config.yml` to `.github/codeql-config.yml`. Wire it into all three `codeql-action/init` steps in `.github/workflows/security.yml` via `config-file: ./.github/codeql-config.yml` so it actually takes effect.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep at root, wire in | Fewer moves | Wrong idiomatic location | Rejected per family rationale |
| Delete unused file | Removes dead config | Loses the intended ignore rules | Rejected |
| Move + wire (chosen) | Correct location + functional | Requires editing three workflow steps | Correct |

## Consequences

- **Positive**: CodeQL actually consumes the config; scan noise reduced.
- **Negative**: workflow diff touches three init steps.
- **Neutral / follow-ups**: security workflow verified in CI.

## References

- Source: `req` (user: "some project rood dirs should be cleaned up/moved as well")
- Related ADRs: ADR-0029, ADR-0037
