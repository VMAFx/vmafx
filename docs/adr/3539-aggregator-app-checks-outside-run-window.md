# ADR-3539: Count non-Actions app check runs outside the aggregator run window

- **Status**: Accepted
- **Date**: 2026-11-12
- **Tags**: ci

## Context

Under [ADR-3293](3293-aggregator-owed-contexts-must-succeed.md),
`Required Checks Aggregator` (`.github/workflows/required-aggregator.yml`)
requires every owed required context on the head SHA to report `success`.

On 2026-11-11, pull request 3047 at head
`80595f3baaa80bdb423550eee237b1d6d279e746` was converted to draft at
`2026-11-11T18:15:21Z` and marked `ready_for_review` at `19:14:50Z` with no
push between the two transitions. That `ready_for_review` event re-ran every
pull-request workflow on the unchanged SHA.

Aggregator run `43085402627` (created at `19:14:54Z`) failed with
`never reported on SHA 80595f3baaa80bdb423550eee237b1d6d279e746 (owed context, ADR-3293)`
for `CodeQL`, `Semgrep OSS` and `gitleaks`, although the earlier aggregator run
at `00:49Z` on the same SHA had passed. All three check runs existed on that
SHA from app `github-advanced-security` with `completed/success` (`CodeQL`
started `00:51:23Z`, `Semgrep OSS` `00:54:56Z`, `gitleaks` `00:51:07Z`). The
code-scanning app posts its check runs once per SHA and does not re-create them
when the same analysis is uploaded on a later event.

`newestByName` in `required-aggregator.yml` skipped every check run with
`runTimestampMs(run) < minCreatedMs` (`created_at` of the aggregator workflow
run minus two minutes). That window is appropriate for GitHub Actions check
runs, which every event re-creates on the SHA, so stale draft-era Actions runs
cannot satisfy or mask the newly triggered jobs. Applied to other GitHub Apps,
it drops check runs that are never re-posted for the unchanged commit.

## Decision

In `newestByName` (`.github/workflows/required-aggregator.yml`), apply the
`minCreatedMs` age window only to check runs produced by GitHub Actions
(`run.app && run.app.slug === 'github-actions'`) and to check runs missing
`app` or `app.slug` (unknown producer keeps the window, failing closed). Check
runs from any other GitHub App count at any age on the head SHA, and the newest
check run per name still wins. Keep the `excludedSuites` filter, `required`,
`pullRequestOnly`, tier selection, polling bounds and
[ADR-3293](3293-aggregator-owed-contexts-must-succeed.md) evaluation rules
unchanged.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
| --- | --- | --- | --- |
| Scope `minCreatedMs` to `github-actions` (and missing `app` slugs); accept other apps at any age | Preserves stale-Actions filtering and ADR-3293 fail-closed checks while keeping per-SHA app results visible | Inspects `run.app.slug` in `newestByName` | Chosen |
| Re-run the Security workflow on `ready_for_review` events | Keeps `newestByName` unchanged if new check runs were created | Pull-request workflows already re-run on `ready_for_review`, yet `github-advanced-security` reports once per SHA and does not post new check runs when an identical analysis is uploaded again | Rejected |
| Drop the `minCreatedMs` window entirely | Treats every check run uniformly | Stale draft-era GitHub Actions check runs on an unchanged SHA could satisfy or mask newly triggered pull-request jobs before they register | Rejected |
| Exempt `CodeQL`, `Semgrep OSS` and `gitleaks` on `ready_for_review` events | Avoids failing those three contexts after a draft toggle | Opens an ADR-3293 hole where a pull request opened as draft and marked ready could merge without code scanning ever reporting on the SHA, and hardcodes three names instead of modelling app check-run behaviour | Rejected |

## Consequences

- Converting a pull request to draft and marking it ready for review on an
  unchanged head SHA no longer fails `Required Checks Aggregator` when
  `CodeQL`, `Semgrep OSS` and `gitleaks` succeeded earlier on that SHA.
- Failed, skipped, neutral, cancelled or absent check runs from
  `github-advanced-security` or any other external app still block merges under
  [ADR-3293](3293-aggregator-owed-contexts-must-succeed.md).
- A newer `in_progress` or failed check run from the same external app still
  supersedes an older `success` check run on the same SHA, and the aggregator
  waits on active check runs as before.
- GitHub Actions check runs (`app.slug === 'github-actions'`) and check runs
  with no `app` slug keep the `minCreatedMs` window, failing closed when the
  producer is unknown.
- `scripts/ci/tests/test_aggregator_event_scope.py` covers positive, negative,
  window-kept and active-check boundary cases through
  `scripts/ci/required_aggregator_harness.py`.

## References

- [ADR-0106: Decision records](0106-adr-maintenance-rule.md).
- [ADR-3293: Every owed required context must succeed][adr-3293].
- CI run `43085402627` on pull request 3047 at head
  `80595f3baaa80bdb423550eee237b1d6d279e746`, observed 2026-11-11.
- Source: `req` (2026-11-12, paraphrased): scope the `minCreatedMs` check-run
  age filter in `newestByName` to GitHub Actions check runs and runs without an
  app slug so check runs from other apps count at any age on an unchanged pull
  request head SHA.

[adr-3293]: 3293-aggregator-owed-contexts-must-succeed.md
