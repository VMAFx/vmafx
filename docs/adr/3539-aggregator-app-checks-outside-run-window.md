<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3539: Count non-Actions app check runs outside the aggregator run window

- **Status**: Accepted
- **Date**: 2026-11-12
- **Deciders**: Repository maintainers
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
cannot satisfy or mask the newly triggered jobs. Applied to external code
scanning without producer pinning or SARIF upload waiting, either per-SHA app
results are dropped on `ready_for_review` or foreign producers and older
code-scanning passes can mask real failures.

## Decision

In `newestByName` (`.github/workflows/required-aggregator.yml`), pin `CodeQL`,
`Semgrep OSS` and `gitleaks` via `expectedAppByCheck` to
`github-advanced-security` at any age on the head SHA while pinning every other
check name to `github-actions` under the `minCreatedMs` window (admitting check
runs missing `app.slug` under `minCreatedMs` only on `github-actions` names and
logging each ignored foreign-producer check name once). Generalise
`delayedRegistrationStillExpected` and keep polling while any SARIF upload job
mapped in `externalCheckUploadJobs` is active or completed within
`registrationPropagationMs`, whether or not an older external check run already
exists on the SHA, leaving `excludedSuites`, `required`, `pullRequestOnly`,
tier selection, polling bounds and
[ADR-3293](3293-aggregator-owed-contexts-must-succeed.md) evaluation unchanged.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
| --- | --- | --- | --- |
| Pin each check name to its expected app slug, scope `minCreatedMs` to `github-actions` (and missing slugs on `github-actions` names), and wait through `registrationPropagationMs` on SARIF upload jobs | Preserves stale-Actions filtering, blocks foreign-producer collisions, waits for fresh SARIF uploads, and keeps per-SHA code-scanning runs visible | Maintains `expectedAppByCheck` and `externalCheckUploadJobs` alongside `.github/workflows/security-scans.yml` | Chosen |
| Accept any non-Actions app slug at any age without producer pinning or upload-job wait | Avoids maintaining producer and upload-job maps | Unrelated GitHub Apps could satisfy or mask required names, and an older code-scanning pass could finish polling before a newly completed SARIF upload posts a failure | Rejected |
| Require a freshness window on external check runs or rely on `ready_for_review` workflow re-runs | Treats external check runs with a recency bound | Pull-request workflows already re-run on `ready_for_review`, yet `github-advanced-security` reports once per SHA when analysis is unchanged, re-breaking the pull request 3047 incident | Rejected |
| Drop the `minCreatedMs` window entirely | Treats every check run uniformly | Stale draft-era GitHub Actions check runs on an unchanged SHA could satisfy or mask newly triggered pull-request jobs before they register | Rejected |
| Exempt `CodeQL`, `Semgrep OSS` and `gitleaks` on `ready_for_review` events | Avoids failing those three contexts after a draft toggle | Opens an ADR-3293 hole where a pull request opened as draft and marked ready could merge without code scanning ever reporting on the SHA | Rejected |

## Consequences

- Converting a pull request to draft and marking it ready for review on an
  unchanged head SHA no longer fails `Required Checks Aggregator` when
  `CodeQL`, `Semgrep OSS` and `gitleaks` succeeded earlier on that SHA from
  `github-advanced-security`.
- Failed, skipped, neutral or cancelled check runs from
  `github-advanced-security` still block merges under
  [ADR-3293](3293-aggregator-owed-contexts-must-succeed.md), and absent check
  runs block on events where the context is owed (`pullRequestOnly` exempts
  `CodeQL`, `Semgrep OSS` and `gitleaks` outside pull requests).
- Check runs from unexpected apps are ignored and logged once per check name,
  while GitHub Actions check runs (`app.slug === 'github-actions'`) and check
  runs missing `app.slug` on `github-actions` names keep the `minCreatedMs`
  window (missing-slug runs on `github-advanced-security` names are ignored).
- While any SARIF upload job in `externalCheckUploadJobs` is active or
  completed within `registrationPropagationMs`, the aggregator keeps polling
  even when an older code-scanning `success` sits on the SHA, and a newer
  `in_progress` or failed check run from `github-advanced-security` supersedes
  an older pass.
- A producer posting later than the `registrationPropagationMs` propagation
  window after its upload job completes is not seen when an older check run for
  that name already exists on the SHA.
- `scripts/ci/tests/test_aggregator_event_scope.py` covers producer-pin,
  missing-slug, age-window and upload-propagation cases through
  `scripts/ci/required_aggregator_harness.py`.

## References

- [ADR-0106: Decision records](0106-adr-maintenance-rule.md).
- [ADR-3293: Every owed required context must succeed][adr-3293].
- [Research-3539](../research/3539-aggregator-app-checks-outside-run-window.md):
  incident timeline, producer and SARIF upload-job inventory, and alternatives.
- CI run `43085402627` on pull request 3047 at head
  `80595f3baaa80bdb423550eee237b1d6d279e746`, observed 2026-11-11.
- Source: `req` (2026-11-12, paraphrased): scope the `minCreatedMs`
  check-run age filter in `newestByName` to GitHub Actions check runs and runs
  without an app slug so check runs from other apps count at any age on an
  unchanged pull request head SHA.

[adr-3293]: 3293-aggregator-owed-contexts-must-succeed.md
