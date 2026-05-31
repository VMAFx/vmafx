<!-- markdownlint-disable MD060 -->
# ADR-0133: Clang-Tidy push-event should scan push delta, not full tree

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: @lusoris, Claude
- **Tags**: ci, lint, clang-tidy

## Context

The `Clang-Tidy (Changed C/C++ Files)` job in
[`.github/workflows/lint-and-format.yml`](../../.github/workflows/lint-and-format.yml)
behaved asymmetrically by event type:

- On `pull_request`, it scanned only the diff vs master — the job name's
  literal promise.
- On `push` to master, it walked `git ls-files '*.c' '*.h' '*.cpp' '*.hpp'`
  over the **entire repository**, excluding only `subprojects/`.

The asymmetry was silent until the PR #70 squash-merge landed
(commit `45319133`). The push-event scan then processed every tracked
C/C++ translation unit, including:

- Vendored libsvm (`core/src/svm.cpp`) — a
  `clang-analyzer-unix.Malloc` finding at line 2984 on the
  `svm_load_model_buffer` code path (allocation of `support_vectors`
  that the analyzer can't prove gets transferred to `model->SV`).
- CUDA sources (`core/src/cuda/*.c`) compiled without CUDA headers
  because the workflow's `meson setup` uses `-Denable_cuda=false`. The
  resulting `clang-diagnostic-error` cascade ("unknown type name
  'VmafCudaState'", "'windows.h' file not found") was not the PR's
  regression — it's intrinsic to running tidy over CUDA TUs without
  the toolchain.

PR #70 passed clang-tidy because its delta touched only the MS-SSIM /
SSIMULACRA2 files. The push-event variant then fired on every file,
failing on long-latent warnings in files the push did not touch.
Observed: [run 24686615999, job
72197545195](https://github.com/VMAFx/vmafx/actions/runs/24686615999/job/72197545195).

## Decision

Unify push-event and pull-request-event semantics: both compute a
delta and lint only files inside it.

- **`pull_request`** (unchanged):
  `git diff --name-only origin/<base>...HEAD`.
- **`push`** (new): `git diff --name-only <github.event.before>..HEAD`.
  For the edge case of a new-branch push (`before == 0000...0000`),
  fall back to `HEAD~1..HEAD` when a parent exists, otherwise skip
  (no meaningful delta).
- **`workflow_dispatch` / other**: keep the full-tree scan — manual
  triggers are opt-in and documented in the workflow comment.

`actions/checkout@v6` is upgraded to `fetch-depth: 0` so the diff-base
SHA is reachable locally without a second `git fetch` step.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave full-tree scan, silence svm.cpp warnings via `// NOLINTNEXTLINE` or `.clang-tidy` `HeaderFilterRegex` | No workflow change; addresses the specific finding | Only fixes the currently-visible case. Next vendored-code push or clang-tidy upgrade re-exposes the same class of issue. Vendored libsvm is not ours to annotate | Treats symptom, not cause |
| Exclude vendored paths (`core/src/svm.cpp`, `core/src/compat/`, `core/src/cuda/`) via a negative-glob list | Pragmatic; keeps the "scan everything" posture | Exclude list drifts from reality; CUDA files *should* be linted under a CUDA-enabled job (future ADR), not permanently excluded | Would hide genuine issues in those paths on the PR that modifies them |
| Push-delta (this ADR) | Restores the semantic promised by the job name. Auto-scales: any file a PR touches still gets linted, on both PR and the post-merge push. No per-path exclude maintenance | Slight risk that a warning regresses through a file nobody touches for a while; acceptable because that file would have stayed silent under the full-tree scan too | Chosen |
| Drop the push-event trigger entirely | Simplest workflow YAML | Loses the defence-in-depth check that a PR merge didn't surface something new (e.g. another PR's rebase introduced a conflict). Push still catches this under delta semantics | Over-corrects |

## Consequences

- **Positive**: Post-merge pushes no longer fail on long-latent
  warnings in vendored or toolchain-gated code. Job name now matches
  behaviour. `fetch-depth: 0` costs a few seconds per run but makes
  the diff base unconditionally reachable.
- **Negative**: If a pre-existing warning exists in a file and nobody
  touches that file for months, the post-merge push won't surface it.
  Acceptable — the full-tree scan hid the same warnings for years
  under PRs and only surfaced them once a push triggered. The
  weekly nightly job (`nightly.yml`) remains the appropriate place
  for whole-repo scans.
- **Neutral / follow-ups**:
  - A follow-up ADR will decide whether to add a CUDA-enabled
    clang-tidy leg so `core/src/cuda/*.c` lints with its
    headers present. Not blocking.
  - The `svm.cpp:2984` analyzer finding remains latent; if a future
    PR modifies that file, the delta-scan will surface it and that
    PR will have to annotate or fix it.

## References

- Failing run: [actions/runs/24686615999/job/72197545195](https://github.com/VMAFx/vmafx/actions/runs/24686615999/job/72197545195)
- Workflow: [.github/workflows/lint-and-format.yml](../../.github/workflows/lint-and-format.yml)
- Source: `req` — "this must be something in the ci, the pr passed
  lol" (2026-04-20 popup directing CI-side fix over code-side).
