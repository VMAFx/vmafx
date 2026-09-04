<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1171: release-please credential gate warns on push, errors on dispatch

- **Status**: Accepted
- **Date**: 2026-09-04
- **Deciders**: Lusoris (maintainer), Claude Code session d0961a83
- **Tags**: `ci`, `release`, `release-please`, `supersedes-partial`

## Context

ADR-1151 made `release-please.yml` authenticate as the release-bot GitHub App and,
until the App and its two repository secrets exist, made the job **fail** on its
first step so it could never fall back to `GITHUB_TOKEN` and recreate a release PR
that receives no check runs. The App is a one-time maintainer action that has not
happened yet. The consequence on 2026-09-04: every push to `master` produces a red
`release-please` run. That red run is noise with three costs. It hides a real
failure of the same workflow (nobody looks at a workflow that is always red). It
makes epic #1246's gate "`master` fully green including nightly legs" unreachable
by construction. And it teaches every agent that reads `gh run list` to ignore
release-please failures. The safety property ADR-1151 wanted — no release PR, no
tag, no draft release is ever created with `GITHUB_TOKEN` — does not need a red
run to hold; it needs the write steps to be skipped.

## Decision

The credential check keeps its position as the first step and keeps refusing any
fallback, but its severity follows the trigger. On the `push` trigger a missing
`RELEASE_BOT_APP_ID` / `RELEASE_BOT_PRIVATE_KEY` emits a `::warning` annotation,
records `present=false`, and every later step (token mint, both `gh api` probes,
both release-please invocations) is skipped; a final step prints that the pipeline
is idle and points at `docs/development/release.md`. The job concludes **success**
with the warning visible on the run and on the commit. On `workflow_dispatch` an
operator explicitly asked for a release step, so the same condition stays an
`::error` and the job fails. A local preflight,
`scripts/release/check-release-bot-secrets.sh`, asserts through `gh secret list`
that both secret names exist and is part of the `/prep-release` dry run, so a
release cannot be attempted without the identity even though master stays green.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep ADR-1151's hard failure on every push | Loudest possible signal | Master is red on every push; real release-please failures are invisible; "master fully green" impossible until a human creates the App | Loudness that is always on carries no information |
| Fall back to `GITHUB_TOKEN` when the App is missing | Release PR keeps getting refreshed | Recreates exactly the un-runnable PR ADR-1151 diagnosed (zero check runs, BLOCKED behind admin bypass) | Rejected by ADR-1151 and still wrong |
| Gate the whole job with `if: secrets present` at job level | No step-level plumbing | Secrets are not readable in job-level `if`; a skipped job hides the reason entirely | Not expressible; loses the warning |
| Warning on push, error on dispatch, local preflight (chosen) | Master green, warning still on every run, operator path still fails loudly, release attempt still blocked locally | One more conditional per step | — |

## Consequences

- **Positive**: master's `release-please` run is green-with-warning until the App
  exists; the workflow's failures become meaningful again; the no-`GITHUB_TOKEN`
  guarantee is unchanged because every write step is skipped, not substituted.
- **Negative**: a maintainer who only looks at run colours can forget the App is
  missing; the warning annotation and the preflight script are the reminders.
- **Neutral / follow-ups**: `docs/development/release.md` "Release-bot identity"
  describes the new behaviour; `check-release-bot-secrets.sh` is wired into the
  `/prep-release` dry run; when the App lands nothing here needs to change.
  Partially supersedes ADR-1151 finding 1's "fails loudly on its first step" wording
  (the identity requirement itself stands).

## References

- ADR-1151 — release-bot identity and the first-release rollover.
- `.github/workflows/release-please.yml` — the `creds` step and the per-step guards.
- req — maintainer, 2026-09-04, on being told the red run was theirs to fix:
  paraphrased "do not stop and report, fix it".
