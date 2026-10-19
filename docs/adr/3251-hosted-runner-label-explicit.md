<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3251: Every hosted Linux job names its image by version; the floating label is refused

- **Status**: Accepted
- **Date**: 2026-10-11
- **Deciders**: lusoris
- **Tags**: ci, build

## Context

[ADR-0802](0802-ci-runner-image-standardization.md) (2026-05-29) pinned every non-Docker job to `ubuntu-24.04` and left the jobs of two Docker workflows on `ubuntu-latest`. The tree has moved away from that record without a new one. #876 moved the runners to Ubuntu 26.04 on 2026-06-12, #893 reverted the move the next day, and #1488 (`ba6fbf99f`, 2026-09-19) moved CI to `ubuntu-26.04` again. At the start of this change (`768cf5b0c`) the `runs-on` keys named `ubuntu-26.04` 93 times, `ubuntu-latest` 63 times in 19 workflow files, and `ubuntu-24.04` 5 times; one runner-pool expression fell back to `ubuntu-latest`. The accepted record still said `ubuntu-24.04`.

GitHub now moves the x64 `ubuntu-latest` label from Ubuntu 24.04 to 26.04, in a rollout that starts on 2026-10-19 and is planned to end on 2026-11-19 ([actions/runner-images#14748](https://github.com/actions/runner-images/issues/14748), read 2026-10-11). The announcement names a window, not a day. A job on the floating label changes its compiler, Python, glibc and kernel on some day inside that window, with no commit in this repository. A red check on that day has no diff to point at.

Nothing kept a floating label out. actionlint accepts every label GitHub knows, so `ubuntu-latest` passes it, and ADR-0802 had no check behind it: the tree left its image twice (#876, #1488) and carried the floating label in 63 jobs without a gate noticing.

One job is on the older image on purpose. `verify-native-artifacts` in `supply-chain.yml` runs on `ubuntu-24.04` because it proves that the native Linux bundle loads on the oldest hosted image that can load it ([ADR-1354](1354-native-bundle-release-track.md)). Moving it to the current image would remove what it tests.

## Decision

Every job that runs on a GitHub-hosted Linux runner names its image by version: `ubuntu-26.04`, or `ubuntu-26.04-arm` for the arm64 legs. No workflow names `ubuntu-latest`, one of its larger-runner forms or `ubuntu-slim`, which carries no version. The rule covers Docker and release jobs too; the exemption ADR-0802 gave the Docker jobs ends.

Another versioned image is allowed in two cases:

1. **Compatibility target.** The job's subject is the older image. The job is listed in `COMPATIBILITY_TARGETS` of `scripts/ci/check_hosted_runner_labels.py` with the record that says why. Today that is `verify-native-artifacts` on `ubuntu-24.04` (ADR-1354). It is not a debt and has no end date; it ends when its record changes.
2. **Dated fallback.** The job fails on the current image and needs time. It is declared in `.config/lint-exceptions.d/hosted-runner-label.toml` with the workflow file, the job, the label, the reason and an end date at most 90 days out. The file is part of the repository's declared exception list and follows its rules. No job needs one today, so the file does not exist yet.

`scripts/ci/check_hosted_runner_labels.py` holds the rule. It parses every file of `.github/workflows/` and reads every string, because a label is spelled in more places than `runs-on`: a matrix value, the fallback of an expression, the matrix a job prints as JSON for a later job, an input handed to a reusable workflow. YAML comments and the comment lines of a script are not read. It fails on a floating label, on an older image without one of the two declarations, on a fallback that has expired, names a floating label or no longer matches its job, and on a listed compatibility target that no longer names its image. It runs as the pre-commit hook `check-hosted-runner-labels` and as the third step of `make lint-actions`. `scripts/ci/tests/test_hosted_runner_labels.py` holds positive, negative and boundary cases for it.

Not changed: the macOS jobs still name `macos-latest`, the Windows jobs keep their versioned labels (`windows-2025`, `windows-11-vs2026-arm`), and the self-hosted pools and the `ARC_RUNNERS_ENABLED` expression keep their form. The check reads Ubuntu labels only.

This record supersedes ADR-0802.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Name `ubuntu-latest` and follow GitHub's move | No label edits, ever; always the image GitHub recommends | The image changes on a day GitHub picks, inside the rollout window; a failure has no commit to bisect | A check that goes red without a change in the repository cannot be told from a regression |
| Keep ADR-0802: `ubuntu-24.04` everywhere | Matches the accepted record | 93 jobs already run on 26.04, and the dev container and the CUDA and ROCm images are built on Ubuntu 26.04; going back splits the hosted jobs from them | The tree left this state on purpose; the record was stale, not the tree |
| Explicit `ubuntu-26.04`, by convention only | No new script | ADR-0802 was a convention and was left twice unnoticed; actionlint cannot see the difference | A rule without a check is the state this record ends |
| Explicit `ubuntu-26.04` with a check and two declared ways to differ (chosen) | The image moves only in a pull request, whose hosted run proves the jobs it starts; a deliberate older image and a temporary one are told apart; a temporary one ends | One more script and hook; the next image needs a label edit | Chosen |
| Hold the older-image job as a dated fallback too | One mechanism instead of two | The job would need a renewal every 90 days for a state that is intended; a renewal that only moves a date teaches nobody anything | A compatibility test is not a migration debt |

## Consequences

- **Positive**: the hosted Linux image changes in a pull request and nowhere else. The rollout that starts on 2026-10-19 changes nothing here.
- **Positive**: a floating label cannot come back through a matrix value, an expression fallback or a printed matrix; the check reads all of them.
- **Positive**: a fallback cannot outlive its reason. It fails the check when it expires and when the job stops naming the label.
- **Negative**: the next image is a deliberate change: `ALLOWED` in the script, the labels in `.github/workflows/` and the declaration in `.github/actionlint.yaml` move in one pull request.
- **Negative**: the shared exception list accepts one entry per rule and file, so one workflow file carries one fallback at a time. Two jobs of one file that both need the older image need the list to learn a second key first.
- **Neutral / follow-ups**: a pull request run does not start every job. Workflows that run only on a tag, a schedule, a push to `master` or a manual dispatch are proven by their next run, not by the pull request that edits their label.
- **Neutral / follow-ups**: nine install-step comments still describe the 24.04 image (compiler archives, package names) on jobs that run on 26.04. Whether each step is still needed there is not measured and is a follow-up.
- **Neutral / follow-ups**: `macos-latest` floats the same way. Naming the macOS image by version is a follow-up with its own measurement.

## References

- Source: fleet decisions of 2026-10-11, ledger ids `fleet-runner-label-20261019` and `vmafx-runner-label-light-checks` (paraphrased): before GitHub moves the floating label, replace it with the explicit 26.04 label in one pull request; a job that fails there goes back to the 24.04 label in the same pull request with a dated exception for one job, with a reason and an end within 90 days; the linter configuration declares the label; the record that supersedes ADR-0802 lands in the same pull request.
- [actions/runner-images#14748](https://github.com/actions/runner-images/issues/14748), "[Ubuntu] `ubuntu-latest` label will use Ubuntu 26.04 in November 2026", read 2026-10-11: rollout from 2026-10-19, planned to complete by 2026-11-19; the x64 label only.
- actions/runner-images `images/ubuntu/Ubuntu2604-Readme.md`, read 2026-10-11 (image 20260927.149.1, Ubuntu 26.04.1 LTS).
- [ADR-0802](0802-ci-runner-image-standardization.md) (superseded by this record), [ADR-1354](1354-native-bundle-release-track.md).
- #876, #893, #1488.
- User documentation: [CI runner pools](../development/ci-runners.md#hosted-linux-image), [Hosted runner labels](../development/pre-commit-hooks.md#hosted-runner-labels).

## Status update 2026-10-11: Accepted, three dated fallbacks

The first master run on `ubuntu-26.04` (head `c13628fdc`, the commit after this record's pull request #2739) and the next one (head `23fa449d1`) failed three jobs that passed on `768cf5b0c`, where the same jobs ran on the 24.04 image: `dev-container-build-work` (run 38153818052, job 114512668126: `No space left on device` during the image build), `ffmpeg-work` in its `FFmpeg Ubuntu gcc` row and `ffmpeg-sycl-work` (run 38153817985, jobs 114512707088 and 114512707033: the zero-warning gate stops on warnings the image's gcc prints in FFmpeg's own sources). The three jobs name `ubuntu-24.04` again, each with a dated fallback in `.config/lint-exceptions.d/hosted-runner-label.toml` that ends on 2026-10-25 (ledger id `vmafx-runner-2604-fallout`, 2026-10-11, operator decision relayed by the fleet controller; paraphrased: the two job families return to the 24.04 image now under a dated exception, a disk clean-up step and the FFmpeg warning fixes follow). Every other hosted Linux job stays on `ubuntu-26.04`.

Two of the three jobs are in one workflow file, the case the negative consequence above names as needing a second key. The shared list has it now: `PART_KEYS` in `scripts/ci/lint_exceptions.py` lets the rule `hosted-runner-label`, and no other rule, hold one entry per job of a file; the same file and job twice is still a duplicate (`scripts/ci/tests/test_lint_exceptions.py`, `scripts/ci/tests/test_hosted_runner_labels.py`). The decision itself is unchanged, so this is an update of this record and not a new one. The fixes are tracked as `T-CI-DEV-CONTAINER-DISK-UBUNTU-2604-2026-10-11` and `T-CI-FFMPEG-WARNINGS-UBUNTU-2604-2026-10-11` in `docs/state.md`.
