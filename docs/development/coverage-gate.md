<!-- markdownlint-disable MD013 MD024 MD060 -->
# Coverage Gate — ADR-0922 Ratchet Semantics and Runbook

This document describes how the fork's Coverage Gate works, how the per-PR
ratchet introduced by ADR-0922 is wired into CI, and the runbook for raising
or lowering the coverage floor.

## Quick-reference

| What | Script / step | Threshold |
|------|---------------|-----------|
| Absolute floor — overall | `scripts/ci/coverage-check.sh` arg 2 | 70 % |
| Absolute floor — security-critical | `scripts/ci/coverage-check.sh` arg 3 | 90 % |
| Per-PR overall drop tolerance | `coverage-delta-check.sh --max-overall-drop` | 0.5 pp |
| Per-PR file drop tolerance | `coverage-delta-check.sh --max-file-drop` | 0.5 pp |
| Per-file override — `core/src/dnn/ort_backend.c` | `PER_FILE_MIN` map | 83 % |
| Per-file override — `core/src/dnn/dnn_api.c` | `PER_FILE_MIN` map | 83 % |
| Per-file override — `core/src/dnn/tiny_extractor_template.h` | `PER_FILE_MIN` map | 75 % |

All thresholds are enforced as required checks on every non-draft PR targeting
`master`.

---

## 1. Two complementary gates

The fork runs two separate coverage checks as part of the `coverage` CI job
in `.github/workflows/tests-and-quality-gates.yml`:

### 1.1 Absolute-floor gate (`coverage-check.sh`)

Runs on every push event **and** every PR. It reads the gcovr JSON summary
produced for the head commit and checks:

1. **Overall line coverage** must be at or above `OVERALL_MIN` (currently
   70 %).
2. **Security-critical files** (anything under `core/src/dnn/`,
   `core/src/opt.cpp`, `core/src/read_json_model.cpp`) must each be at or
   above `CRITICAL_MIN` (currently 90 %), unless the file has a per-file
   override in the `PER_FILE_MIN` map.

The gate fails with exit code 1 and prints the file and percentage that
violated the floor.

**What counts as "security-critical":** the `case` pattern in
`coverage-check.sh` matches `*core/src/dnn/*`, `*core/src/opt.cpp`, and
`*core/src/read_json_model.cpp`. These files parse user-supplied ONNX/JSON
paths and command-line options — attack surface where a missed branch or null
dereference has direct impact.

### 1.2 Per-PR delta gate (`coverage-delta-check.sh`)

Runs **only on `pull_request` events** (the `if: github.event_name == 'pull_request'`
condition on both the "Compute base-branch coverage" and "Enforce
coverage-delta gate" steps). It is not invoked on direct pushes to `master`.

The gate:

1. Builds the PR's merge-base commit in a lightweight worktree (CPU-only,
   no ORT, no Python suite) to produce `/tmp/base-coverage.json`.
2. Collects the already-built head coverage from `/tmp/head-coverage.json`
   (a copy of `core/build-coverage/coverage.json` saved before the worktree
   swap).
3. Runs `coverage-delta-check.sh` with both JSONs and the list of files
   touched by the PR (`git diff --name-only $MERGE_BASE..HEAD`).

**What the delta gate checks:**

- If **overall** coverage at head is more than `--max-overall-drop` (0.5 pp)
  below the base, the gate fails with exit code 1.
- If **any file** that appears in both the base report AND the PR diff drops
  by more than `--max-file-drop` (0.5 pp), the gate fails with exit code 2.
- Files added by the PR have no base row and are skipped (their absolute
  floor is enforced by the absolute gate).
- Files removed by the PR are skipped.
- Files not touched by the PR are skipped (the absolute gate already covers
  their floor; penalising a PR for unrelated test-suite drift would be
  unfair).

---

## 2. How the delta is computed on PR builds

The key question for correctness is: **what is "base"?**

The workflow step "Compute base-branch coverage for delta gate" does:

```bash
git fetch --no-tags --depth=1 origin "$BASE_REF"
MERGE_BASE="$(git merge-base HEAD "$BASE_REF")"
git diff --name-only "$MERGE_BASE"..HEAD > /tmp/changed-files.txt
git worktree add /tmp/base-tree "$MERGE_BASE"
```

`BASE_REF` is `${{ github.event.pull_request.base.sha }}` — the **tip of the
target branch at the time the PR was created/updated**, not the current
`master` tip. `git merge-base` then finds the actual common ancestor between
the PR head and that ref.

This means:

- The delta compares the PR head against the **true merge-base commit**,
  not `master` tip. This is correct: it measures what the PR itself changed,
  not drift that accumulated on `master` while the PR was open.
- If `master` advanced while the PR was open and the PR was not rebased, the
  delta comparison is still accurate because `merge-base` anchors correctly.
- If the PR is a rebase-on-master PR, `merge-base` equals `master` tip,
  which is the intended comparison point.

The base build is intentionally lean (no ORT, no Python suite, CPU-only) to
cap the additional CI cost to roughly 4 minutes on `ubuntu-latest`. It does
not instrument SIMD/AVX-512 paths that the full head build exercises; small
systematic differences in per-file numbers are tolerated by the 0.5 pp
tolerance band.

---

## 3. Per-file overrides (`PER_FILE_MIN`)

Some files have a structural ceiling that prevents them from reaching the
global 90 % critical floor. These are recorded in the `declare -A PER_FILE_MIN`
map in `coverage-check.sh`. Each entry must cite the ADR that justifies the
lower bar.

Current overrides:

| File | Floor | Justification |
|------|-------|---------------|
| `core/src/dnn/ort_backend.c` | 83 % | ADR-0114 — EP-availability structural ceiling; the CUDA EP attach success arm is unreachable on CPU-only ORT (no real GPU). |
| `core/src/dnn/dnn_api.c` | 83 % | ADR-0114 — same EP-availability ceiling as `ort_backend.c`. |
| `core/src/dnn/tiny_extractor_template.h` | 75 % | ADR-0881 — template helpers are only instantiated by callers; only the subset invoked by the four current extractors is reachable. |

**The per-file overrides are ratcheted upward**, not downward: each entry was
tightened by +5 pp by ADR-0922 and must only increase. To lower an override
a new ADR that explicitly supersedes ADR-0922 is required (see §5).

---

## 4. Floor history

| Floor (overall) | Date | Reason |
|-----------------|------|--------|
| 40 % | Original | Baseline post meson unit suite + Python feature/CLI tests. |
| 37 % | 2026-05-19 | ~2 200 LOC of new MCP/HIP/DNN/scaffold C added by the merge burst (PRs #1417–#1425) diluted overall coverage from ~39 % to 37.7 %. Floor tracked the measured value. |
| 60 % | 2026-05-31 | ADR-0922 aggressive ratchet; original proposal before coverage uplift PRs merged. |
| 70 % | 2026-05-31 | Post-merge of PRs #420 and #412 (DNN + Python harness coverage uplift), master measured above 70 %; floor raised to match. |

The critical floor followed a parallel track:

| Floor (critical) | Date | Reason |
|------------------|------|--------|
| 85 % | Original | ADR-0114 / docs/principles.md §3 target. |
| 90 % | 2026-05-31 | ADR-0922 aggressive ratchet; exceeds the documented 85 % aspirational target. |

---

## 5. Runbook: raising the floor

Raising the floor is straightforward and does not require an ADR. Directly
update the argument passed to `coverage-check.sh` in the workflow step
"Enforce coverage thresholds":

```yaml
scripts/ci/coverage-check.sh core/build-coverage/coverage.json <NEW_OVERALL> <NEW_CRITICAL>
```

Add a one-line comment in the step's history block documenting the date and
reason (see the existing `Floor history` comments in the workflow). Verify
that master CI is currently at or above the new floor before pushing (check
the latest `coverage-cpu` artifact under `core/build-coverage/coverage.txt`).

Similarly, to tighten a `PER_FILE_MIN` override, increment its value in
`coverage-check.sh` and add a comment citing the measurement evidence.

**No ADR is required to raise a floor.** Raising is always correct direction.

---

## 6. Runbook: lowering the floor (requires ADR)

Lowering any floor — the overall minimum, the critical minimum, a
`PER_FILE_MIN` override, or either delta tolerance — requires:

1. **File a new ADR** under `docs/adr/NNNN-*.md` (allocate with
   `scripts/adr/next-free.sh --claim <slug>`). The ADR must:
   - Explicitly state it supersedes ADR-0922 (and, if applicable, ADR-0114).
   - Quantify the structural reason the lower floor is unavoidable (e.g.,
     "the new subsystem has unreachable error paths on the CI runner by
     design; see §Alternatives").
   - Describe what mitigations are in place to prevent quality regression
     (e.g., integration tests on a different runner, fuzzing, etc.).
2. **Reference the new ADR inline** at the changed threshold in
   `coverage-check.sh` or `coverage-delta-check.sh`. An inline comment such
   as `# ADR-NNNN — supersedes ADR-0922, lowers floor to X%` is required.
   A bare number change without a citation is itself a lint violation
   (`scripts/ci/check-adr-numbering.sh` does not enforce this specific rule,
   but reviewers must reject it).
3. **Do not use `# noqa`-style inline escape hatches.** The per-file override
   map in `PER_FILE_MIN` is the only supported mechanism for per-file
   exceptions. Suppressing the check at the call site is not permitted.

---

## 7. Runbook: debugging a failing Coverage Gate

### 7.1 Absolute gate failed

Run the gate locally against the CI artifact:

```bash
# Download the coverage-cpu artifact from the failed run, then:
scripts/ci/coverage-check.sh coverage.json 70 90
```

Or rebuild locally:

```bash
cd core
meson setup build-coverage --buildtype=debug \
  -Db_coverage=true -Denable_cuda=false -Denable_sycl=false \
  -Denable_float=true -Denable_avx512=true -Denable_dnn=enabled \
  -Dc_args=-fprofile-update=atomic -Dcpp_args=-fprofile-update=atomic
ninja -C build-coverage
meson test -C build-coverage --print-errorlogs --num-processes 1
~/.local/bin/gcovr --root .. \
    --filter 'src/.*' \
    --exclude '.*/test/.*' \
    --exclude '.*/subprojects/.*' \
    --gcov-ignore-parse-errors=negative_hits.warn \
    --gcov-ignore-parse-errors=suspicious_hits.warn \
    --json-summary build-coverage/coverage.json \
    --txt build-coverage/coverage.txt \
    build-coverage
scripts/ci/coverage-check.sh build-coverage/coverage.json 70 90
```

The per-file `coverage.txt` output shows which files are dragging down the
overall or per-file numbers.

### 7.2 Delta gate failed

The CI log output from the "Enforce coverage-delta gate" step prints every
touched file with its base and head coverage and the delta. Example:

```text
Coverage-delta gate (ADR-0922)
  Base overall: 72.1234%
  Head overall: 71.5678%
  Overall delta: -0.5556pp (tolerance: -0.5pp)
  FAIL: core/src/foo.c: 85.0000% -> 83.0000% (-2.0000pp, tolerance -0.5pp)
```

**The delta gate can fire for legitimate reasons:**

- The PR removed or refactored code that previously had tests, lowering
  per-file coverage.
- The PR added significant new code without adding tests.
- The base build uses a different ORT / Python suite than the head build
  (the lean base build deliberately omits these), so per-file numbers may
  differ slightly for DNN files. If the file is `core/src/dnn/…` and the
  delta is small (≤ 1 pp), this is likely a build-configuration artifact.

**To resolve:** add tests covering the lines the PR touched, then re-run the
gate locally:

```bash
# Simulate the delta gate locally (you need both JSON summaries):
scripts/ci/coverage-delta-check.sh \
  --base-json /tmp/base-coverage.json \
  --head-json core/build-coverage/coverage.json \
  --changed-files <(git diff --name-only origin/master)
```

If the drop is structurally unavoidable (e.g., you are adding error-handling
code that is only reachable when a file descriptor is unavailable), open a
follow-up ADR superseding ADR-0922 per §6 above.

### 7.3 "Base coverage JSON not found" error

The base build step uses `git worktree add /tmp/base-tree` followed by a
lean meson build. If the worktree checkout fails (e.g., network issue
fetching `BASE_REF`), the delta gate step will error because
`/tmp/base-coverage.json` was never written. In that case:

- Re-run the workflow; the `git fetch --depth=1 origin "$BASE_REF"` retry
  logic (3 attempts, 5 s delay) usually recovers.
- If the merge-base commit is very old and not in the shallow history, the
  `|| true` on the `git fetch` allows the job to continue; `git merge-base`
  will then fall back to the oldest available ancestor, which may differ
  slightly from the true merge-base. This is acceptable for the delta gate —
  a false positive here is much safer than a false negative.

---

## 8. Relation to Codecov

The workflow also uploads `coverage.xml` to Codecov (ADR-0903) for trend
visualisation and PR comments. Codecov is **informational only** —
`fail_ci_if_error: false` is set. The enforcing threshold gate is always the
in-tree `coverage-check.sh` + `coverage-delta-check.sh` pair. Never rely on
a green Codecov badge alone; check the workflow step outcomes.

---

## 9. Related ADRs

| ADR | Topic |
|-----|-------|
| [ADR-0110](../adr/0110-coverage-gate-fprofile-update-atomic.md) | Atomic gcov counters; foundation of the current gate. |
| [ADR-0111](../adr/0111-coverage-gate-gcovr-with-ort.md) | `lcov → gcovr` migration that made per-file numbers honest. |
| [ADR-0114](../adr/0114-coverage-gate-per-file-overrides.md) | `PER_FILE_MIN` map and structural ceiling rationale. |
| [ADR-0117](../adr/0117-coverage-gate-warning-noise-suppression.md) | stderr filter for gcovr suspicious-hits noise. |
| [ADR-0637](../adr/0637-ci-test-failures-omnibus.md) | Committed to ratcheting upward as targeted tests landed. |
| [ADR-0881](../adr/0881-coverage-gate-tiny-extractor-template-floor.md) | `tiny_extractor_template.h` floor rationale. |
| [ADR-0903](../adr/0903-codecov-oidc-integration.md) | Codecov OIDC integration (informational dashboard). |
| [ADR-0922](../adr/0922-coverage-ratchet-aggressive.md) | Aggressive ratchet (37 % → 70 %) + per-PR delta gate. |
