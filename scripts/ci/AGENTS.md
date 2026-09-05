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
| --- | --- | --- |
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
| `classify-dependency-pr.sh` | `rule-enforcement.yml` — `deep-dive-checklist` and `doc-substance-check` jobs ([ADR-1152](../../docs/adr/1152-dependency-pr-gate-exemption.md)) | Reads `$PR_AUTHOR`, `$HEAD_REF`, `$BASE_SHA`, `$HEAD_SHA` from workflow env. The exemption is author-AND-path-gated and must never be widened to a path glob alone. Bot identity requires `renovate[bot]` / `dependabot[bot]` (or `app/renovate` / `app/dependabot`), or a `renovate/*` / `dependabot/*` branch, AND all changed paths must be in the explicit manifest/lockfile allowlist. Bot PRs touching source code must still satisfy both documentation gates. Test suite: `scripts/ci/test-classify-dependency-pr.sh`. |
| `test-classify-dependency-pr.sh` | (local-only fixture driver, not invoked by CI) | Run before pushing changes to `classify-dependency-pr.sh`; exercises the predicate space across dependency-only diffs, mixed source diffs, non-bot authors, and real PR fixtures (#1206, #1207, #1212, #1214). |
| `check-runner-available.sh` | `sycl-parity.yml` (`runner-available` job, step `Check runner availability`) | Reads the lane switch `$RUNNER_ENABLED` (= `vars.SYCL_ARC_RUNNER_ENABLED`). Disabled: exit 0, `available=false`, no API call. Enabled: queries `GET repos/<repo>/actions/runners` with `$GH_TOKEN` (`secrets.SYCL_RUNNER_PROBE_TOKEN`) and requires an ONLINE runner labelled `sycl-arc`; API error, no such runner, or all offline = exit 1 with `::error::`. Never maps an API error to "unregistered". Test suite: `scripts/ci/tests/test-runner-available.sh`. |

## `check-vcs-version-not-bare-sha.sh` invariants

`core/include/meson.build` builds `VMAF_VERSION` from `git describe`, and
upstream Netflix/vmaf spells that call with `--always`. The fork deliberately
does not. With `--always`, git exits 0 even with no reachable `v*.*.*` tag and
prints a bare abbreviated object name, which meson writes into
`vcs_version.h` verbatim — so `vmaf --version`, the JSON/XML `version` field
and `vmaf_version()` all report a commit instead of a version on any shallow
checkout, tarball export, or worktree whose `.git` is a file.

Three properties are load-bearing, and this gate enforces each:

| Property | Why it matters |
| --- | --- |
| No `--always` in the `vcs_tag` command | It is what suppresses the non-zero exit that the fallback path depends on. |
| An explicit `fallback:` | Meson would default it to `meson.project_version()`, but the fallback *is* the tagless path here; spelling it out keeps the intent across meson upgrades. |
| `--match 'v*.*.*'` retained | Without it any tag in the repository can supply the version. |

Two things make the defect easy to reintroduce and hard to notice. It conflicts
with upstream on every sync, so a mechanical "take theirs" resolution restores
`--always`; and it is invisible until the seven-character abbreviation happens
to contain no ASCII digit — about one commit in a thousand — which is the only
condition `core/test/test_output.c::test_vmaf_version` can detect. Assume any
version-string failure on one leg is environmental until you have checked
whether the checkout could reach a tag.

`.github/workflows/build.yml` must therefore keep `fetch-depth: 0` on its
checkout: `git describe --long` needs both the tag objects and the commit
distance to them, and the `actions/checkout` default of 1 supplies neither.

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

## tidy-ratchet.py invariants (ADR-1142)

- `scripts/ci/tidy-baseline-<lane>.json` is generated only by `tidy-ratchet.py --write`
  (or `make tidy-ratchet-write`); never hand-edit a count. A baseline may only
  decrease; raising a number to make CI green is a policy violation, not a fix.
- The dedup key `(path, line, column, check)` and the NOLINT rule ("cited" =
  `ADR-NNNN` on the previous, the same or the next line, or anywhere in the
  `/* ... */` block comment that holds the marker; `NOLINTEND` never counts) are
  load-bearing: the baselines were measured with exactly these rules, so
  changing either requires re-measuring every lane in the same PR (the `cpu`
  baseline is CI's own `tidy-ratchet-cpu` artifact, never a workstation run).
- The `Tidy Ratchet` job starts unconditionally and gates its
  work on the ADR-1140 planner's `c_core` selector; `.clang-tidy`, this
  directory (ratchet + baselines) and the workflow are CI-authority inputs, so
  editing any of them forces `mode=full` and the lane runs. Never add a
  `paths:` filter or a custom early-skip probe to the job.
- A `clang-diagnostic-error` in any TU is a measurement failure (exit 4), never a
  zero. Build (generated headers) before measuring.

## release-pr-exempt.sh invariants (ADR-1151)

- The predicate is `release-please--` head ref **AND** bot author. Never relax
  it to head-ref-only: the four gates it disarms are required contexts, so a
  head-ref-only test would let anyone skip them by naming a branch
  `release-please--anything`.
- It always exits 0 and communicates through `exempt=true|false`. A gate that
  consumes it must use a step-level `if:` so the job still **reports** —
  skipping the whole job makes the check *absent*, which the aggregator's
  absent-means-pass rule (ADR-0313) cannot tell apart from a path-filter skip,
  and that is exactly the ambiguity the `mustReport` list exists to close.
- Only the four authoring-discipline gates may consult it: Deliverables
  Checklist, Doc-Substance Gate, `docs/state.md` Gate, FFmpeg-Patches Surface
  Sync. `Release Script Contract` and `ADR Collision Guard` stay armed on
  release PRs — the former is the gate that proves the cut ran, and it also runs
  `tests/test-release-pr-exempt.sh`, so the exemption's own test can never be
  skipped by the exemption.
- A new gate added to the aggregator's `required` array must be checked against
  a release PR's shape (a `.release-please-manifest.json` + coordinated
  version-marker diff, and a rendered-changelog body) before it is promoted.

## Adding a Renovate-managed surface (ADR-1152)

`classify-dependency-pr.sh` exempts a bot PR only when **every** changed path
matches its allowlist — one unmatched path fails the whole PR, and a bot cannot
write a deliverables checklist to recover. So whenever a new dependency-pinning
surface appears in the tree (a new chart under `deploy/helm/`, a new compose
file, a new container build file outside `docker/`), add it to
`is_allowed_dependency_path` **and** add a fixture case to
`test-classify-dependency-pr.sh` in the same change.

Two invariants the test suite pins deliberately — do not "simplify" them away:

- Widening the allowlist must never drop the conjunction with condition (a).
  A human-authored PR touching an allowlisted path must still be gated.
- A bot PR that touches an allowlisted path **and** source code must still be
  gated. That asymmetry is the entire point of the gate.

Derive additions from what Renovate actually edits (`gh pr list --author
app/renovate` and diff the file lists), not from what looks like a manifest —
see [`docs/research/1152-dependency-classifier-surface-audit.md`](../../docs/research/1152-dependency-classifier-surface-audit.md).

## check-aggregator-names.sh invariants

- Gates 1:1 parity between the 35 required status checks declared in
  `.github/workflows/required-aggregator.yml` (`const required = [...]`) and
  the `# required-aggregator` markers on `name:` fields across workflow files.
- Enforced locally via `make lint-sh` and pre-commit hook `check-aggregator-names`.
- Display names must stay concise ($\le 30$ chars) per `docs/development/ci-job-names.md`.
## Self-hosted SYCL Arc runner invariants (ADR-1177)

The Intel Arc A380 self-hosted runner executes hardware-in-the-loop SYCL parity tests
under `.github/workflows/sycl-parity.yml`. The following invariants are load-bearing:

1. **Untrusted fork PR execution prohibition**: `sycl-parity.yml` must strictly enforce
   `if: github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository`.
   Fork PRs must NEVER execute arbitrary workflows or code on self-hosted infrastructure.
2. **Device isolation**: Container passthrough (`dev/docker-compose.runner.yml`) is
   restricted to `/dev/dri/renderD129` (Intel Arc A380, vendor `0x8086`, device `0x56a5`,
   PCI `03:00.0`). The host NVIDIA RTX 4090 and AMD iGPU device nodes must NOT be passed into
   the container under any circumstances.
3. **Container security posture**: The runner container runs as an unprivileged user
   `runner` (uid 1001, gid 1001) in groups 988 (`render`) and 984 (`video`). No Docker socket
   (`/var/run/docker.sock`) is mounted. Container resource limits are capped at 8 CPUs and 16 GB RAM.
   Ephemeral mode (`--ephemeral`) ensures a clean environment per job without state persistence.
4. **Lane-switch contract**: `required-aggregator.yml` lists `SYCL Parity (Arc A380)` as required and
   reads `vars.SYCL_ARC_RUNNER_ENABLED` (it makes no runner API call — `GITHUB_TOKEN` cannot list
   self-hosted runners):
   - Lane disabled (variable unset / not `true`): absent or skipped is accepted as pass.
   - Lane enabled: the job MUST report `success`; absent or skipped (the probe failed because the
     runner is unregistered, offline, or the probe token was rejected) is a loud aggregator failure.
   Never reintroduce an auto-detect probe that treats an API error as "unregistered" — that makes a
   required check silently green.
5. **Probe token**: `check-runner-available.sh` runs the runner-list query only while the lane is
   enabled, with `secrets.SYCL_RUNNER_PROBE_TOKEN` (fine-grained PAT, single repository,
   Administration: read-only). Do not widen the workflow's `permissions:` in an attempt to replace it —
   there is no `administration` scope there.
6. **Render node is resolved, not hard-coded**: `dev/docker-compose.runner.yml` takes
   `ARC_RENDER_NODE` from `dev/scripts/arc-render-node.sh` (exactly one vendor-`0x8086` render node).
   Do not replace it with a bare `renderD<N>`; numbers change after PCI re-enumeration.
