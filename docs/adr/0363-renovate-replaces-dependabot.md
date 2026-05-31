<!-- markdownlint-disable MD013 MD060 -->
# ADR-0363: Mend Renovate replaces Dependabot as the dependency-update bot

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: ci, security, dependencies, github-actions, pre-commit, fork-local

## Context

The fork maintains 23 SHA-pinned GitHub Actions across `.github/workflows/*.yml`
(required by the OSSF Scorecard `Pinned-Dependencies` check, per ADR-0263), 9
pre-commit hook revisions in `.pre-commit-config.yaml`, and Python dependencies
spread across four package directories (`python/`, `ai/`, `mcp-server/vmaf-mcp/`,
`dev-llm/`). Dependabot, which was configured in `.github/dependabot.yml`, covers
GitHub Actions, pip, and Docker — but it cannot update arbitrary shell-script
variables. In particular, `ffmpeg-patches/test/build-and-run.sh` line 27 pins
`FFMPEG_SHA` to an FFmpeg release tag (`n8.1.1`) via a shell default
(`: "${FFMPEG_SHA:=n8.1.1}"`), and `scripts/ci/ffmpeg-patches-check.sh` uses
a similar `FFMPEG_PATCHES_BRANCH` default. Dependabot has no regex/custom-manager
capability and cannot track these.

Mend Renovate's self-hosted GitHub Action mode provides:

- All Dependabot ecosystems (GitHub Actions, pip/pep621, Docker).
- `customManagers` with regex support for arbitrary file patterns — enabling
  FFmpeg tag tracking in shell scripts.
- Built-in pre-commit hook manager.
- Grouped update PRs (reducing PR noise relative to Dependabot's per-package
  approach for the 23 pinned actions).
- Scorecard-friendly SHA-pinned PR output (Renovate bumps the pinned SHA, not
  just the semver comment).

## Decision

We will adopt Mend Renovate self-hosted via a GitHub Actions workflow
(`.github/workflows/renovate.yml`) and disable Dependabot by renaming
`.github/dependabot.yml` to `.github/dependabot.yml.disabled`. Configuration
lives in `renovate.json` at the repository root. The `RENOVATE_TOKEN` secret
(a fine-grained PAT with `Contents: Read & Write`, `Pull requests: Read &
Write`, `Issues: Write`) must be set by the operator.

Renovate runs weekly on Monday before 06:00 Vienna time (cron `17 4 * * 1`),
matching the existing Dependabot Monday schedule. The `renovatebot/github-action`
action is SHA-pinned to `79dc0ba74dc3de28db0a7aeb1d0b95d5bf5fde2a` (v46.1.13)
per the fork's SHA-pin invariant (ADR-0263 / `.github/AGENTS.md`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep Dependabot, add a separate cron job for FFmpeg SHA | No new tool to adopt | Two separate systems; Dependabot still cannot group pinned-action bumps; FFmpeg tracking requires a custom script with no PR automation | Fragmented; more maintenance surface than Renovate alone |
| Renovate Cloud (hosted) | Zero self-hosting overhead | Requires granting a third-party OAuth app write access to the repo; Renovate Cloud reads `renovate.json` from the default branch, so misconfigured JSON becomes immediately live without a PR gate | Self-hosted via GitHub Action keeps the token within the repo's own secrets; config ships as a reviewable file |
| Keep Dependabot, patch it for FFmpeg | No new dependency | Dependabot's ecosystem list is closed; regex managers are not on the roadmap | Not feasible |

## Consequences

- **Positive**: single dependency-update bot; FFmpeg release tags are now tracked
  automatically; pre-commit hooks are updated alongside pip deps in a single
  grouped PR; SHA-pinned action bumps are grouped (reducing 23 individual
  Dependabot PRs to ~2 grouped Renovate PRs per cycle).
- **Negative**: requires one operator action to provision the `RENOVATE_TOKEN`
  secret before the first Monday cron fires; Renovate's `renovate.json` schema is
  richer than Dependabot's YAML and has a steeper learning curve.
- **Neutral / follow-ups**: `.github/dependabot.yml.disabled` is retained so the
  config is not lost if the operator ever wants to revert. Renovate's Dependency
  Dashboard issue (`":dependencyDashboard"` preset) will appear in the GitHub
  issue tracker after the first run; this is expected behaviour. The `AGENTS.md`
  SHA-pin sync gate in `.github/AGENTS.md` applies unchanged — Renovate PRs must
  still satisfy the 40-char SHA + semver-comment invariant, and Renovate's
  `github-actions` manager produces exactly that format.

## References

- Mend Renovate self-hosted docs: <https://docs.renovatebot.com/self-hosting/>
- `renovatebot/github-action` v46.1.13: SHA `79dc0ba74dc3de28db0a7aeb1d0b95d5bf5fde2a`
- ADR-0263: OSSF Scorecard policy (SHA-pin requirement)
- `.github/AGENTS.md`: SHA-pin invariant + resolution recipe
- Operator playbook: [`docs/development/dependency-bot.md`](../../docs/development/dependency-bot.md)
- req: "Add Mend Renovate (self-hosted via GitHub Actions) to VMAFx/vmafx as the dependency-update bot, replacing Dependabot."
