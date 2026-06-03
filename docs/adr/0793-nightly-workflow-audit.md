# ADR-0793: Nightly Workflow Audit — TSan, Artifact Retention, Python Version

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `ci`, `nightly`, `sanitizers`, `artifacts`, `fork-local`

## Context

A periodic audit of the five cron-scheduled workflows (`nightly.yml`,
`nightly-bisect.yml`, `sanitizers.yml`, `fuzz.yml`, and the upstream-watcher
family) identified three concrete defects and one structural redundancy:

1. **Duplicate TSan job** — `nightly.yml` ran a full ThreadSanitizer build and
   test suite on a daily cron. After ADR-0710 (CI Slim-Down v2), `sanitizers.yml`
   already fires a TSan job on every push to `master`, which is both more timely
   and higher signal (it catches the specific commit that introduced a race, not
   a 24-hour-old snapshot). The nightly cron TSan consumed ~45 minutes of runner
   time every night for zero incremental coverage.

2. **Missing artifact retention** — Both nightly artifacts (`clang-tidy-full-report`
   and `nightly-benchmark-results`) had no explicit `retention-days` setting,
   defaulting to GitHub's 90-day retention. Clang-tidy logs are diagnostic and
   lose value after a week; benchmark JSONs are comparable period-over-period but
   do not need to be kept for three months.

3. **Wrong Python version in `nightly-bisect.yml`** — The step was named
   "Set up Python 3.12" but pinned `python-version: "3.14.5"`. Python 3.14 is
   a pre-release alpha series that does not yet have a stable release; the
   `setup-python` action would attempt to download a non-existent version and
   fail the job. The step name was the authoritative intent; the version string
   was a copy-paste error from an unreleased future spec.

4. **Stale `libvmaf` source-dir reference in `fuzz.yml`** (already fixed in the
   current tree by PR fix/ci-paths-libvmaf-to-core-20260528): confirmed clean in
   the worktree; no change needed here.

## Decision

Apply the following targeted fixes to `nightly.yml` and `nightly-bisect.yml`:

- Remove the `tsan` job from `nightly.yml`. All TSan coverage is now provided by
  the master-push `sanitizers.yml` job (ADR-0710). A header comment explains
  the delegation.
- Add `retention-days: 14` to the `clang-tidy-full-report` artifact. Fourteen
  days is sufficient for a developer to investigate a finding before the log
  is garbage-collected.
- Add `retention-days: 30` to the `nightly-benchmark-results` artifact. Thirty
  days provides one month of period-over-period comparisons without the 90-day
  default accumulation.
- Fix `python-version: "3.14.5"` → `"3.12"` in `nightly-bisect.yml`. This
  matches the step name and uses the latest stable series the `ai/` dependencies
  have been validated against.

No changes are made to the upstream-watcher workflows, `scorecard.yml`,
`security-scans.yml`, or `fuzz.yml` — all of those are structurally sound.

## Nightly workflow inventory post-fix

| Workflow | Schedule | Purpose | Still needed? |
| --- | --- | --- | --- |
| `nightly.yml` — `clang-tidy-full` | 03:17 UTC daily | Full-tree clang-tidy (too slow for PRs) | Yes |
| `nightly.yml` — `netflix-benchmark` | 03:17 UTC daily | CPU benchmark throughput baseline | Yes |
| `nightly-bisect.yml` | 04:37 UTC daily | Bisect-model-quality smoke + sticky-issue update | Yes |
| `sanitizers.yml` — `fuzz-nightly` | 04:30 UTC daily | libFuzzer × 3 harnesses × 60 s | Yes |
| `sanitizers.yml` — `tsan` | master push | ThreadSanitizer (replaces nightly cron TSan) | Yes |
| `scorecard.yml` | Mon 04:19 UTC weekly | OSSF Scorecard supply-chain health | Yes |
| `security-scans.yml` | Mon 06:00 UTC weekly | CodeQL + Semgrep + Gitleaks | Yes |
| `upstream-watcher.yml` | Mon 08:00 UTC weekly | FFmpeg av1_videotoolbox probe | Yes (until encoder lands) |
| `upstream-netflix-955-watcher.yml` | Sun 06:00 UTC weekly | Netflix#1494 merge probe | Yes (until merged) |
| `upstream-netflix-645-hdr-model-watcher.yml` | Sun 06:15 UTC weekly | HDR model file probe | Yes (until landed) |
| `upstream-ffmpeg-hip-hwdec-watcher.yml` | Sun 06:30 UTC weekly | FFmpeg HIP hwdec probe | Yes (until landed) |

## Resource cost (post-fix, estimated per night)

| Job | Runner | Est. duration | Runner-minutes/night |
| --- | --- | --- | --- |
| clang-tidy-full | ubuntu-24.04 | ~45 min | 45 |
| netflix-benchmark | ubuntu-24.04 | ~20 min | 20 |
| nightly-bisect | ubuntu-24.04 | ~5–10 min | 10 |
| fuzz × 3 | ubuntu-latest × 3 | ~5 min each | 15 |
| **Total** | | | **~90 min/night** |

The removed TSan job was ~45 min/night → net saving ~45 runner-minutes/night
(~22 hours/month).

## Triage status

- `clang-tidy-full-report` artifacts: reviewed on demand; findings surface in
  the fork's lint gate when the touched file is changed in a PR. The nightly
  job exists to catch latent warnings in files not touched recently.
- `nightly-benchmark-results`: informal throughput tracking. No automatic alert
  on regression; operator reviews monthly or when a performance PR lands.
- `bisect-report`: sticky comment on issue #40 updated every night; any
  `WIRING BROKE` verdict makes the job red and visible in the Actions tab.
- Fuzz crash artifacts: uploaded for 30 days on non-zero exit; failures make
  the job red; no automated triage issue yet (tracked as a follow-up in
  docs/state.md T-SANITIZER-DEFECTS-REVEALED-758).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep nightly TSan, remove master-push TSan | Always-fresh nightly snapshot | 24-hour lag between commit and detection; duplicates coverage | master-push TSan is strictly better |
| Keep both TSan jobs | Belt-and-suspenders | ~90 extra runner-minutes/month for zero incremental signal | Redundant |
| Set fuzz retention to 7 days | Minimal storage | Crash artifacts may need re-download after a weekend; 30 days matches fuzz.yml precedent | 30 days is reasonable |

## Consequences

- **Positive**: ~45 runner-minutes/night saved; artifact storage reduced from 90
  to 14/30 days for nightly jobs; `nightly-bisect.yml` Python version now matches
  a real stable release.
- **Negative**: None. TSan coverage is fully preserved via `sanitizers.yml`.
- **Neutral**: Benchmark triage workflow unchanged; no alert automation added
  (out of scope for this ADR).

## References

- ADR-0710 (VMAFX CI Slim-Down v2 — introduced master-push TSan in sanitizers.yml)
- ADR-0109 (nightly-bisect scaffold)
- ADR-0270, ADR-0311 (libFuzzer nightly)
- ADR-0448 (upstream-watcher governance)
- `req` (paraphrased): user requested a nightly workflow audit covering necessity,
  brittleness, cost, triage habits, and artifact GC.
