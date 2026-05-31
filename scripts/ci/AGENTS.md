# `scripts/ci/` — agent invariants

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


| `agent-eligibility-precheck.py` | (no workflow lane today; called manually from `.claude/workflows/*.md` per [ADR-0355](../../docs/adr/0355-symphony-agent-dispatch-infra.md)) | Imports `scripts/lib/backlog_tracker.py` via `sys.path.insert(0, …)`. The two files move together. The exit-code contract (0 = eligible, 1 = block, 2 = bad CLI usage) and the `::error title=...::...` stderr format are **the** dispatcher contract; never change without updating `.claude/workflows/_template.md` in the same PR. |

| `state-md-touch-check.sh` | `rule-enforcement.yml` — `state-md-touch-check` job (ADR-0334) | Reads `$PR_TITLE`, `$PR_BODY`, `$BASE_SHA`, `$HEAD_SHA` from the workflow env. The trigger predicate (Conventional-Commit `fix:`, bare `bug` token, GitHub close-keywords, unchecked Bug-status checkbox) and opt-out sentinel (`no state delta: REASON`) are coupled to the `## Bug-status hygiene` row in `.github/PULL_REQUEST_TEMPLATE.md` — keep them in sync. |

| `state-md-touch-check.sh` | `rule-enforcement.yml` — `state-md-touch-check` job (ADR-0334) | Reads `$PR_TITLE`, `$PR_BODY`, `$BASE_SHA`, `$HEAD_SHA` from the workflow env. The trigger predicate (Conventional-Commit `fix:`, bare `bug` token, GitHub close-keywords, unchecked Bug-status checkbox) and opt-out sentinel (`no state delta: REASON`) are coupled to the `## Bug-status hygiene` row in `.github/PULL_REQUEST_TEMPLATE.md` — keep them in sync. The placeholder-ref hardening (ADR-0334 status update 2026-05-09) additionally rejects inserted lines in `docs/state.md` matching `this PR` / `this commit` / `TBD` / `<PR>` / `#NNN`; the placeholder vocabulary is coupled to PR #541's audit findings. |

| `test-state-md-touch-check.sh` | (local-only fixture driver, not invoked by CI) | Run before pushing changes to `state-md-touch-check.sh`; constructs throw-away `mktemp -d` git repos so the test is hermetic. |

| `sycl-bench-env.sh` | (sourced via `eval "$(scripts/ci/sycl-bench-env.sh <version>)"` from any caller that needs side-by-side oneAPI activation; not directly invoked by a workflow today) | The script must keep `$ROOT` (derived from the `$ONEAPI_PREFIX` env or the `$VERSION` CLI arg) out of any `bash -c "..."` body — both inputs are externally-controlled and the 2026-05-30 shell-injection sweep round 2 demonstrated that a hostile prefix with a closing single-quote + `\|\|`-fallback escapes the helper subshell (`set -e` does not block OR-branches). The current form forwards `$ROOT` as a positional argument (`$1`) to `bash -c '... "$1/setvars.sh" ...' _ "$ROOT"` so the path is never re-tokenised by the shell. `scripts/ci/test-sycl-bench-env.sh` codifies the contract; run it before any change to this helper. |

| `test-sycl-bench-env.sh` | (local fixture driver, not invoked by CI today) | Side-channel oracle: drops a marker file under `mktemp -d` and verifies it is never created when a hostile `$ONEAPI_PREFIX` is passed to `sycl-bench-env.sh`. If you need to re-introduce a `bash -c "..."` form, this test must continue to pass — string interpolation of operator-supplied paths into a child shell's command body is a flat-out shell-injection foot-gun. |


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

```
grep -qE "(Lusoris and Claude|Copyright [0-9]+ Lusoris)"
```

**Invariant**: do not simplify this to a single literal string. The
2026-05-27 copyright-rebrand decision (memory: `project_copyright_lusoris_only`)
dropped "and Claude (Anthropic)" from new files; older files in-tree still
carry the legacy form. A grep that matches only one format causes the script
to silently exit 0 ("no fork-added files found; skipping"), bypassing the
assertion-density gate for all files carrying the other format.

Test coverage: `scripts/ci/tests/test-assertion-density.sh` (T1–T6).
