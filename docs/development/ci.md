# CI overview

This page documents the fork's CI surface for contributors. The
authoritative trigger / gate behaviour lives in the workflow files
under [`.github/workflows/`](../../.github/workflows/); this doc
explains the rules a contributor needs to know without reading every
file.

## Workflows

The fork ships nine `pull_request`-triggered workflows:

| File | Purpose |
| --- | --- |
| [`docker-image.yml`](../../.github/workflows/docker-image.yml) | Docker image build (advisory). |
| [`security-scans.yml`](../../.github/workflows/security-scans.yml) | Semgrep / CodeQL / Gitleaks / Dependency Review. |
| [`lint-and-format.yml`](../../.github/workflows/lint-and-format.yml) | Pre-commit, clang-tidy (changed files + whole-tree ratchet, ADR-1142), cppcheck, mypy, registry validate, twin-drift gate (ADR-1135). |
| [`required-aggregator.yml`](../../.github/workflows/required-aggregator.yml) | Single required-check aggregator (ADR-0313). |
| [`ffmpeg-integration.yml`](../../.github/workflows/ffmpeg-integration.yml) | FFmpeg + libvmaf build (gcc / clang / SYCL / Vulkan). |
| [`libvmaf-build-matrix.yml`](../../.github/workflows/libvmaf-build-matrix.yml) | Cross-platform / cross-backend libvmaf build matrix. |
| [`rule-enforcement.yml`](../../.github/workflows/rule-enforcement.yml) | ADR-0100 / 0106 / 0108 / 0165 process gates. |
| [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml) | Netflix golden, sanitizers, tiny-AI, MCP, coverage, assertion-density. |
| [`sycl-parity.yml`](../../.github/workflows/sycl-parity.yml) | SYCL parity tests on self-hosted Intel Arc A380 runner (ADR-1177; see [runbook](ci-self-hosted-sycl.md)). |

For the complete inventory, mapping of shortened names, and conventions,
see [CI job display names](ci-job-names.md).

## Draft pull requests do not trigger CI

Per [ADR-0331](../adr/0331-skip-ci-on-draft-prs.md), every
`pull_request`-triggered workflow above is gated to skip when the PR
is in `draft` state. Concretely:

- Each workflow's `pull_request:` block lists
  `types: [opened, synchronize, reopened, ready_for_review]`.
- Each top-level job carries an `if:` clause of the form
  `github.event_name != 'pull_request' || github.event.pull_request.draft == false`.

What this means for contributors:

1. **A draft PR shows no green checks.** The required-checks
   aggregator skips on drafts and branch protection treats the
   missing aggregator as "required check absent". This is benign —
   GitHub blocks merging a draft PR by definition, so the gate cannot
   be bypassed.
2. **Promoting the draft to ready-for-review fires CI exactly once.**
   GitHub's `ready_for_review` event is what re-triggers the
   workflows; subsequent `synchronize` events on the now-ready PR
   fire CI as before.
3. **Pushing to `master` is unaffected.** The job-level `if:` clause
   short-circuits to `true` when there is no PR object (for example
   on `push:` events).

To preview CI status before merging, mark the PR ready-for-review.
You can flip back to draft afterwards if more work is needed; the next
`ready_for_review` will fire a fresh matrix.

## CI impact routing (ADR-1140)

Required checks no longer decide *whether they apply* from a workflow-level
`paths:` / `paths-ignore:` filter. Every workflow that hosts a check named in
`required-aggregator.yml` starts on every non-draft PR and every push to
`master`; the first step of each required job runs the planner:

```bash
python3 scripts/ci/plan-ci-impact.py --event pull_request \
  --base <base-sha> --head <head-sha> --github-output "$GITHUB_OUTPUT"
```

It diffs the event's exact revisions — the **merge-base** of head and base for
a PR, the exact `before..head` for a push — and maps the changed paths onto
the selectors declared in `.github/ci-impact.json`:

| Selector | Owns | Gates |
| --- | --- | --- |
| `c_core` | `core/`, `ffmpeg-patches/`, `model/`, `testdata/`, golden fixtures | build legs, sanitizers, cppcheck, CodeQL C/C++, assertion density |
| `golden_harness` | `c_core` ∪ `python` | Netflix golden tests, coverage gate |
| `tiny_ai` | `c_core` ∪ `ai` ∪ `python` | Tiny AI (DNN suite + `ai/` pytests) |
| `python_lint` | `python` ∪ `ai` | CodeQL Python |
| `docs` | `docs/`, `mkdocs.yml`, `*.md`, `changelog.d/` | Docs build |
| `actions` | `.github/` | CodeQL Actions |
| `go`, `rust`, `shell`, `container` | their trees | (non-required workflows — still path-filtered, follow-up) |

Steps gated on a selector that is **not** impacted are skipped and the job
emits `::notice::<selector> not impacted (mode=… reason=…)` before reporting
`success`, so the aggregator always sees a real conclusion with a real reason.

The planner **fails closed**: an unknown top-level path, any status other
than add/modify (delete, rename, copy, mode change), a change to a
CI-authority file (the map, the planner, `scripts/ci/**`, the workflows
hosting required contexts, `.pre-commit-config.yaml`, `Makefile`,
`.clang-tidy`, …), a missing merge-base, a non-linear push or an over-large
diff all produce `mode=full` — every selector true, i.e. the pre-ADR-1140
behaviour.

Local use:

```bash
python3 scripts/ci/plan-ci-impact.py --event pull_request \
  --base "$(git merge-base origin/master HEAD)" --head HEAD --print
python3 -m unittest scripts/ci/tests/test_ci_impact.py   # map ↔ tree contract
```

Adding a top-level directory or file? Add it to `known_prefixes` /
`known_files` (and to a selector if a required check owns it); the contract
test fails otherwise, because an unknown path would silently force `full`
mode on every PR that touches it.

## Required-checks aggregator

The single required check on `master` branch protection is the
**Required Checks Aggregator** (see
[ADR-0313](../adr/0313-ci-required-checks-aggregator.md)). It runs on
every non-draft PR, polls for the named sibling check_runs to reach a
terminal state, and accepts `success`, `skipped`, or `neutral` per
check. Because the aggregator itself skips on drafts, draft PRs
display "missing required check" — same situation as item 1 above
and unmergeable for the same reason.

For the hardware-dependent `SYCL Parity (Arc A380)` check
([ADR-1177](../adr/1177-sycl-arc-self-hosted-runner.md)), the aggregator
reads the repository variable `SYCL_ARC_RUNNER_ENABLED`. While it is not
`true` (lane not provisioned or paused by the operator) an absent or skipped
job is accepted as passing; while it is `true` the job must report
`success`, and a skip — which is what the loud probe failure in
`sycl-parity.yml` produces when the runner is unregistered, offline, or the
probe token is rejected — fails the aggregator. Operator runbook:
[ci-self-hosted-sycl.md](ci-self-hosted-sycl.md).

## Twin-drift gate

`core/` carries same-directory `.c`/`.cpp` twin pairs left by the C++23
migration ([ADR-0729](../adr/0729-cpp23-wave3-bundle.md)). Twice a fix
landed on one twin and never reached the other, and twice a rename
(`mem.c` → `mem.cpp`, `dict.c` → `dict.cpp`) left a stale path in a build
file that only nightly or opt-in lanes configure.
[ADR-1135](../adr/1135-ci-twin-drift-gate.md) turns both into a blocking,
required check — `Twin Drift` in
[`lint-and-format.yml`](../../.github/workflows/lint-and-format.yml),
backed by
[`scripts/ci/twin-drift-check.sh`](../../scripts/ci/twin-drift-check.sh).

**The gate fails when either holds:**

1. A same-directory `.c`/`.cpp` pair exists and one side is compiled by
   **no** build file (`meson.build`, `setup.py`, `*.pyx`) — unless that
   side is listed in
   [`scripts/ci/twin-drift-allowlist.txt`](../../scripts/ci/twin-drift-allowlist.txt)
   **with a reason**.
2. Any build file names a source path (`.c .cpp .cc .cxx .cu .hip .m .mm
   .metal .pyx`) that does not exist in the tree.

**How references are resolved:**

| Build-file form | Resolution |
| --- | --- |
| `'../src/x.c'`, `'x.c'` | relative to the build file's directory |
| `src_dir + 'x.c'` | through the `src_dir = './…/'` assignment in the same file |
| `_m + '_parity.c'` (prefix is not a literal directory) | suffix search over `git ls-files`; reported as `NOTE` |
| `os.path.join("..", "core", "x.c")` | joined; identifiers resolve through assignments |
| `output: 'gen.c'`, `'@PLAINNAME@.c'` | skipped — generated files |
| `/abs/path.c` | skipped — toolchain-provided |
| `# …` comments | ignored (quote-aware) |

**Clearing a failure:**

- *Stale source reference* — fix the path in the build file. There is no
  allowlist for this class. If the parser genuinely cannot model a
  construct, append `# twin-drift-ignore: <reason>` to that line; the
  reason is mandatory and the line is reported as `NOTE`.
- *Dead twin side* — wire the side into a build file, delete it, or add a
  row `path  reason` to the allowlist. Rows without a reason, rows whose
  file is gone, whose side is compiled again, or whose pair no longer
  exists fail the gate, so the allowlist cannot rot.

Sides compiled only by test / fuzz build files are printed as `INFO`
(non-failing): that is the drift-risk shape to keep an eye on when
touching one of them.

**Local run** (identical to CI, about two seconds, no build needed; also
wired as a `pre-push` hook):

```bash
bash scripts/ci/twin-drift-check.sh
bash scripts/ci/tests/test-twin-drift-check.sh   # 24 fixture cases
```

## Whole-tree lint ratchet (ADR-1142)

Since [ADR-1142](../adr/1142-whole-codebase-standards.md) the coding standards
apply to every file in the tree, and CI enforces that with a **ratchet**
instead of a touched-files rule:

- `scripts/ci/tidy-ratchet.py` runs clang-tidy over **every** translation
  unit in a `compile_commands.json`, deduplicates diagnostics by
  `(path, line, column, check)`, counts `NOLINT` markers with no inline
  `ADR-NNNN` citation (a citation counts on the previous, the same or the
  next line, or anywhere in the `/* ... */` block comment that holds the
  marker), and compares the per-file numbers with the committed baseline
  `scripts/ci/tidy-baseline-<lane>.json`.
- The rule is *baseline equals measurement*. Exit codes: `0` match, `2` a file
  is above its baseline (fix the code, never raise the baseline), `3` a file is
  below its baseline (tighten it: `make tidy-ratchet-write`, commit the JSON
  in the same PR), `4` clang-tidy could not compile a TU (fail closed), `5`
  usage/IO error.
- **`cpu` lane** — the required context `Tidy Ratchet` in
  `lint-and-format.yml` (aggregator list, ADR-0313). Like every required job
  it always starts and first runs the [ADR-1140](../adr/1140-ci-impact-planner.md)
  impact planner (`scripts/ci/plan-ci-impact.py`, step id `impact`); the
  install / build / ratchet steps run only when the planner's `c_core`
  selector is `true`, otherwise a `Not impacted` notice satisfies the context.
  `.clang-tidy`, `scripts/ci/**` (the ratchet and its baselines) and the
  workflow file are CI-authority inputs that force `mode=full`, so a ratchet
  or baseline edit always runs the lane. It uploads `tidy-ratchet-cpu` (the
  measurement JSON): when the job fails with exit 3 after a cleanup, download
  that artifact and commit it as `scripts/ci/tidy-baseline-cpu.json` — the
  committed `cpu` baseline is always CI's own measurement (the hosted build
  lacks optional dependencies, so its TU set differs from a workstation
  build). The compile database also lists the model-JSON → C translation
  units meson generates under `build/src/` (`vmaf_v0.6.1.json.c`, …); they are
  measured like every other TU and appear in the baseline under that path, so
  the `cpu` lane is always measured with `--build-dir build` at the repository
  root, as CI does. The nightly workflow runs the same lane and fails on drift
  (it used to swallow the full scan with `|| true`).
- **`cuda`, `sycl`, `hip` lanes** — baselines committed from the 2026-09-02
  workstation measurement (clang-tidy 22.1.8 against a `-Denable_cuda=true
  -Denable_sycl=true -Denable_hip=true` build; CUDA TUs analysed with
  `--cuda-host-only -nocudalib`, HIP with `-x hip -D__HIP_PLATFORM_AMD__=1`,
  SYCL through `scripts/ci/clang-tidy-sycl.sh`). Run locally with
  `make tidy-ratchet LANE=cuda TIDY_RATCHET_BUILD_DIR=build-gpu` (same for
  `sycl`, `hip`). They become PR-required contexts as soon as a hosted
  toolchain exists for the lane; until then a lane that cannot run is reported
  as *not run*, never as clean. Metal (`.mm` / `.metal`) has no Linux
  toolchain and is tracked by structural proxy only.
- The changed-files job `Tidy Changed` stays as fast
  feedback and keeps the `WarningsAsErrors` hard stop; ADR-0141's "a touched
  file ends the PR at zero" is unchanged. The ratchet adds the bound on
  untouched files.

Baselines at the time this landed (2026-09-02): cpu 5,241 warnings / 281 TUs /
83 uncited NOLINTs; cuda 1,650; sycl 716; hip 1,173 (whole tree ≈ 8,780).

### Carve-outs still open after ADR-1142

The 2026-09-02 inventory ([research digest](../research/2027-lint-carveout-inventory-2026-09-02.md))
found 218 scope restrictions across the lint/CI configuration. This ADR's PR
retires the nightly `|| true` and bounds the whole CPU tree; the remaining
rows are owned by the wave that brings the blocking toolchain or build option
to CI:

| Carve-out | Blocker | Owner / plan |
| --- | --- | --- |
| Changed-files clang-tidy job excludes `core/src/cuda/`, `core/src/feature/cuda/`, `core/test/test_cuda_*`, `core/test/test_gpu_picture_pool.c` | CUDA toolkit headers on the hosted runner (`--cuda-host-only` needs them) | cuda lane → PR-required; retire the `grep -v` lines in the same PR |
| … excludes `core/src/sycl/`, `core/src/feature/sycl/`, `core/test/test_sycl*`; `Tidy SYCL (advisory)` job is `continue-on-error` | oneAPI on the runner (the advisory job already installs it) | sycl lane → PR-required first; drop `continue-on-error` |
| … excludes `core/src/hip/`, `core/src/feature/hip/`, `core/test/test_hip*` | ROCm headers on the hosted runner | hip lane → PR-required |
| … excludes `core/src/feature/arm64/` | no aarch64 compile DB on x86 runners | measure on the ARM build leg (cross `-target aarch64`) |
| … excludes `core/src/mcp/`, `core/test/test_mcp*`, `core/test/fuzz/`, `core/src/compat/win32/`, `core/tools/vmaf_vpl.c` | needs `-Denable_mcp=true` / fuzz / libva / MinGW compile DBs | add those TUs to the cpu-lane build in CI |
| `.cppcheck-suppressions.txt` per-file suppressions, `.clang-tidy` disabled checks, `.semgrep.yml` path excludes, `pyproject.toml` per-file ignores | none — each is a fix-the-code item | rework waves; each removal is a ratchet decrease |

## Bug-status hygiene gate (ADR-0165 / ADR-0334)

Per [CLAUDE.md §12 rule 13](../../CLAUDE.md) and
[ADR-0165](../adr/0165-state-md-bug-tracking.md), every PR that
closes a bug, opens a bug, or rules a Netflix upstream report
not-affecting-the-fork updates [`docs/state.md`](../state.md) in the
**same PR**. Until [ADR-0334](../adr/0334-state-md-touch-check-ci-gate.md)
this rule was reviewer-enforced; it now runs as the
`state-md-touch-check` job in
[`rule-enforcement.yml`](../../.github/workflows/rule-enforcement.yml),
backed by the single-purpose script
[`scripts/ci/state-md-touch-check.sh`](../../scripts/ci/state-md-touch-check.sh).

**The gate fires when any of the following hold:**

- PR title carries a Conventional-Commit `fix:` or `fix(scope):` prefix.
- PR title contains the bare token `bug` (word-boundary, so `debug`
  does not fire).
- PR title or body contains a `closes` / `fixes` / `resolves`
  `#N` GitHub-issue close keyword (case-insensitive).
- PR body has the `## Bug-status hygiene` template section with the
  `docs/state.md` checkbox left unchecked.

**The gate clears when either:**

1. The diff against `BASE_SHA..HEAD_SHA` includes
   [`docs/state.md`](../state.md) (the row landed in the
   appropriate section: Open / Recently closed / Confirmed
   not-affected / Deferred) **AND** none of the inserted lines
   carry a placeholder PR/commit reference (see
   "Placeholder-ref hardening" below), **or**
2. The PR description contains `no state delta: REASON` (REASON is
   any non-empty token that is not the literal placeholder
   `REASON`). Use this for pure `feat` / `refactor` / `infra` PRs
   that genuinely have no bug-status impact.

**Placeholder-ref hardening (ADR-0334 status update 2026-05-09).**
Touching `docs/state.md` is necessary but not sufficient. PR #541's
row audit found that the dominant staleness pattern is post-merge
backfill drift — closing PRs write `this PR` as the closer-PR
placeholder, the merge happens, the placeholder never gets rewritten
to the merged numeric refs. The gate therefore additionally rejects
any inserted line in `docs/state.md` containing:

| Placeholder | Why |
| --- | --- |
| `this PR` | post-merge backfill drift (most common) |
| `this commit` | same drift mode for SHA-shaped refs |
| `TBD` | obvious fill-it-in-later marker |
| `<PR>` | template placeholder |
| `#NNN` | template placeholder (real refs are digits) |

Canonical accept forms — explicitly NOT matched — are `PR #N` (any
positive integer) and ``commit `<sha>` `` (the SHA wrapped in
backticks). For an in-flight PR whose number is not yet final, you
can either:

1. Land the row with a placeholder, then push a follow-up commit
   that rewrites it to `PR #<number>` after `gh pr create` returns
   the number, **or**
2. Use `PR #<this-pr-number>` once GitHub has assigned it (the PR
   number is known the moment `gh pr create` exits).

**Local dry-run** (mirrors the
[`deliverables-check.sh`](../../scripts/ci/deliverables-check.sh)
pattern):

```bash
PR_TITLE="fix: foo segfault" \
PR_BODY="$(gh pr view 999 --json body -q .body)" \
  bash scripts/ci/state-md-touch-check.sh
```

Or pipe the body on stdin if `gh` isn't on `PATH`:

```bash
gh pr view 999 --json body -q .body \
  | PR_TITLE="fix: foo segfault" bash scripts/ci/state-md-touch-check.sh
```

The companion fixture script
[`scripts/ci/test-state-md-touch-check.sh`](../../scripts/ci/test-state-md-touch-check.sh)
exercises the gate against 18 cases (5 primary + 3 regression + 10
placeholder-ref). Run it after touching either script:

```bash
bash scripts/ci/test-state-md-touch-check.sh
```

## Local pre-flight gate

Before pushing, run the local subset of CI to catch the common
formatter / lint / fast-test failures:

```bash
make format-check   # clang-format + black + isort, no writes
make lint           # clang-tidy + cppcheck + iwyu + ruff + semgrep
meson test -C build --suite=fast
bash scripts/ci/twin-drift-check.sh  # .c/.cpp twin drift + stale source refs (ADR-1135)
pre-commit run --all-files  # if .pre-commit-config.yaml hooks are installed
```

The format-check + pre-commit pair catches roughly the same surface as
`lint-and-format.yml`'s `pre-commit` job in seconds, vs. a 10-minute
CI round-trip.
