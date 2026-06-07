# ADR-1086: CI Workflow Least-Privilege Permissions Audit

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `ci`, `security`

## Context

A permissions audit of all 28 `.github/workflows/*.yml` files surfaced two
deviations from the least-privilege model the fork otherwise applies
consistently:

1. **`upstream-watcher.yml`**: The workflow-level block was set to
   `permissions: read-all`, which grants every available read scope to any
   future job added to the file. The single existing job overrides this to
   `{contents: read, issues: write}`, so the broad default was never actually
   exercised, but it left an overly wide footprint.

2. **`docker-publish-operator-node.yml`**: Two checkout steps (`build-operator`
   at line 62, `build-node` at line 152) omitted `persist-credentials: false`.
   Both jobs hold `packages: write` (push to GHCR). Without
   `persist-credentials: false`, `actions/checkout` leaves the `GITHUB_TOKEN`
   with `packages: write` scope stored in `.git/config` for the lifetime of the
   runner. Every other checkout in the repository already sets this flag. The
   inconsistency was a latent scope-escalation risk: a rogue step appended to
   either job could push to GHCR without needing an explicit `secrets.*`
   expression.

All other workflows were verified to have:

- Explicit top-level `permissions:` blocks (none missing).
- Per-job narrowing where write scopes are required.
- `persist-credentials: false` on checkout steps that co-exist with write scopes.
- `scorecard.yml`'s `permissions: read-all` is intentional and required by the
  `ossf/scorecard-action` to probe repository metadata; it is not changed.

## Decision

1. Change the `upstream-watcher.yml` workflow-level permission from
   `read-all` to `contents: read`. The existing job already opts into
   `issues: write` per-job; no job behaviour changes.

2. Add `persist-credentials: false` to the two checkout steps in
   `docker-publish-operator-node.yml` (`build-operator` and `build-node`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `read-all` on upstream-watcher | Zero risk since job overrides it | Violates least-privilege; a future job would silently inherit broad read access | Not chosen |
| Remove `persist-credentials: false` pattern globally | Simpler YAML | Leaves tokens with write scope in `.git/config` across step boundaries | Not chosen |

## Consequences

- **Positive**: `upstream-watcher.yml` now starts from a least-privilege
  baseline; any future job added to the file starts from `contents: read` rather
  than read-all.
- **Positive**: `docker-publish-operator-node.yml` jobs drop the GITHUB_TOKEN
  from `.git/config` immediately after checkout, consistent with every other
  checkout in the repository.
- **Neutral**: No behaviour change in any workflow run. The `ossf/scorecard`
  Scorecard TokenPermissionsID check will report an improvement on the next
  weekly scan.

## References

- [OSSF Scorecard TokenPermissionsID check](https://github.com/ossf/scorecard/blob/main/docs/checks.md#token-permissions)
- GitHub Actions documentation: `persist-credentials: false` in `actions/checkout`
- [ADR-0253](0253-ossf-scorecard-policy.md) — Scorecard policy for this fork
- Audit scope: `.github/workflows/*.yml` (28 files), 2026-06-06
