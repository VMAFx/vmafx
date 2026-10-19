---
paths:
  - .github/workflows/*.yml
  - .github/actionlint.yaml
  - scripts/ci/check_hosted_runner_labels.py
  - scripts/ci/tests/test_hosted_runner_labels.py
  - .config/lint-exceptions.d/hosted-runner-label.toml
invariant: Hosted Linux jobs name `ubuntu-26.04` / `ubuntu-26.04-arm`; no floating label; older image needs declaration.
---
# Hosted Linux runner labels

[ADR-3251](../../docs/adr/3251-hosted-runner-label-explicit.md), supersedes ADR-0802.

## Rule

- Hosted Linux job names `ubuntu-26.04` or `ubuntu-26.04-arm`. Docker + release
  jobs too.
- Refused: `ubuntu-latest`, larger-runner forms (`ubuntu-latest-4-cores`),
  `ubuntu-slim`.
- Reason: GitHub moves `ubuntu-latest` 24.04 -> 26.04, rollout 2026-10-19 to
  2026-11-19 (actions/runner-images#14748). Floating label = image change
  without commit.

## Check

`scripts/ci/check_hosted_runner_labels.py`: pre-commit hook
`check-hosted-runner-labels`, third step of `make lint-actions`. Reads every
string of every workflow, not only `runs-on`: matrix values, expression
fallbacks, printed JSON matrices, reusable-workflow inputs. YAML comments +
script comment lines not read.

## Older versioned image: two declarations only

| Case | Declared in | Ends |
| --- | --- | --- |
| Job tests older image itself | `COMPATIBILITY_TARGETS` in script, with its ADR | when ADR changes |
| Job fails on 26.04, needs time | `.config/lint-exceptions.d/hosted-runner-label.toml` | <= 90 days |

- Only compatibility target today: `verify-native-artifacts`
  (`supply-chain.yml`) on `ubuntu-24.04`, ADR-1354. Never move it to 26.04:
  check fails, test loses its subject.
- Fallback entry keys: `path`, `job`, `label`, `reason`, `expires`. One entry
  per job of workflow file; same file + job twice = duplicate (`PART_KEYS`
  in `scripts/ci/lint_exceptions.py`, this rule only).
- Fallbacks today, label `ubuntu-24.04`, end 2026-10-29: `ffmpeg-work` +
  `ffmpeg-sycl-work` (`ffmpeg-integration.yml`): FFmpeg source warnings with
  26.04 gcc, row `T-CI-FFMPEG-WARNINGS-UBUNTU-2604-2026-10-11`.
- Job fixed on 26.04: label back + entry out, same PR. Stale entry fails check.
- Remove fallback file with last entry. Do not keep empty file.
- Expired fallback, stale fallback, fallback naming floating label: each fails
  check.

## Next image

One PR moves `ALLOWED` in script, labels in `.github/workflows/`, declaration
in `.github/actionlint.yaml`. Hosted run of that PR = proof per started job.
Jobs not started by PR run (tag, schedule, push to `master`, dispatch): proven
by next own run; name them in PR body.

## Rebase

Conflict on `runs-on`: take explicit label. New upstream or branch job with
`ubuntu-latest`: change label in same PR. Not covered: `macos-latest`, Windows
labels, self-hosted pools.
