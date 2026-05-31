<!-- markdownlint-disable MD013 MD060 -->
# ADR-0032: Relocate root unittest script to scripts/

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: testing, workspace

## Context

`ROOT/unittest` was a bare shell script at the repo root, exactly the kind of artefact cluttering external discovery. `scripts/` already hosts `ci/`, `setup/`, and `test-matrix.sh`, so consolidating shell entry points there is natural.

## Decision

Move `ROOT/unittest` to `scripts/run_unittests.sh`. Keep the name discoverable next to `test-matrix.sh` rather than renaming further. Update `docs/usage/python.md` accordingly.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep at root | No churn | Root clutter | Rejected per family rationale |
| Move to `scripts/run_unittests.sh` (chosen) | Consolidates shell entry points | Path update in docs | Correct |
| Rename to `test-python-unit.sh` | More descriptive | Breaks discoverability from `test-matrix.sh` | Rejected |

## Consequences

- **Positive**: all ad-hoc shell entry points in one place.
- **Negative**: contributors with muscle memory for `./unittest` must relearn.
- **Neutral / follow-ups**: doc update to `docs/usage/python.md` already done.

## References

- Source: `req` (user: "some project rood dirs should be cleaned up/moved as well")
- Related ADRs: ADR-0029
