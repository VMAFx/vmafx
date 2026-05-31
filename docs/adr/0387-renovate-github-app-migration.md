# ADR-0387: Migrate Renovate from self-hosted workflow to GitHub App

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris
- **Tags**: `infra`, `dependency-bot`, `fork-local`

## Context

The fork ran Mend Renovate self-hosted as a GitHub Actions workflow
(`.github/workflows/renovate.yml`, scheduled cron). On 2026-05-10 the
listener session wedged twice (7-hour stall, lost session post-bounce),
causing user-visible symptoms: 13 dependency updates parked under
"Awaiting Schedule", 2 stuck under "Pending Status Checks", and a
duplicate dashboard issue (#603) the listener never reaped.

## Decision

Remove `.github/workflows/renovate.yml` and rely on the Mend-hosted
[Renovate GitHub App](https://github.com/apps/renovate) (free for public
repos, webhook-driven, hosted by Mend).

## Alternatives considered

1. **Keep self-hosted; harden the listener** — ARC controller restart
   loop, liveness probe, scheduled bounces. Ongoing infra maintenance
   for a side-quest. Rejected.
2. **Run both in parallel** — duplicate-PR risk from two bots competing
   on the same `renovate.json`. Rejected.
3. **Switch to Dependabot** — lacks Renovate's grouping + custom regex
   managers (FFmpeg pin tracker). Rejected.

## Consequences

**Positive:**

- No infra to maintain (no `RENOVATE_TOKEN`, no ARC runner-set, no cron).
- Webhook-driven response to dashboard ticks (vs polling).
- Free for public repos.

**Negative:**

- External SaaS dependency (Mend); rollback path documented in
  `docs/development/dependency-bot.md`.

## References

- ADR-0363 (self-hosted Renovate adoption — superseded by this ADR).
- Issue #749 (live dashboard).
- Issue #603 (closed duplicate, 2026-05-10).
- PR #750 (renovate.json config widened earlier the same session).
