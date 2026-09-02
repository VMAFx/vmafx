<!-- markdownlint-disable MD013 MD060 -->
# `scripts/ci/` — agent invariants

Parent: [../AGENTS.md](../AGENTS.md).

Fork-local CI utilities. Anything in this directory is invoked from
`.github/workflows/*.yml` (see "Rebase-sensitive surfaces" below);
upstream Netflix/vmaf has no equivalent tree, so the rebase risk is
"workflow drift" rather than "merge conflict".

## Rebase-sensitive surfaces

The following pairs are tightly coupled — a rename or signature
change in one **must** land alongside the matching update in the
other, in the **same PR**. Required-status-check names are derived
from the workflow file's `name:` fields, so a check that gets dropped
or renamed turns into a phantom-required gate that blocks every PR
until master is fixed.

| Script | Workflow lane(s) that invoke it | What couples them |
|---|---|---|
| `cross_backend_vif_diff.py` | `tests-and-quality-gates.yml` — every `*-cross-backend-diff` step | The `--feature`, `--backend`, `--places` flag names; the `FEATURE_METRICS` dict (workflow steps reference feature names verbatim). |
| `cross_backend_parity_gate.py` | `tests-and-quality-gates.yml` — `Run GPU-parity matrix gate` step | The `--gpu-id`, `--calibration-table`, `--backends`, `--features`, `--fp16-features`, `--json-out`, `--md-out` flag names. The matrix-gate step name (`gpu-parity-matrix-gate`) is itself a required-status check on PRs. |
| `cross_backend_calibration.py` | (loader, not invoked directly by workflow) | Imported by the two gate scripts via `sys.path.insert(0, …)`; lives next to them on purpose. Don't move it without updating the import sites. |
| `gpu_ulp_calibration.yaml` | (data, not invoked directly by workflow) | The default path is hard-coded as `Path(__file__).parent / "gpu_ulp_calibration.yaml"` in `cross_backend_calibration.DEFAULT_CALIBRATION_PATH`. Renaming this file is a breaking change for the gate scripts and any caller that didn't pass `--calibration-table` explicitly. |
| `test_calibration.py` | `tests-and-quality-gates.yml` — pytest collection (`pytest-tests` lane) | Discovered automatically by pytest; the test module name is part of the gate's contract. |
| `test_e2e_runtime_contract.py` | `rule-enforcement.yml` and `e2e-k8s.yml` — `Verify E2E runtime contract` | The always-on PR gate and exact E2E lane enforce explicit CPU node + Go server targets, three-image transfer into kind, exact-local Helm pulls, and the real chart-backed scoring case. Keep it outside the E2E trigger gate as well as inside the image job. |
| `test_security_workflow_contract.py` | `rule-enforcement.yml` — `Verify Security Scans concurrency contract` | The Security Scans group must include workflow, event name, and ref. This keeps same-event cancellation while preventing a schedule on `refs/heads/master` from canceling a master-push CodeQL run (or vice versa). |

| `agent-eligibility-precheck.py` | (no workflow lane today; called manually from `.claude/workflows/*.md` per [ADR-0355](../../docs/adr/0355-symphony-agent-dispatch-infra.md)) | Imports `scripts/lib/backlog_tracker.py` via `sys.path.insert(0, …)`. The two files move together. The exit-code contract (0 = eligible, 1 = block, 2 = bad CLI usage) and the `::error title=...::...` stderr format are **the** dispatcher contract; never change without updating `.claude/workflows/_template.md` in the same PR. |

| `state-md-touch-check.sh` | `rule-enforcement.yml` — `state-md-touch-check` job (ADR-0334) | Reads `$PR_TITLE`, `$PR_BODY`, `$BASE_SHA`, `$HEAD_SHA` from the workflow env. The trigger predicate (Conventional-Commit `fix:`, bare `bug` token, GitHub close-keywords, unchecked Bug-status checkbox) and opt-out sentinel (`no state delta: REASON`) are coupled to the `## Bug-status hygiene` row in `.github/PULL_REQUEST_TEMPLATE.md` — keep them in sync. |

| `state-md-touch-check.sh` | `rule-enforcement.yml` — `state-md-touch-check` job (ADR-0334) | Reads `$PR_TITLE`, `$PR_BODY`, `$BASE_SHA`, `$HEAD_SHA` from the workflow env. The trigger predicate (Conventional-Commit `fix:`, bare `bug` token, GitHub close-keywords, unchecked Bug-status checkbox) and opt-out sentinel (`no state delta: REASON`) are coupled to the `## Bug-status hygiene` row in `.github/PULL_REQUEST_TEMPLATE.md` — keep them in sync. The placeholder-ref hardening (ADR-0334 status update 2026-05-09) additionally rejects inserted lines in `docs/state.md` matching `this PR` / `this commit` / `TBD` / `<PR>` / `#NNN`; the placeholder vocabulary is coupled to PR #541's audit findings. |

| `test-state-md-touch-check.sh` | (local-only fixture driver, not invoked by CI) | Run before pushing changes to `state-md-touch-check.sh`; constructs throw-away `mktemp -d` git repos so the test is hermetic. |

| `twin-drift-check.sh` + `twin-drift-allowlist.txt` | `lint-and-format.yml` — `twin-drift-check` job ([ADR-1135](../../docs/adr/1135-ci-twin-drift-gate.md)); the `twin-drift-check` pre-push hook in `.pre-commit-config.yaml` | The job `name:` (`Twin Drift + Stale Source Refs (ADR-1135)`) is listed verbatim in `required-aggregator.yml` — rename both in the same commit or every PR blocks on a phantom check. The allowlist path is the default of `TWIN_DRIFT_ALLOWLIST`; each row is `<path> <reason>` and is validated (reason mandatory; a row whose file is gone, whose side is compiled again, or whose pair no longer exists fails the gate). The source-extension regex (`c cpp cc cxx cu hip m mm metal pyx`), the `output:` / `@…@` / absolute-path skip rules, the `var + 'x.c'` and `os.path.join` resolution, the suffix-search fallback and the `twin-drift-ignore: <reason>` marker are the parser contract — change them in the script AND in `tests/test-twin-drift-check.sh` together. The awk program must stay POSIX (mawk is Ubuntu's default `awk`): no `gensub`, no `length(array)`, no `--re-interval`-only syntax. |

| `tests/test-twin-drift-check.sh` | (local-only fixture driver, not invoked by CI) | Run before pushing changes to `twin-drift-check.sh`; 24 hermetic `mktemp -d` git-repo cases covering both predicates, the allowlist validation and every resolution rule. Also run it under `gawk --posix` when touching the awk. |

| `coverage-check.sh` | `tests-and-quality-gates.yml` — `Enforce coverage thresholds` step on both `coverage-cpu` and `coverage-gpu` (advisory) jobs | The CLI shape (`coverage-check.sh <gcovr-summary.json> <overall_min%> <critical_min%>`) and the in-script `PER_FILE_MIN` map are the gate definition. Every entry in `PER_FILE_MIN` must cite the ADR that justifies the lower bar ([ADR-0114](../../docs/adr/0114-coverage-gate-per-file-overrides.md)). Audit cadence + tighten/keep/remove rule codified in [ADR-0881](../../docs/adr/0881-coverage-overrides-audit-2026-05-30.md). Gcovr's emit-path format (currently `core/src/...` relative to repo root) is the join-key with `PER_FILE_MIN`; if a future gcovr upgrade changes that format, the override silently stops applying and the global 85 % gate kicks in — the per-line "min XX%" output is the canary. |
| `check-dispatch-registry.sh` | `.pre-commit-config.yaml` (`check-dispatch-registry` hook), `tests-and-quality-gates.yml` (`Pre-Commit` job) | Cross-references backend symbols `vmaf_fex_*_<backend>` in `core/src/feature/<backend>/` against `feature_extractor_list[]` in `core/src/feature/feature_extractor.cpp`. Fails if any backend symbol is omitted from the registration array. Test suite: `scripts/ci/tests/test-check-dispatch-registry.sh`. |

## Calibration table contract (ADR-0234)

`gpu_ulp_calibration.yaml` is the single source of truth for
per-GPU-generation tolerance overrides on the cross-backend parity
gate. The lookup contract:

1. Caller passes `--gpu-id <runtime_id>` to the gate. ID format
   follows Research-0041:
   - `vulkan:0xVVVV:0xDDDD`
   - `cuda:M.m`
   - `sycl:0xVVVV:DRIVER`
2. The loader picks the most-specific glob match (longest non-
   wildcard prefix wins; trailing `*` is supported).
3. If a row has a `features:` override for the cell, that wins.
   Otherwise the gate falls back to its built-in
   `FEATURE_TOLERANCE` default (preserving backward compatibility
   for every caller that pre-dates ADR-0234).
4. If `--gpu-id` is omitted, no calibration is consulted at all
   (legacy behaviour exact).

**Invariant**: `tolerance_for(feature, gpu_id, default)` returns
`default` whenever any of the resolution steps above falls through.
This is enforced by `test_calibration.py`. A future PR that
"optimises" the lookup must keep all four fallback paths intact, or
existing CI lanes that don't pass `--gpu-id` will silently change
behaviour.

## When adding a new lane

1. New `--feature` value → add to `FEATURE_METRICS` in *both*
   gate scripts (single source of truth lives in the parity gate;
   the per-feature script mirrors it). Add the workflow step to
   `tests-and-quality-gates.yml`.
   Existing compatibility names are not always `feature + suffix`:
   ADR-0586 renamed Vulkan integer ADM to `integer_adm_vulkan`, so both
   scripts must keep `BACKEND_EXTRACTOR_ALIASES[("adm", "vulkan")]`.
   ADR-0662 routes lavapipe motion parity through
   `BACKEND_EXTRACTOR_ALIASES[("motion", "vulkan")] =
   "integer_motion_vulkan"` because the legacy `motion_vulkan`
   compatibility extractor stays explicit-name only.
2. New backend → extend `BACKEND_SUFFIX`, `BACKEND_DEVICE_FLAG`,
   `BACKEND_DEFAULT_DEVICE` in both scripts.
3. New GPU arch → add a row to `gpu_ulp_calibration.yaml`. Mark it
   `status: placeholder` until a real-hardware corpus exists; the
   placeholder row is operationally a no-op (empty `features:`
   block).

## When updating from upstream

`scripts/ci/` is fork-introduced; nothing in here merges from
upstream. The risk on `/sync-upstream` is the opposite: an upstream
change to a feature extractor's emitted-metric names would silently
invalidate `FEATURE_METRICS` rows. Re-run the matrix gate after any
upstream sync that touches `core/src/feature/`.

## PR-body deliverables validator (`validate-pr-body.sh`)

`scripts/ci/validate-pr-body.sh`, `scripts/git-hooks/pre-push`, and
`scripts/git-hooks/pre-push-pr-body-lint.sh` are local mirrors of the
`.github/workflows/rule-enforcement.yml` deep-dive-checklist gate
(ADR-0108). They re-use `scripts/ci/deliverables-check.sh` verbatim as
the parser; the validator only injects the diff via a `PATH`-shim that
intercepts `git diff --name-only`.

`pre-push-pr-body-lint.sh` is the standalone entry point referenced by
the `.pre-commit-config.yaml` `validate-pr-body` hook (`stages:
[pre-push]`). The omnibus `pre-push` hook delegates to the same
validator logic. Both skip gracefully when `gh` is absent or no open PR
exists for the current branch.

**Invariant — single parser source of truth**: do not fork or
re-implement the deliverables-check parsing logic in any other
language. If the gate's regex shape ever changes, the change lands
in `deliverables-check.sh` and the validator picks it up
automatically. The test harness `test-validate-pr-body.sh` should
catch any drift between the validator's expectations and the
parser's actual behaviour.

**Invariant — shim scope**: the `git` shim built inside
`validate-pr-body.sh` intercepts only the `diff --name-only` call
shape. Every other `git` invocation falls through to the real
binary. A future change to `deliverables-check.sh` that uses a
different git subcommand to compute the diff must update the shim
accordingly, or `validate-pr-body.sh` will silently use the real
git's output (potentially fine, potentially wrong depending on
local repo state).

## assertion-density.sh — copyright-grep scope (ADR-0968)

`assertion-density.sh` identifies fork-added files by scanning the first
20 lines of each `.c` / `.cpp` for a Lusoris copyright marker. The grep
pattern **must** accept both the legacy format (`Lusoris and Claude
(Anthropic)`) and the current post-rebrand format (`Copyright YYYY
Lusoris`). The current pattern is:

```text
grep -qE "(Lusoris and Claude|Copyright [0-9]+ Lusoris)"
```

**Invariant**: do not simplify this to a single literal string. The
2026-05-27 copyright-rebrand decision (memory: `project_copyright_lusoris_only`)
dropped "and Claude (Anthropic)" from new files; older files in-tree still
carry the legacy form. A grep that matches only one format causes the script
to silently exit 0 ("no fork-added files found; skipping"), bypassing the
assertion-density gate for all files carrying the other format.

Test coverage: `scripts/ci/tests/test-assertion-density.sh` (T1–T6).

## Coverage Gate ratchet (ADR-0922)

`scripts/ci/coverage-check.sh` (absolute floors) and the new
`scripts/ci/coverage-delta-check.sh` (per-PR delta gate) are tightly
coupled to `.github/workflows/tests-and-quality-gates.yml` and to each
other. Rebase-sensitive invariants:

1. **Floors are one-way.** `OVERALL_MIN` (70), `CRITICAL_MIN` (90), and
   every `PER_FILE_MIN` value may be raised in any PR; lowering any of
   them requires a new ADR that explicitly supersedes ADR-0922 and is
   cited inline at the changed threshold. The change-control comment
   above each `PER_FILE_MIN` row carries the citation; do not delete
   those comments when editing the table.
2. **Delta-gate tolerances default to 0.5pp.** The two CLI flags
   (`--max-overall-drop`, `--max-file-drop`) exist for the workflow to
   pin the values explicitly; do not tighten beyond 0.5pp without first
   confirming gcov hit-count variance has fallen (current floor of
   variance ~0.2pp, see ADR-0922 alternatives table).
3. **Workflow coupling.** The `Compute base-branch coverage for delta
   gate` and `Enforce coverage-delta gate (ADR-0922)` steps in
   `tests-and-quality-gates.yml`'s `coverage` job require:
   - `actions/checkout` with `fetch-depth: 0` (the delta gate runs
     `git merge-base HEAD "$BASE_REF"` — shallow clone breaks it).
   - `gcovr>=8.0` installed in the runner (same dependency as
     `coverage-check.sh`).
   - The `coverage:` job's `if:` predicate still gates on draft-PR
     status (ADR-0331 self-hosted-runner economy convention applies
     even for the hosted CPU lane to avoid wasted base-coverage builds
     on draft PRs).
4. **Grace window.** PRs opened before 2026-05-31 are exempt from the
   new floors and the delta gate through 2026-06-30 (operational, not
   enforced in code). After 2026-06-30 the workflow can drop any
   remaining grace-related notes.
5. **Upstream sync impact.** Upstream Netflix/vmaf has no coverage
   gate, so `/sync-upstream` cannot conflict with these files. The
   only risk is that an upstream-introduced source file lands without
   any tests and drags overall coverage below the OVERALL_MIN floor; in
   that case the sync PR itself trips the gate and the resolution is to
   add tests in the same PR (preferred) or to land an ADR-0922 supersede
   ADR first (only if structurally impossible).

## Pre-commit hook hygiene — no submodules (ADR-0893)

`.pre-commit-config.yaml` ships the upstream `forbid-new-submodules`
hook. The fork pulls upstream Netflix/vmaf code via `subprojects/`
(Meson wraps with sha256 pinning) and `ffmpeg-patches/` (out-of-tree
patch series), **never** via `.gitmodules`. A submodule entry would
bypass:

- the wrap-pin sha256 enforcement,
- the CycloneDX SBOM walk (which inspects `subprojects/*.wrap`, not
  `.gitmodules`),
- and the license-allow-list audit.

If you need to add a new third-party dependency, use a Meson wrap
(or vendor it under a clear "Vendored 3rd-party" banner, with the
attendant `.semgrepignore` / `check-copyright` exclusion). Do not
work around the `forbid-new-submodules` hook with `--no-verify`.

**Pinned-revision audit cadence**: re-audit `.pre-commit-config.yaml`
revisions roughly every ~6 months or when CI surfaces a deprecation
warning. Use `pre-commit autoupdate` as a starting point but verify
each proposed bump against `git ls-remote --tags --refs <repo>` —
the autoupdate heuristic has a known sort-order bug on repos that
land point releases out of branch order (it suggested a
`gitleaks v8.30.1 → v8.30.0` downgrade during the ADR-0893 audit).
Alpha pre-releases (`X.Y.Za<N>`) are never an acceptable pin.

## CI impact planner (ADR-1140)

- `plan-ci-impact.py` + `.github/ci-impact.json` decide which surfaces a change
  touches; every required job runs it first and gates heavy steps on the
  selectors. It is **fail-closed**: unknown top-level paths, non-additive
  statuses (delete/rename/copy), CI-authority files (this directory included),
  missing merge-base, non-linear pushes and over-large diffs all yield
  `mode=full`.
- Any file under `scripts/ci/` is a CI-authority input: changing one forces
  `full` mode for that PR by design.
- `tests/test_ci_impact.py` (stdlib `unittest`) pins the map ↔ tree contract and
  the no-path-filter invariant on required-context workflows. Run it after
  adding a top-level directory or a required check.
