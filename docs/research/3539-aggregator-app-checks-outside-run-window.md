<!-- markdownlint-disable MD013 MD060 -->
# Research-3539: Producer pinning and upload-job wait for external checks in Required Checks Aggregator

- **Status**: Completed
- **Workstream**: [ADR-3539](../adr/3539-aggregator-app-checks-outside-run-window.md), [ADR-3293](../adr/3293-aggregator-owed-contexts-must-succeed.md)
- **Last updated**: 2026-11-12

## Question

How can `Required Checks Aggregator` (`.github/workflows/required-aggregator.yml`) accept per-SHA code-scanning check runs that GitHub Advanced Security created before a `ready_for_review` re-run on an unchanged commit, without letting unrelated GitHub Apps satisfy or mask required contexts or letting an older code-scanning pass finish polling before a newly completed SARIF upload reports a failure?

## Sources

- CI run `43085402627` on pull request 3047 at head `80595f3baaa80bdb423550eee237b1d6d279e746` (observed 2026-11-11): `ready_for_review` re-run after a draft toggle with no intervening commit.
- GitHub REST API check-runs listing (`GET /repos/{owner}/{repo}/commits/{ref}/check-runs`) measured on pull request head `76357c67f` (2026-11-12): producer slugs across required and non-required check runs.
- `.github/workflows/security-scans.yml` in this repository: job definitions and SARIF upload steps for `Semgrep`, `CodeQL (C/C++)`, `CodeQL (Python)`, `CodeQL (Actions)` and `Gitleaks`.
- [`github/codeql-action` `analyze` action definition](https://docs.github.com/en/code-security/code-scanning/integrating-with-code-scanning/configuring-code-scanning-in-your-ci-system): `github/codeql-action/analyze` defaults to `upload: true` (`always`), uploading SARIF to GitHub code scanning unless explicitly disabled, whereas external scanners upload through `github/codeql-action/upload-sarif`.
- `.github/workflows/required-aggregator.yml`, `scripts/ci/required_aggregator_harness.py` and `scripts/ci/tests/test_aggregator_event_scope.py` in this repository.

## Findings

1. **Incident on an unchanged pull-request head.** On 2026-11-11, pull request 3047 at commit `80595f3baaa80bdb423550eee237b1d6d279e746` was converted to draft at `2026-11-11T18:15:21Z` and marked `ready_for_review` at `19:14:50Z` with no push between the two events. Every pull-request workflow re-ran on `ready_for_review`, creating aggregator run `43085402627` at `19:14:54Z` (`minCreatedMs` = `19:12:54Z`). `github-advanced-security` had already posted `completed/success` check runs on that SHA (`gitleaks` at `00:51:07Z`, `CodeQL` at `00:51:23Z`, `Semgrep OSS` at `00:54:56Z`) and does not create new check runs when an identical SARIF analysis is uploaded again for the same SHA. Because `newestByName` dropped every check run older than `minCreatedMs`, all three owed contexts disappeared from `byName` and failed with `never reported on SHA 80595f3baaa80bdb423550eee237b1d6d279e746 (owed context, ADR-3293)`.

2. **Check-run producers and SARIF upload jobs.** Querying check runs on pull request head `76357c67f` and inspecting `.github/workflows/security-scans.yml` establishes the full set of non-Actions check-run producers and the GitHub Actions jobs that upload SARIF to code scanning on pull requests:

   | External app slug | Check-run name(s) | In `required` (`pullRequestOnly`) | Uploading Actions job(s) in `security-scans.yml` | Upload step |
   | --- | --- | --- | --- | --- |
   | `github-advanced-security` | `CodeQL` | Yes (Yes) | `CodeQL (C/C++)`, `CodeQL (Python)`, `CodeQL (Actions)` | `github/codeql-action/analyze` (default `upload: always`, `security-events: write`) |
   | `github-advanced-security` | `Semgrep OSS` | Yes (Yes) | `Semgrep` | `github/codeql-action/upload-sarif` (`category: semgrep-local`, `security-events: write`) |
   | `github-advanced-security` | `gitleaks` | Yes (Yes) | `Gitleaks` | `github/codeql-action/upload-sarif` (`category: gitleaks`, `security-events: write`) |
   | `codacy-production` | Non-required analysis check | No | None | External service webhook |
   | `coderabbitai` | Non-required review check | No | None | External service webhook |
   | `sourcery-ai` | Non-required review check | No | None | External service webhook |

3. **Producer pinning blocks cross-app spoofing and name collisions.** Exempting every non-`github-actions` check run from `minCreatedMs` without checking which app owns which name leaves two gaps: any installed GitHub App could post a check run under a required name (`CodeQL` or a `github-actions` gate name) and satisfy it, or a newer foreign check run could overwrite a real `github-actions` failure in `newestByName`. Pinning `CodeQL`, `Semgrep OSS` and `gitleaks` to `github-advanced-security` in `expectedAppByCheck` and pinning every other name to `github-actions` ensures each check run counts only from its expected producer. Check runs missing `app.slug` remain admitted inside `minCreatedMs` only for `github-actions` names (preserving fail-closed age filtering) and are rejected on `github-advanced-security` names. Collecting ignored foreign-producer names in a `Set` and logging once after polling keeps log output bounded.

4. **Upload-job propagation wait closes the stale-pass race.** When `security-scans.yml` re-runs on a commit that already carries an older `github-advanced-security` `success` check run, `byName` is populated on the first poll even while `CodeQL (C/C++)`, `CodeQL (Python)`, `CodeQL (Actions)`, `Semgrep` or `Gitleaks` is still running or has just uploaded a new SARIF file. Generalising `delayedRegistrationStillExpected(dependencies, name, nowMs)` and polling while any owed external check has an upload job in `externalCheckUploadJobs` that is active or completed within `registrationPropagationMs` (two minutes) holds the aggregator across the upload-to-check-run handoff without duplicating the dependency loop.

## Alternatives explored

- **Drop the `minCreatedMs` window entirely**: Rejected. Stale draft-era `github-actions` check runs on an unchanged SHA would remain eligible and could satisfy or mask newly triggered pull-request jobs before they register.
- **Require a freshness window on external check runs**: Rejected. `github-advanced-security` posts per-SHA check runs once when analysis results are unchanged, so any recency cutoff re-introduces the pull request 3047 failure on `ready_for_review`.
- **Accept any non-`github-actions` app slug without producer pin or upload wait**: Rejected. Permits unrelated apps to collide with required check names and lets an older code-scanning `success` exit polling during the propagation gap right after a SARIF upload job completes.
- **Producer pin (`expectedAppByCheck`) plus SARIF upload-job wait (`externalCheckUploadJobs`)**: Chosen.

## Open questions

- If `github-advanced-security` posts a new check run more than `registrationPropagationMs` (two minutes) after its SARIF upload job completes and an older check run for that name already exists on the SHA, the aggregator will have exited on the older run. Raising the propagation window would add two-plus minutes to every clean draft-to-ready re-run where code scanning intentionally posts nothing new.

## Related

- [ADR-3539](../adr/3539-aggregator-app-checks-outside-run-window.md) — Count non-Actions app check runs outside the aggregator run window
- [ADR-3293](../adr/3293-aggregator-owed-contexts-must-succeed.md) — Every owed required context must succeed
