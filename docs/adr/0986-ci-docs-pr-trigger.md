# ADR-0986: Add PR trigger to docs.yml CI workflow

- **Status**: Accepted
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: `ci`, `docs`, `fork-local`

## Context

The `docs` workflow (`.github/workflows/docs.yml`) previously triggered only on
`push` to `master`. This meant `mkdocs build --strict` ran for the first time
*after* a PR merged, turning doc-substance gaps (broken links, missing required
pages, malformed YAML front-matter) into post-merge failures. Approximately 38
of the last 50 master pushes failed the docs job for exactly this reason.

A pre-push hook exists but contributors can bypass it via `--no-verify` or by
working outside the clone that has hooks installed. The only reliable gate is
a required CI status check on the PR itself.

## Decision

Add an `on.pull_request` trigger to `docs.yml` with the same `paths` filter as
the existing `push` trigger so that `mkdocs build --strict` runs as a required
pre-merge check on every PR that touches `docs/**`, `mkdocs.yml`, or the
workflow file itself.

The `deploy` job and the `upload-pages-artifact` step are gated on
`github.event_name == 'push'` so they do not execute on PR builds. The
`pages: write` and `id-token: write` permissions are moved from the top-level
`permissions` block into the `deploy` job's own block so they are not granted
to the `build` job when running in a PR context (principle of least privilege).

The `concurrency` group key is parameterised by `${{ github.event_name }}-${{
github.ref }}` so that PR builds and master-push deploys do not cancel each
other.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Status-quo (push-only) | No change required | Gaps surface post-merge; ~38/50 failures | Rejected — root cause is the missing pre-merge gate |
| Separate `docs-check.yml` for PRs | Clean separation of concerns | Duplicate step definitions, two workflows to maintain | Rejected — single workflow is simpler and `if:` conditions are sufficient |
| `workflow_run` trigger from the existing workflow | No duplication | Complex; requires the PR to push a workflow artifact first; poor UX | Rejected — direct `pull_request` trigger is the idiomatic solution |

## Consequences

- **Positive**: `mkdocs build --strict` fails *before* merge, not after; doc
  gaps become a required pre-merge check.
- **Positive**: `pages: write` is no longer granted to the build job on PR
  runs, reducing the permission surface.
- **Positive**: PR and push concurrency groups are independent — a long-running
  PR build no longer cancels an in-flight master deploy.
- **Negative**: PRs that touch docs will consume one extra CI runner slot.
- **Neutral**: The `deploy` job is unchanged in behaviour for master pushes.

## References

- req: "docs.yml currently triggers only on push to master, causing ~38/50
  master-push failures because doc-substance gaps surface only after merge."
- Related: ADR-0100 (doc-substance rule), ADR-0108 (deep-dive deliverables).
