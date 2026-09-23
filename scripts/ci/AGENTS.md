<!-- markdownlint-disable MD013 MD060 -->
# `scripts/ci/` — agent invariants

Parent: [../AGENTS.md](../AGENTS.md).

Fork-local CI utilities. Anything here invoked from
`.github/workflows/*.yml` (see "Rebase-sensitive surfaces" below);
upstream Netflix/vmaf has no equivalent tree, so rebase risk =
"workflow drift", not "merge conflict".

## Rebase-sensitive surfaces

### Local data-root separation (ADR-1277)

`check-local-data-contract.sh` separates three authorities: private state and
bounded cache under `.workingdir/`, datasets and reusable derived data under
`.corpus/`, and public evidence in tracked files. The retired numbered state
root stays unignored so accidental recreation is visible. Tracked Markdown may
show local paths in commands but never link into ignored data. Preserve the
hermetic shell suite, always-run pre-commit hook, and
`rule-enforcement.yml` invocation together.

### Exact-source Scorecard reports (ADR-1247)

`scorecard_gate.py` validates complete reviewed check sets and tool identity,
recomputes risk-weighted unrounded score, rejects scanner errors. Keep
all zero and inconclusive states in summaries; only Signed-Releases/-1 with
exact reason `no releases found` unassessed rather than error. PR-local
reports have no upstream commit identity: preserve Git-object byte/mode checks,
extra-input rejection, before/after run-bound receipts. Every followed
symlink component must track; do not permit links through Git metadata or
other mutable inputs even when final file tracked. Preserve literal
symlink targets and legitimate directory chains, with bounded cycle rejection.
Git subprocesses and fixtures must clear inherited GIT_* and caller
global/system configuration.
Never use latest public API results, merge SHA instead of PR head, or omit
check to improve denominator. Workflow/aggregator and source-tamper
controls run through `scorecard-policy-contract` hook and both gate jobs.
Preserve companion ADR-1248 offline repository-policy hook and live master
checker; neither local fixture pass nor aggregate score proves settings.

### Configured native lint (ADR-1142)

`lint-configured.py` owns local `make lint-c` selection. Make first regenerates
Meson metadata with `--reconfigure BUILD_DIR LIBVMAF_DIR`, without option
overrides, then builds generated prerequisites. Make must prepend the absolute
`VIRTUAL_ENV_ABS` to `PATH`: Meson persists its resolved Ninja command and
later launches it from the build directory, where relative `.venv/bin` is
invalid. The real-Make fixture asserts this path stays absolute. Because Meson
1.12 no longer materialises its native database, `write-compile-commands.py`
must then export
exactly Ninja's `c_COMPILER` and `cpp_COMPILER` rules. Keep that export
validated, atomic and fail-closed; never accept an empty/partial rule set or
replace a last-valid database after a failed export. Intersect the resulting
native database with tracked native sources, including engine roots, tests,
C++ tools and tracked vendored code. Preserve every configured command variant;
never infer commands for inactive backends, never regenerate database with
unfiltered `ninja -t compdb`. Only positive numeric `-flto=N` becomes `-flto`
in private analyzer copy. Keep missing/invalid inputs fatal, report excluded
scope, run cppcheck even after clang-tidy fails. Scratch-Git fixture
`tests/test_lint_configured.py` executes real Make target and both analyzer
boundaries; `tests/test_write_compile_commands.py` owns exporter failure and
last-valid-file preservation. Required Pre-Commit runs both when driver,
exporter, workflows or Makefile change.
Every configured clang-tidy command carries `--warnings-as-errors=*`. The tool
otherwise exits zero after printing ordinary findings, which turns a red
whole-tree inventory into a false-green gate. Preserve the fixture that emits
a warning with clang-tidy's default zero exit and proves the driver promotes it
to failure. Do not replace this with log parsing, a baseline, touched-file
selection, or an upstream-origin exemption.
Does not replace lane-specific ratchet measurements or their baselines.

### SYCL custom-command lint database

Meson emits per-feature SYCL translation units as `CUSTOM_COMMAND` rules, so
`gen-sycl-compile-commands.py` must augment the native compilation database
before clang-tidy can see them. Keep both legacy `-Xs` removal and the current
target-scoped pair (`-Xsycl-target-backend=spir64_gen` plus its following
backend argument) in the translator; passing either device-only option to
stock clang++ breaks the analyzer lane. `test_sycl_aot_command.py` is the
required pre-commit contract for the Meson AOT spelling, built-in language
standard policy, and translator output. Do not exempt a SYCL TU because it was
ported from upstream or predates the gate.

Running the generator is not left to the caller. `make tidy-ratchet` and
`make tidy-ratchet-write` expand `TIDY_RATCHET_COMPDB_$(LANE)` between the
native `write-compile-commands.py` export and the measurement; for `sycl` that
variable runs `gen-sycl-compile-commands.py`, and for `cpu` / `cuda` / `hip` /
`arm64` it is empty ([ADR-1290](../../docs/adr/1290-sycl-tidy-lane-compile-database.md)).
Keep the hook in both targets and keep it ordered between the two: while it was
missing the lane measured zero SYCL feature TUs, and `tidy-baseline-sycl.json`
recorded an empty backend while still reporting the lane as clean.
`test_tidy_ratchet_sycl_compdb.py` pins that wiring. The GPU lanes also need
their build dir configured `-Db_lto=false` and placed outside the repository;
see the variable block in the `Makefile`.

Real-Make fixtures create failing/recording pip sentinel before fake
Meson and Ninja, satisfying recursive build dependency graph without tool
bootstrap. GNU Make does not propagate `-o` to sub-makes. Keep `PIP_NO_INDEX=1`
and assertions that no pip call, real venv or sentinel overwrite occurred;
host network access must never turn broken fixture into passing test.

Both local and required CI cppcheck invocations load the installed analyzer's
full `posix` model through `write_cppcheck_posix_model.py`. Fork pthread types on
POSIX and in the Windows compatibility shim are C aggregates, not unknown C++
classes with implicit constructors. For pre-2.22 models, the generator inserts
the missing `pthread_cond_init` contract. For the newer defective shape it
removes only argument 2's `not-null` marker: POSIX permits `NULL` to select
default condition attributes. Preserve argument 1's marker and every other
installed model node; leave an already-correct entry unchanged. Resolve the
paired model through `--filesdir` or the pre-2.18 install-relative layout; do
not copy or hand-maintain a second POSIX model. Unknown/duplicate model shape,
missing source, or analyzer validation failure must fail closed and leave any
last-valid generated file intact.

Keep the generated model separate from target selection: preserve database
defines, include paths and language settings, with no forced platform or
language. `tests/test_cppcheck_posix_model.py` runs actual cppcheck on shared
headers, nullable/default attributes, the still-non-null condition object, and
uninitialized-member/constructor negative controls in the Cppcheck job after
installation. Missing tools/models fail that test; no diagnostic category is
disabled. See [model investigation](../../docs/research/cppcheck-pthread-model-2026-09-08.md).

Both paths select `--check-level=exhaustive` (ADR-1245). Preserve this value-flow
policy alongside existing severity sets and all command variants; never
suppress `normalCheckLevelMaxBranches` to hide incomplete analysis. Real-tool
suite includes normal/exhaustive branch-budget controls and defect controls.

Both paths also load `cppcheck-public-entrypoints.cfg` (ADR-1246). Keep its exact
public names shared; adding private helper to remove unused warning is not
export contract. Configured-driver hook validates model against
`VMAF_EXPORT` declarations and Meson's explicit installed-header lists, including
option-conditional headers. Preserve its cfg/header/workflow/test trigger paths.
Real-tool Cppcheck suite must reject missing/invalid models and still find
unlisted unused helpers and defects inside listed bodies. Entry names are
scope/linkage-blind: do not reuse them for private/static functions. Keep
measured collision control and both existing severity selections unchanged.

### Base-image references (ADR-1231)

`check-base-image-single-source.sh` delegates FROM/COPY instruction parsing to
`check-container-image-references.py`. External references without digest
still external: never restore old `*@sha256:*`-only detection. Keep
instruction case, flags and continuations covered by fixture tests.
Shared FROM arguments need global, single-line default named by
`build-config.env`; shell gate owns default drift/repair. Local image
exceptions bind exact consumer and value, never broad unpinned-tag rule.
`tests/test_base_image_single_source.py` exercises actual gate in scratch
repositories, wired through `test-base-image-single-source` in
`.pre-commit-config.yaml`.

### CUDA coordinated pin (ADR-1285)

One CUDA release = sixteen literals, seven spellings, seven files. Authority =
`build-config.env` `CUDA_VERSION`. `check-cuda-pin-lockstep.py` checks all of
them, `--write` derives the five Renovate cannot express (`$cudaMajorMinor`,
both `cuda-toolkit-NN-N` apt names, the OCI description label), and a residual
sweep fails on any CUDA release literal in an unrecognised spelling. Never
narrow the sweep to silence a new site: teach the gate its shape and add the
file to `renovate.json`'s CUDA manager in the same change, or the site drifts.
`--write` must never touch `CUDA_VERSION` (authority) or an image pin (digest
is not derivable). Renovate side: custom manager resolving the plain `x.y.z`
literals as `nvidia/cuda`, `extractVersion` stripping the flavour suffix
because no bare tag exists, and the `CUDA release (coordinated pin)` rule
scoped to major/minor/patch so digest refreshes stay in `Docker digests`.
Fixture: `tests/test_cuda_pin_single_source.py`, run by the
`test-base-image-single-source` hook.

### Level Zero version consumption (ADR-1231)

`dev/Containerfile` copies and sources `build-config.env` in its SDK download
RUN. Loader version = runtime shell input, so needs no Docker ARG
mirror. Preserve source step, both URL components and command-execution
fixture in `tests/test_level_zero_single_source.py`; base-image test hook
runs both single-source suites. `check-workflow-versions.py` verifies this
container consumer alongside Windows workflow mirror. Renovate tracks
Level Zero only in `build-config.env`; ROCm uses central image manager.

`check-workflow-versions.py` also owns the formatter pins: the `Makefile`'s
`RUFF_VERSION` / `BLACK_VERSION` must equal the ruff-pre-commit and black revs
in `.pre-commit-config.yaml`, and a recipe may not spell `ruff==<n>` or
`black==<n>` as a literal. Renovate moves both files through the
`pre-commit hooks` group (two regex managers on `Makefile`); keep the group
name identical on both rules or the bumps split into two pull requests and the
first one fails this gate. Fixture: `tests/test_formatter_pins_single_source.py`.

`check_mypy_python_version` owns the third pairing (ADR-1282): `pyproject.toml`'s
`[tool.mypy] python_version` must equal the `major.minor` floor of
`[project] requires-python` and the `major.minor` of `PYTHON_CI_VERSION`.
Deleting the key is a finding too — mypy would then model whichever interpreter
the caller runs. A comment was the only thing holding these together before, and
the pin stayed at `3.10` against a `>=3.14` floor long enough to abort every
`ai/src/` run (mypy will not parse numpy's PEP 695 `type` statement below 3.12).
Raise all three in one commit. Fixture:
`tests/test_mypy_python_version_single_source.py`; add its path and any new
trigger file to the `test-base-image-single-source` hook's `files:` regex, which
is what decides when the `test_*single_source.py` discovery runs.

### Dev-container GitHub build secret (ADR-1271)

`check-dev-container-build-secret.py` binds five surfaces: the optional
`github_token` mount in `dev/Containerfile`, its Compose environment source,
the authenticated raw build in `dev-container-build.yml`, the NEO fetcher's
rate-limit remedy, and the anonymous/authenticated operator examples. Never
replace the secret with `ARG` or `ENV`, make it required, or expose it to the
runtime service. `tests/test_dev_container_build_secret.py` mutation-checks
those failure modes and is wired to pre-commit/pre-push; the Dev Container
workflow additionally runs native Docker and Compose `--check` before building.

### Workflow coupling

Following pairs tightly coupled — rename or signature
change in one **must** land alongside matching update in
other, in **same PR**. Required-status-check names derive
from workflow file's `name:` fields, so check dropped
or renamed turns into phantom-required gate that blocks every PR
until master fixed.

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `cross_backend_vif_diff.py` | `tests-and-quality-gates.yml` — every `*-cross-backend-diff` step | The `--feature`, `--backend`, `--places` flag names; the `FEATURE_METRICS` dict (workflow steps reference feature names verbatim). |
| `cross_backend_parity_gate.py` | `tests-and-quality-gates.yml` — `Run GPU-parity matrix gate` step | The `--gpu-id`, `--calibration-table`, `--backends`, `--features`, `--fp16-features`, `--json-out`, `--md-out` flag names. The matrix-gate step name (`gpu-parity-matrix-gate`) is itself a required-status check on PRs. |
| `cross_backend_calibration.py` | (loader, not invoked directly by workflow) | Imported by the two gate scripts via `sys.path.insert(0, …)`; lives next to them on purpose. Don't move it without updating the import sites. |
| `gpu_ulp_calibration.yaml` | (data, not invoked directly by workflow) | The default path is hard-coded as `Path(__file__).parent / "gpu_ulp_calibration.yaml"` in `cross_backend_calibration.DEFAULT_CALIBRATION_PATH`. Renaming this file is a breaking change for the gate scripts and any caller that didn't pass `--calibration-table` explicitly. |
| `test_calibration.py` | `tests-and-quality-gates.yml` — pytest collection (`pytest-tests` lane) | Discovered automatically by pytest; the test module name is part of the gate's contract. |
| `test_e2e_runtime_contract.py` | `rule-enforcement.yml` and `e2e-k8s.yml` — `Verify E2E runtime contract` | The always-on PR gate and exact E2E lane enforce explicit CPU node + Go server targets, three-image transfer into kind, exact-local Helm pulls, and the real chart-backed scoring case. Keep it outside the E2E trigger gate as well as inside the image job. |
| `test_go_workflow_contract.py` | `rule-enforcement.yml` — `Verify Go required-check contract` | Executes the embedded aggregator with Go pass/fail outcomes and guards ready-event coverage plus step-level `go_checks` routing. Keep it before authoring exemptions (ADR-1238). |
| `test_security_workflow_contract.py` | `rule-enforcement.yml` — `Verify Security Scans concurrency contract` | The Security Scans group must include workflow, event name, and ref. This keeps same-event cancellation while preventing a schedule on `refs/heads/master` from canceling a master-push CodeQL run (or vice versa). |
| `test_fail_closed_ci.py` | `rule-enforcement.yml` — `Verify fail-closed CI contract`; `.pre-commit-config.yaml` — `fail-closed-ci-contract` | Protects real exit propagation for tox coverage, CPU coverage pytest, nightly benchmarks, advisory Semgrep, and sanitizer test discovery. Diagnostic continuation is valid only when a final `if: always()` step reasserts the recorded raw outcome. Keep both callers wired. |
| `tests/test-dedupe-gate.sh` | `standards-gate.yml` — `Reject duplicate implementation families`; `rule-enforcement.yml` — `Verify duplicate implementation gate`; `.pre-commit-config.yaml` — `dedupe-gate-contract`; `lefthook.yml`; `make verify-all` | The clone scan stays explicit in the required Standards job, both blocking local lefthook stages, and the aggregate local command. Its real-Make fixture proves a scanner failure makes `make verify-all` fail. `standardsctl audit` is not a substitute because it does not run the AST clone detector. |

| `agent-eligibility-precheck.py` | (no workflow lane today; called manually from `.claude/workflows/*.md` per [ADR-0355](../../docs/adr/0355-symphony-agent-dispatch-infra.md)) | Loads `scripts/lib/backlog_tracker.py`; the two files move together. The exit-code contract (0 = eligible, 1 = block, 2 = bad CLI usage) and the `::error title=...::...` stderr format are **the** dispatcher contract. Missing rows, unreadable task files, and unavailable or failed GitHub queries block dispatch unless the operator selected the corresponding explicit `--skip-*` flag. Never change the contract without updating `.claude/workflows/_template.md` and `docs/development/agent-dispatch.md` in the same PR. |

| `state-md-touch-check.sh` | `rule-enforcement.yml` — `state-md-touch-check` job (ADR-0334) | Reads `$PR_TITLE`, `$PR_BODY`, `$BASE_SHA`, `$HEAD_SHA` from the workflow env. The trigger predicate (Conventional-Commit `fix:`, bare `bug` token, GitHub close-keywords, unchecked Bug-status checkbox) and opt-out sentinel (`no state delta: REASON`) are coupled to the `## Bug-status hygiene` row in `.github/PULL_REQUEST_TEMPLATE.md` — keep them in sync. |

| `state-md-touch-check.sh` | `rule-enforcement.yml` — `state-md-touch-check` job (ADR-0334) | Reads `$PR_TITLE`, `$PR_BODY`, `$BASE_SHA`, `$HEAD_SHA` from the workflow env. The trigger predicate (Conventional-Commit `fix:`, bare `bug` token, GitHub close-keywords, unchecked Bug-status checkbox) and opt-out sentinel (`no state delta: REASON`) are coupled to the `## Bug-status hygiene` row in `.github/PULL_REQUEST_TEMPLATE.md` — keep them in sync. The placeholder-ref hardening (ADR-0334 status update 2026-05-09) additionally rejects inserted lines in `docs/state.md` matching `this PR` / `this commit` / `TBD` / `<PR>` / `#NNN`; the placeholder vocabulary is coupled to PR #541's audit findings. |

| `test-state-md-touch-check.sh` | (local-only fixture driver, not invoked by CI) | Run before pushing changes to `state-md-touch-check.sh`; constructs throw-away `mktemp -d` git repos so the test is hermetic. |

| `check-aggregator-names.sh` | `check-aggregator-names` pre-commit hook; `rule-enforcement.yml` — `Required check names have one reporter each (ADR-1259)` (gate + `tests/test-check-aggregator-names.sh`) | Two invariants: the aggregator's `required` list equals the `# required-aggregator`-marked names, and each required name is reported by exactly one job (the aggregator keeps only the newest run per name, so a shared name lets one job mask the other's failure — `Windows MSVC+CUDA`, fixed 2026-09-19). `job_names()` skips everything under a `steps:` key and the workflow's top-level `name:`; change that parser and the fixture test together. A new lane must not reuse a required name. |
| `sycl-bench-env.sh` | (sourced via `eval "$(scripts/ci/sycl-bench-env.sh <version>)"` by any caller that needs side-by-side oneAPI activation; no workflow invokes it today) | `$ROOT` (from the `$ONEAPI_PREFIX` env or the version argument, both externally controlled) must stay out of any `bash -c "..."` body. It reaches the helper subshell as a positional argument: `bash -c '... source "$1/setvars.sh" ...' _ "$ROOT"`, where the body is a single-quoted literal. Interpolated, a prefix that closes the quote (`x' \|\| <payload>; false #`, or `x'$(<payload>)'`) runs arbitrary code; `set -e` blocks neither. **This fix (PR #350) was reverted once by a stale squash-merge (PR #414) and re-applied on 2026-09-19** — when resolving a conflict here, never take the double-quoted form. `test-sycl-bench-env.sh` is the gate. |
| `test-sycl-bench-env.sh` | `.pre-commit-config.yaml` — `test-sycl-bench-env` hook (pre-commit + pre-push); the required `Pre-Commit` CI job runs it with `--all-files` | Side-channel oracle: a marker file under `mktemp -d` that a hostile `$ONEAPI_PREFIX` would create, plus a check that the hostile prefix is still sourced as a literal path. It existed when the fix was reverted and would have failed (4 of 7 cases fail on the vulnerable form), but nothing ran it. Do not unwire the hook; a regression test that no gate executes protects nothing. |
| `tests/test-dev-mcp-entrypoint-probe.sh` | `.pre-commit-config.yaml` — `test-dev-mcp-entrypoint-probe` hook (pre-commit + pre-push); the required `Pre-Commit` CI job | Guards `_probe_with_retry` in `dev/scripts/dev-mcp-entrypoint.sh`: the probe is one program name run as argv[0], never `eval "${cmd}"` (same PR #350 / PR #414 history). The test lifts the function out of the real entrypoint with `awk` between `^_probe_with_retry() {` and the first `^}` — keep the function at top level with that exact opening line, or the test exits 2. It stubs `sleep`, so the ten-attempt / always-return-0 contract is asserted too. End shell tests with `[[ "$fail" -eq 0 ]]`, not `exit 0`: a trailing unconditional `exit` makes shellcheck 0.11 report every trap handler and stub as SC2329. |
| `tests/test_semgrep_vendored_scope.py` | `.pre-commit-config.yaml` — `test-semgrep-vendored-scope` hook (own `semgrep` venv, like `semgrep-local`); the required `Pre-Commit` CI job | Planted-defect recall for the `vmaf-no-strcpy-strcat-sprintf` rule: copies the real `.semgrep.yml` + `.semgrepignore` into a throwaway project, plants banned calls at `core/src/mcp/3rdparty/cJSON/` and `core/src/pdjson.{c,h}`, and requires both scan forms (whole tree = CI, explicit files = hook) to report them; then scans the real vendored files. It exists because a rule-level `paths.exclude` plus a `.semgrepignore` line hid vendored cJSON, so the 1.7.19 re-vendor reverted ADR-0683 / ADR-1061 with every gate green. A finding in vendored code is answered by fixing the code, never by an exclusion (ADR-1142). `--no-git-ignore` is deliberate: the throwaway project may sit under a git-ignored `TMPDIR`. |

| `twin-drift-check.sh` + `twin-drift-allowlist.txt` | `lint-and-format.yml` — `twin-drift-check` job ([ADR-1135](../../docs/adr/1135-ci-twin-drift-gate.md)); the `twin-drift-check` pre-push hook in `.pre-commit-config.yaml` | The job `name:` (`Twin Drift + Stale Source Refs (ADR-1135)`) is listed verbatim in `required-aggregator.yml` — rename both in the same commit or every PR blocks on a phantom check. The allowlist path is the default of `TWIN_DRIFT_ALLOWLIST`; each row is `<path> <reason>` and is validated (reason mandatory; a row whose file is gone, whose side is compiled again, or whose pair no longer exists fails the gate). The source-extension regex (`c cpp cc cxx cu hip m mm metal pyx`), the `output:` / `@…@` / absolute-path skip rules, the `var + 'x.c'` and `os.path.join` resolution, the suffix-search fallback and the `twin-drift-ignore: <reason>` marker are the parser contract — change them in the script AND in `tests/test-twin-drift-check.sh` together. The awk program must stay POSIX (mawk is Ubuntu's default `awk`): no `gensub`, no `length(array)`, no `--re-interval`-only syntax. |

| `tests/test-twin-drift-check.sh` | (local-only fixture driver, not invoked by CI) | Run before pushing changes to `twin-drift-check.sh`; 24 hermetic `mktemp -d` git-repo cases covering both predicates, the allowlist validation and every resolution rule. Also run it under `gawk --posix` when touching the awk. |

| `coverage-check.sh` | `tests-and-quality-gates.yml` — `Enforce coverage thresholds` step on both required `coverage` and `coverage-gpu` jobs | The CLI shape (`coverage-check.sh <gcovr-summary.json> <overall_min%> <critical_min%>`) and the in-script `PER_FILE_MIN` map are the gate definition. Every entry in `PER_FILE_MIN` must cite the ADR that justifies the lower bar ([ADR-0114](../../docs/adr/0114-coverage-gate-per-file-overrides.md)). Audit cadence + tighten/keep/remove rule codified in [ADR-0881](../../docs/adr/0881-coverage-overrides-audit-2026-05-30.md). Gcovr's emit-path format (currently `core/src/...` relative to repo root) is the join-key with `PER_FILE_MIN`; if a future gcovr upgrade changes that format, the override silently stops applying and the global 85 % gate kicks in — the per-line "min XX%" output is the canary. |
| `check-dispatch-registry.sh` | `.pre-commit-config.yaml` (`check-dispatch-registry` hook), `tests-and-quality-gates.yml` (`Pre-Commit` job) | Cross-references backend symbols `vmaf_fex_*_<backend>` in `core/src/feature/<backend>/` against `feature_extractor_list[]` in `core/src/feature/feature_extractor.cpp`. Fails if any backend symbol is omitted from the registration array. Test suite: `scripts/ci/tests/test-check-dispatch-registry.sh`. |
| `classify-dependency-pr.sh` | `rule-enforcement.yml` — `deep-dive-checklist` and `doc-substance-check` jobs ([ADR-1152](../../docs/adr/1152-dependency-pr-gate-exemption.md)) | Reads `$PR_AUTHOR`, `$HEAD_REF`, `$BASE_SHA`, `$HEAD_SHA` from workflow env. The exemption is author-AND-path-gated and must never be widened to a path glob alone. Bot identity requires `renovate[bot]` / `dependabot[bot]` (or `app/renovate` / `app/dependabot`), or a `renovate/*` / `dependabot/*` branch, AND all changed paths must be in the explicit manifest/lockfile allowlist. Bot PRs touching source code must still satisfy both documentation gates. Test suite: `scripts/ci/test-classify-dependency-pr.sh`. |
| `test-classify-dependency-pr.sh` | (local-only fixture driver, not invoked by CI) | Run before pushing changes to `classify-dependency-pr.sh`; exercises the predicate space across dependency-only diffs, mixed source diffs, non-bot authors, and real PR fixtures (#1206, #1207, #1212, #1214). |
| `check-runner-available.sh` | `sycl-parity.yml` (`runner-available` job, step `Check runner availability`) | Reads the lane switch `$RUNNER_ENABLED` (= `vars.SYCL_ARC_RUNNER_ENABLED`). Disabled: exit 0, `available=false`, no API call. Enabled: queries `GET repos/<repo>/actions/runners` with `$GH_TOKEN` (`secrets.SYCL_RUNNER_PROBE_TOKEN`) and requires an ONLINE runner labelled `sycl-arc`; API error, no such runner, or all offline = exit 1 with `::error::`. Never maps an API error to "unregistered". Test suite: `scripts/ci/tests/test-runner-available.sh`. |

## `check-vcs-version-not-bare-sha.sh` invariants

`core/include/meson.build` builds `VMAF_VERSION` from `git describe`, and
upstream Netflix/vmaf spells that call with `--always`. Fork deliberately
does not. With `--always`, git exits 0 even with no reachable `v*.*.*` tag,
prints bare abbreviated object name. Meson writes that into
`vcs_version.h` verbatim — so `vmaf --version`, JSON/XML `version` field
and `vmaf_version()` all report commit instead of version, on any shallow
checkout, tarball export, or worktree whose `.git` is a file.

Three properties load-bearing; this gate enforces each:

| Property | Why it matters |
| --- | --- |
| No `--always` in the `vcs_tag` command | It is what suppresses the non-zero exit that the fallback path depends on. |
| An explicit `fallback:` | Meson would default it to `meson.project_version()`, but the fallback *is* the tagless path here; spelling it out keeps the intent across meson upgrades. |
| `--match 'v*.*.*'` retained | Without it any tag in the repository can supply the version. |

Two things make defect easy to reintroduce, hard to notice. Conflicts
with upstream on every sync, so mechanical "take theirs" resolution restores
`--always`. Invisible until seven-character abbreviation happens
to contain no ASCII digit — about one commit in a thousand — only
condition `core/test/test_output.c::test_vmaf_version` can detect. Assume any
version-string failure on one leg environmental until checked
whether checkout could reach a tag.

`.github/workflows/build.yml` must therefore keep `fetch-depth: 0` on its
checkout: `git describe --long` needs both tag objects and commit
distance to them; `actions/checkout` default of 1 supplies neither.

## Calibration table contract (ADR-0234)

`gpu_ulp_calibration.yaml` = single source of truth for
per-GPU-generation tolerance overrides on cross-backend parity
gate. Lookup contract:

1. Caller passes `--gpu-id <runtime_id>` to gate. ID format
   follows Research-0041:
   - `vulkan:0xVVVV:0xDDDD`
   - `cuda:M.m`
   - `sycl:0xVVVV:DRIVER`
2. Loader picks most-specific glob match (longest non-
   wildcard prefix wins; trailing `*` supported).
3. If row has `features:` override for cell, that wins.
   Else gate falls back to built-in
   `FEATURE_TOLERANCE` default (preserving backward compatibility
   for every caller pre-dating ADR-0234).
4. If `--gpu-id` omitted, no calibration consulted at all
   (legacy behaviour exact).

**Invariant**: `tolerance_for(feature, gpu_id, default)` returns
`default` whenever any resolution step above falls through.
Enforced by `test_calibration.py`. Future PR
"optimising" lookup must keep all four fallback paths intact, or
existing CI lanes not passing `--gpu-id` will silently change
behaviour.

## When adding a new lane

1. New `--feature` value → add to `FEATURE_METRICS` in *both*
   gate scripts (single source of truth lives in parity gate;
   per-feature script mirrors it). Add workflow step to
   `tests-and-quality-gates.yml`.
   Existing compatibility names not always `feature + suffix`:
   ADR-0586 renamed Vulkan integer ADM to `integer_adm_vulkan`, so both
   scripts must keep `BACKEND_EXTRACTOR_ALIASES[("adm", "vulkan")]`.
   ADR-0662 routes lavapipe motion parity through
   `BACKEND_EXTRACTOR_ALIASES[("motion", "vulkan")] =
   "integer_motion_vulkan"` because the legacy `motion_vulkan`
   compatibility extractor stays explicit-name only.
2. New backend → extend `BACKEND_SUFFIX`, `BACKEND_DEVICE_FLAG`,
   `BACKEND_DEFAULT_DEVICE` in both scripts.
3. New GPU arch → add row to `gpu_ulp_calibration.yaml`. Mark it
   `status: placeholder` until real-hardware corpus exists;
   placeholder row operationally no-op (empty `features:`
   block).

## When updating from upstream

`scripts/ci/` fork-introduced; nothing here merges from
upstream. Risk on `/sync-upstream` = opposite: upstream
change to feature extractor's emitted-metric names would silently
invalidate `FEATURE_METRICS` rows. Re-run matrix gate after any
upstream sync touching `core/src/feature/`.

## PR-body deliverables validator (`validate-pr-body.sh`)

`scripts/ci/validate-pr-body.sh`, `scripts/git-hooks/pre-push`, and
`scripts/git-hooks/pre-push-pr-body-lint.sh` = local mirrors of
`.github/workflows/rule-enforcement.yml` deep-dive-checklist gate
(ADR-0108). Re-use `scripts/ci/deliverables-check.sh` verbatim as
parser; validator only injects diff via `PATH`-shim that
intercepts `git diff --name-only`.

`pre-push-pr-body-lint.sh` = standalone entry point referenced by
the `.pre-commit-config.yaml` `validate-pr-body` hook (`stages:
[pre-push]`). The omnibus `pre-push` hook delegates to same
validator logic. Its `gh` lookup is time-bounded; unavailable credentials fall
back to the public PR list and page. Only a confirmed no-open-PR result skips
validation. Indeterminate metadata fails closed. Preserve
`test-pre-push-pr-body-lint.py` in both commit and push hooks so a locked keyring
cannot restore the unbounded hang or an authentication failure bypass.

**Invariant — single parser source of truth**: do not fork or
re-implement deliverables-check parsing logic in any other
language. If gate's regex shape ever changes, change lands
in `deliverables-check.sh`, validator picks it up
automatically. Test harness `test-validate-pr-body.sh` should
catch any drift between validator's expectations and
parser's actual behaviour.

**Invariant — shim scope**: `git` shim built inside
`validate-pr-body.sh` intercepts only `diff --name-only` call
shape. Every other `git` invocation falls through to real
binary. Future change to `deliverables-check.sh` using
different git subcommand to compute diff must update shim.
Else `validate-pr-body.sh` silently uses real
git's output — potentially fine, potentially wrong depending on
local repo state.

## PR-body stdin classification (`pr-body-input.sh`)

**Invariant — hard sibling-file dependency.** Four scripts source
`scripts/ci/pr-body-input.sh` unconditionally, before any input branch, each
resolving it from its own `${BASH_SOURCE[0]}` directory rather than from
`$PWD`: `deliverables-check.sh`, `validate-pr-body.sh`,
`ffmpeg-patches-surface-check.sh`, `state-md-touch-check.sh`. None of the four
is standalone any more. Consequences to preserve:

- Copying, vendoring or relocating one of those scripts **must** carry
  `pr-body-input.sh` with it, into the same directory. A copy that loses the
  sibling does not degrade — it dies at the `.` line before it reads anything.
- `pr-body-input.sh` is sourced, never executed, and defines only
  `pr_body_*` functions and the `PR_BODY_STDIN_KIND` / `PR_BODY_STDIN_FD`
  variables. Do not give it top-level side effects: it runs inside four gates
  that have already set `set -euo pipefail` and their own `trap … EXIT`.
- The rebase-sensitive surfaces table above lists these scripts against their
  workflow jobs. A workflow that invokes one of them by path is invoking two
  files; a checkout or artifact that ships only the named script is broken.
- `scripts/ci/.shellcheckrc` sets `external-sources=true` **because** of this
  dependency. The pre-commit `shellcheck` hook passes only the staged files, so
  staging one of the four without the helper made ShellCheck emit SC1091 ("not
  specified as input") on a file that is fine. `external-sources` makes it
  follow the `# shellcheck source=` directives the four already carry — more
  analysis, not less: measured over all 57 `scripts/ci/*.sh` checked one at a
  time, 4 findings before and 0 after, with nothing new introduced and no
  effect on the 120 shell scripts outside this directory. Keep the directive
  lines when editing these scripts, and do not reach for
  `# shellcheck disable=SC1091` instead.

**Invariant — never test `[ ! -t 0 ]` for "was a body piped".** That asks
whether fd 0 is a terminal, which is true for `/dev/null`, for a CI step's
null stdin, and for a *closed* descriptor. On a closed fd 0
`PR_BODY="$(cat)"` does not fail — it **deadlocks**, because the command
substitution's pipe takes the freed descriptor 0 and `cat` reads the pipe it
is writing to. All four gates hung this way; measured at `timeout 12` → 124.
New gates that read a PR body call `pr_body_classify_stdin` instead.

**Invariant — classify, read, close.** `pr_body_classify_stdin` hands the
caller a *duplicate* of fd 0 in `PR_BODY_STDIN_FD`; the duplicate is what
makes the subsequent read safe, since no later command substitution can claim
a descriptor already in use. The caller must release it with
`pr_body_close_stdin` after reading. That release cannot move inside
`pr_body_read_stdin`: callers invoke it as `"$(pr_body_read_stdin)"`, and an
`exec {fd}<&-` in a subshell closes the subshell's copy while the caller's
stays open — inherited by every `git`, `python3` and `mktemp` the gate spawns
afterwards.

**Invariant — an absent body means different things to different gates.**
`deliverables-check.sh` / `validate-pr-body.sh` exist only to parse a body, so
no body is exit 2. `ffmpeg-patches-surface-check.sh` /
`state-md-touch-check.sh` also have a diff, so no body makes their opt-out
sentinel unclaimable and they fall through to the diff check. Do not
"harmonise" the two families: turning the latter pair into exit 2 makes a
missing body abort the gate instead of enforcing it. They deliberately also
differ on `$PR_BODY` precedence — `+x` (set counts) for the first pair, `-n`
(non-empty) for the second, because only the first pair reports the two cases
differently.

`scripts/ci/tests/test-pr-body-input-selection.sh` pins all of the above for
all four gates, bounded by `timeout` so a re-regression reports a hang rather
than becoming one. It is wired as a pre-commit/pre-push hook whose `files:`
pattern lists every gate; adding a fifth caller means adding it there too.

## assertion-density.sh — copyright-grep scope (ADR-0968)

`assertion-density.sh` identifies fork-added files by scanning first
20 lines of each `.c` / `.cpp` for Lusoris copyright marker. Grep
pattern **must** accept both the legacy format (`Lusoris and Claude
(Anthropic)`) and the current post-rebrand format (`Copyright YYYY
Lusoris`). Current pattern:

```text
grep -qE "(Lusoris and Claude|Copyright [0-9]+ Lusoris)"
```

**Invariant**: do not simplify this to single literal string.
2026-05-27 copyright-rebrand decision (memory: `project_copyright_lusoris_only`)
dropped "and Claude (Anthropic)" from new files; older files in-tree still
carry legacy form. Grep matching only one format causes script
to silently exit 0 ("no fork-added files found; skipping"), bypassing
assertion-density gate for all files carrying other format.

Test coverage: `scripts/ci/tests/test-assertion-density.sh` (T1–T6).

## Coverage Gate ratchet (ADR-0922)

`scripts/ci/coverage-check.sh` (absolute floors) and new
`scripts/ci/coverage-delta-check.sh` (per-PR delta gate) tightly
coupled to `.github/workflows/tests-and-quality-gates.yml` and each
other. Rebase-sensitive invariants:

1. **Floors one-way.** `OVERALL_MIN` (70), `CRITICAL_MIN` (90), and
   every `PER_FILE_MIN` value may raise in any PR; lowering any of
   them requires new ADR explicitly superseding ADR-0922, cited
   inline at changed threshold. Change-control comment
   above each `PER_FILE_MIN` row carries citation; do not delete
   those comments when editing table.
2. **Delta-gate tolerances default to 0.5pp.** Two CLI flags
   (`--max-overall-drop`, `--max-file-drop`) exist for workflow to
   pin values explicitly; do not tighten beyond 0.5pp without first
   confirming gcov hit-count variance has fallen (current floor of
   variance ~0.2pp, see ADR-0922 alternatives table).
3. **Workflow coupling.** The `Compute base-branch coverage for delta
   gate` and `Enforce coverage-delta gate (ADR-0922)` steps in
   `tests-and-quality-gates.yml`'s `coverage` job require:
   - `actions/checkout` with `fetch-depth: 0` (delta gate runs
     `git merge-base HEAD "$BASE_REF"` — shallow clone breaks it).
   - `gcovr>=8.0` installed in runner (same dependency as
     `coverage-check.sh`).
   - `coverage:` job's `if:` predicate still gates on draft-PR
     status (ADR-0331 self-hosted-runner economy convention applies
     even for hosted CPU lane, to avoid wasted base-coverage builds
     on draft PRs).
4. **Grace window.** PRs opened before 2026-05-31 exempt from
   new floors and delta gate through 2026-06-30 (operational, not
   enforced in code). After 2026-06-30 workflow can drop any
   remaining grace-related notes.
5. **Upstream sync impact.** Upstream Netflix/vmaf has no coverage
   gate, so `/sync-upstream` cannot conflict with these files. Only
   risk: upstream-introduced source file lands without
   any tests, drags overall coverage below OVERALL_MIN floor. Sync
   PR itself then trips gate; resolution = add tests in same PR
   (preferred), or land ADR-0922 supersede ADR first (only if
   structurally impossible).

## Pre-commit hook hygiene — no submodules (ADR-0893)

`.pre-commit-config.yaml` ships upstream `forbid-new-submodules`
hook. Fork pulls upstream Netflix/vmaf code via `subprojects/`
(Meson wraps with sha256 pinning) and `ffmpeg-patches/` (out-of-tree
patch series), **never** via `.gitmodules`. Submodule entry would
bypass:

- wrap-pin sha256 enforcement,
- CycloneDX SBOM walk (inspects `subprojects/*.wrap`, not
  `.gitmodules`),
- and license-allow-list audit.

Adding new third-party dependency -> use Meson wrap
(or vendor it under clear "Vendored 3rd-party" banner, with
attendant `.semgrepignore` / `check-copyright` exclusion). Do not
work around `forbid-new-submodules` hook with `--no-verify`.

**Pinned-revision audit cadence**: re-audit `.pre-commit-config.yaml`
revisions roughly every ~6 months or when CI surfaces deprecation
warning. `pre-commit autoupdate` = starting point only; verify
each proposed bump against `git ls-remote --tags --refs <repo>`.
Autoupdate heuristic has known sort-order bug on repos that
land point releases out of branch order (suggested a
`gitleaks v8.30.1 → v8.30.0` downgrade during ADR-0893 audit).
Alpha pre-releases (`X.Y.Za<N>`) never acceptable pin.

## CI impact planner (ADR-1140)

- `plan-ci-impact.py` + `.github/ci-impact.json` decide which surfaces change
  touches; every required job runs it first, gates heavy steps on
  selectors. **Fail-closed**: unknown top-level paths, non-additive
  statuses (delete/rename/copy), CI-authority files (this directory included),
  missing merge-base, non-linear pushes and over-large diffs all yield
  `mode=full`.
- Any file under `scripts/ci/` = CI-authority input: changing one forces
  `full` mode for that PR by design.
- `tests/test_ci_impact.py` (stdlib `unittest`) pins map ↔ tree contract and
  no-path-filter invariant on required-context workflows. Run it after
  adding top-level directory or required check.

## tidy-ratchet.py invariants (ADR-1142)

- `scripts/ci/tidy-baseline-<lane>.json` generated only by `tidy-ratchet.py --write`
  (or `make tidy-ratchet-write`); never hand-edit count. Baseline may only
  decrease; raising number to make CI green = policy violation, not fix.
- Dedup key `(path, line, column, check)` and NOLINT rule ("cited" =
  `ADR-NNNN` on previous, same or next line, or anywhere in
  `/* ... */` block comment holding marker; `NOLINTEND` never counts) =
  load-bearing: baselines measured with exactly these rules, so
  changing either requires re-measuring every lane in same PR (`cpu`
  full baseline = CI's own `tidy-ratchet-cpu` artifact; ADR-1243 permits
  only guarded scoped tightening afterward).
- `Tidy Ratchet` job starts unconditionally, gates its
  work on ADR-1140 planner's `c_core` selector; `.clang-tidy`, this
  directory (ratchet + baselines) and workflow = CI-authority inputs, so
  editing any of them forces `mode=full`, lane runs. Never add
  `paths:` filter or custom early-skip probe to job.
- `clang-diagnostic-error` in any TU = measurement failure (exit 4), never
  zero. Build (generated headers) before measuring.
- **Measure on the lane's own toolchain, not the workstation's.** A full
  `--write` records what the measuring host sees, so a host whose libc/compiler
  differs from the lane bakes that host's diagnostics into the baseline. Seen
  on 2026-09-22: on gcc-16/glibc the `assert()` expansion makes
  `misc-static-assert` fire in `core/src/dict.cpp` (+1) and
  `core/src/feature/feature_collector.cpp` (+3) — neither file had changed —
  while the same tree on the lane's gcc-15 `Ubuntu 15.2.0-16ubuntu1` with
  clang-tidy 22.1.8 showed zero increases. `cc_version` in the baseline names
  the compiler to reproduce; the ratchet warns when the measuring compiler
  differs, and that warning means "stop", not "commit anyway". Reproduce the
  lane locally with the Ubuntu 26.04 dev image plus `clang-tidy-22` from
  apt.llvm.org, configured exactly as the workflow does
  (`CC=gcc-15 CXX=g++-15 meson setup build core -Denable_cuda=false
  -Denable_sycl=false -Db_lto=false`). The build directory must be `build`
  inside the repo: the 18 generated `build/src/*.json.c` model TUs are
  baseline entries, and a build directory outside the tree silently drops
  them from the measurement.
- **arm64 lane = cross lane (ADR-1283).** Build dir configured with
  `build-aux/aarch64-linux-gnu.ini`; nothing else in the tree compiles
  `core/src/feature/arm64/` or the `ARCH_AARCH64` bodies of `core/test/`, so
  no other lane's compile database holds them. `TIDY_RATCHET_EXTRA_arm64`
  must keep `--extra-arg=--target=$(AARCH64_TARGET)` and
  `--extra-arg=--sysroot=$(AARCH64_SYSROOT)`: drop the target and clang-tidy
  parses `<arm_neon.h>` / `<arm_sve.h>` as x86 and every NEON TU is exit 4;
  drop the sysroot and libc resolves against the host. `exclude_untidyable()`
  in `lint-and-format.yml` still excludes `^core/src/feature/arm64/` — that
  job's CPU-only `build/` genuinely has no command for those files; this lane
  is where they are measured.
- **Scoped writer (ADR-1243):** `--only` plus `--write` requires exact nonempty
  measured-TU coverage, original tool version/lane, no observed debt
  increase. Preserve every unselected TU/header entry and all full-report
  metadata; append explicit scoped provenance. Validate report/baseline aliases
  before output, replace validated baseline atomically. Full and scoped
  writers share resolved-path advisory lock; preserve baseline-drift checks
  around measurement/replacement, fail on unreadable NOLINT inputs. Diagnostic
  `--only` run is not full comparison. Keep failure/zero-tightening cases in
  `tests/test_tidy_scoped_write.py`; never make CI's full lane use `--only`.
- Promoted clang-tidy checks (`-warnings-as-errors`) remain counted debt. Only
  recognized promotion exit/summary may bypass nonzero-tool-exit guard;
  parse/compile failures still invalidate that measurement. Reports retain
  actual `measured_sources` and `compile_failures` so partial/error output
  never presented as successful whole-tree scan.
- ADR-1113's Pelorus mirror and ADR-1276's manifest-owned boundary are outside
  native-lint ownership.
  `pelorus-mirror-paths.txt` is the single exact-path exemption set consumed by
  the sync guard, format hooks, changed-file tidy gate, and `tidy-ratchet.py`;
  do not restore prefix/directory classification. Keep one shared
  `is_exact_pelorus_mirror()` predicate inside the ratchet for TU selection,
  header diagnostics, and legacy-baseline normalization. A scoped baseline
  write must preserve historical entries for this excluded scope; only a full
  generated write may remove them. Fix mirror diagnostics in Pelorus and
  re-pin; never edit the fixture or raise a baseline locally. Tests live in
  `test_pelorus_mirror.py`, `test_tidy_ratchet.py`, and
  `test_tidy_scoped_write.py`.

## Pelorus mirror provenance gate (ADR-1113, ADR-1276)

`tests/test-sync-pelorus-interop.sh` proves the top-level mirror guard fails
closed for a plain directory and for a Git checkout lacking the exact pin. It
reconstructs source fixtures in disposable repositories, clears inherited
`GIT_*`, disables caller Git configuration, and proves the canonical fixture
prefix, tracked-path allowlist, and final-newline comparisons fail closed while
a synthetic re-pin/update refreshes every banner. The fixture uses a portable
Python byte rewrite, not platform-specific `sed -i`. Keep it wired into
required Pre-Commit through `.pre-commit-config.yaml`.

The real CI check belongs in the existing `Pre-Commit` job in
`lint-and-format.yml`: derive the 40-character pin from
`scripts/sync-pelorus-interop.sh`, check out `VMAFx/pelorus` at that object,
then run the script's default mode. Never replace the object with a branch/tag
or restore its working-tree fallback; a green check must bind every complete
rendered mirror and the exact tracked lint-exemption set to reviewed source
bytes.

## release-pr-exempt.sh invariants (ADR-1151)

- Predicate = `release-please--` head ref **AND** bot author. Never relax
  it to head-ref-only: four gates it disarms = required contexts, so
  head-ref-only test would let anyone skip them by naming branch
  `release-please--anything`.
- Always exits 0, communicates through `exempt=true|false`. Gate
  consuming it must use step-level `if:` so job still **reports**.
  Skipping whole job makes check *absent*, which aggregator's
  absent-means-pass rule (ADR-0313) cannot tell apart from path-filter skip.
  That ambiguity is exactly what `mustReport` list exists to close.
- Only four authoring-discipline gates may consult it: Deliverables
  Checklist, Doc-Substance Gate, `docs/state.md` Gate, FFmpeg-Patches Surface
  Sync. `Release Script Contract` and `ADR Collision Guard` stay armed on
  release PRs. Former = gate proving cut ran; also runs
  `tests/test-release-pr-exempt.sh`, so exemption's own test can never
  be skipped by exemption.
- New gate added to aggregator's `required` array must be checked against
  release PR's shape (`.release-please-manifest.json` + coordinated
  version-marker diff, and rendered-changelog body) before promoted.

## Adding a Renovate-managed surface (ADR-1152)

`tests/test_renovate_file_patterns.py` validates positive file-selection
fixtures for custom managers. `managerFilePatterns` regexes have one slash
delimiter at each end; doubled delimiters silently select no files despite
passing Renovate's schema validator. Keep base-image custom manager's
config-plus-mirror set paired with built-in Docker manager exclusions.
`test-renovate-file-patterns` pre-commit hook runs these fixtures whenever
Renovate configuration or test changes.

`classify-dependency-pr.sh` exempts bot PR only when **every** changed path
matches its allowlist — one unmatched path fails whole PR; bot cannot
write deliverables checklist to recover. New dependency-pinning
surface appearing in tree (new chart under `deploy/helm/`, new compose
file, new container build file outside `docker/`) -> add it to
`is_allowed_dependency_path` **and** add fixture case to
`test-classify-dependency-pr.sh` in same change.

`build-config.env` allowance = exact root-path match (ADR-1231).
Never replace with env-file glob or basename match: nested build
configs and unrelated runtime env files must still fail path condition.

Two invariants test suite pins deliberately — do not "simplify" them away:

- Widening allowlist must never drop conjunction with condition (a).
  Human-authored PR touching allowlisted path must still be gated.
- Bot PR touching allowlisted path **and** source code must still be
  gated. That asymmetry = entire point of gate.

Derive additions from what Renovate edits (`gh pr list --author
app/renovate` and diff the file lists), not from what looks like manifest —
see [`docs/research/1152-dependency-classifier-surface-audit.md`](../../docs/research/1152-dependency-classifier-surface-audit.md).

## check-silent-revert.py invariants (ADR-1284)

Gate reports what merge removes from target that branch never set out to touch.
Four load-bearing properties. Drop one, gate becomes decoration.

1. **Measure merge result, not branch tree.** `merge_result_tree()` runs
   `git merge-tree --write-tree base head` — tree that squash merge commits.
   `git diff base..head` is no substitute: reports every file target changed
   and branch never touched. Red on any behind branch.
2. **Intent excludes merge commits.** `branch_intent()` unions diffs of
   `git rev-list --no-merges merge_base..head`. Conflict resolution is not
   branch work. Resolutions taken against target are what `dropped` and
   `resurrected` hunt. Re-adding `--merges` makes every bad resolution
   self-justifying. Both detectors then check surviving tree — `dropped` needs
   line absent from merged blob, `resurrected` absent from base blob. Drop that
   check and both fire on diff-alignment artefact: insert text above line,
   cumulative diff re-pairs line as delete plus add, no single commit diff shows
   pair.
3. **Workflow resolves live target tip.** `Silent-Revert Guard` fetches
   `github.event.pull_request.base.ref`, uses its tip. Never `base.sha` —
   `base.sha` records base branch at PR open, and defect class is target
   moving afterwards.
4. **Fail closed.** Unresolvable ref, no merge base, merge that does not
   resolve cleanly, git without `merge-tree --write-tree`: all exit non-zero.
   Never print `clean` for case gate could not analyse.

Only opt-out is declaration in PR: `revert:` title, `reverts: #N`,
`intentional revert: <reason>`. No in-tree suppression. `GENERATED_PREFIXES`
covers rendered files only; never widen to source trees.
`is_evidence()` drops conflict markers — `0c494cca0` committed three into
`core/src/feature/cuda/integer_vif_cuda.c`, and PR deleting them reset file to
pre-marker blob.

Regression: `python3 scripts/ci/tests/test_check_silent_revert.py` (12 tests).

## check-aggregator-names.sh invariants

- Gates 1:1 parity between required status checks declared in
  `.github/workflows/required-aggregator.yml` (`const required = [...]`) and
  `# required-aggregator` markers on `name:` fields across workflow files.
- Enforced locally via `make lint-sh` and pre-commit hook `check-aggregator-names`.
- Display names must stay concise ($\le 30$ chars) per `docs/development/ci-job-names.md`.

## Self-hosted SYCL Arc runner invariants (ADR-1177)

Intel Arc A380 self-hosted runner executes hardware-in-the-loop SYCL parity tests
under `.github/workflows/sycl-parity.yml`. Following invariants load-bearing:

1. **Untrusted fork PR execution prohibition**: `sycl-parity.yml` must strictly enforce
   `if: github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository`.
   Fork PRs must NEVER execute arbitrary workflows or code on self-hosted infrastructure.
2. **Device isolation**: Container passthrough (`dev/docker-compose.runner.yml`)
   restricted to `/dev/dri/renderD129` (Intel Arc A380, vendor `0x8086`, device `0x56a5`,
   PCI `03:00.0`). Host NVIDIA RTX 4090 and AMD iGPU device nodes must NOT pass into
   container under any circumstances.
3. **Container security posture**: Runner container runs as unprivileged user
   `runner` (uid 1001, gid 1001) in groups 988 (`render`) and 984 (`video`). No Docker socket
   (`/var/run/docker.sock`) mounted. Container resource limits capped at 8 CPUs and 16 GB RAM.
   Ephemeral mode (`--ephemeral`) ensures clean environment per job without state persistence.
4. **Lane-switch contract**: `required-aggregator.yml` lists `SYCL Parity (Arc A380)` as required,
   reads `vars.SYCL_ARC_RUNNER_ENABLED` (makes no runner API call — `GITHUB_TOKEN` cannot list
   self-hosted runners):
   - Lane disabled (variable unset / not `true`): absent or skipped accepted as pass.
   - Lane enabled: job MUST report `success`; absent or skipped (probe failed because
     runner unregistered, offline, or probe token rejected) = loud aggregator failure.
   Never reintroduce auto-detect probe that treats API error as "unregistered" — makes
   required check silently green.
5. **Probe token**: `check-runner-available.sh` runs runner-list query only while lane
   enabled, with `secrets.SYCL_RUNNER_PROBE_TOKEN` (fine-grained PAT, single repository,
   Administration: read-only). Do not widen workflow's `permissions:` to replace it —
   no `administration` scope there.
6. **Render node resolved, not hard-coded**: `dev/docker-compose.runner.yml` takes
   `ARC_RENDER_NODE` from `dev/scripts/arc-render-node.sh` (exactly one vendor-`0x8086` render node).
   Do not replace with bare `renderD<N>`; numbers change after PCI re-enumeration.

## FFmpeg patch lifecycle (ADR-1240)

`ffmpeg_patch_stack.py`, local `ffmpeg-patches-apply-check` hook and
`ffmpeg-patch-stack.yml` share one release owner: `build-config.env`.
Replay `series.txt` cumulatively, fail on fetch/replay/configuration drift,
write only after whole candidate succeeds. Disposable Git must discard
inherited `GIT_*` repository variables and caller Git configuration. Discovery
scheduled, accepts only stable tags; ordinary checks use reviewed tag.
Required aggregator name = exactly `FFmpeg Patch Stack`.

Fixture setup and assertions obey same isolation rule as production
replayer. `test_ffmpeg_patch_stack.py`, `test_ffmpeg_patch_smoke_safety.py`
and dependency-classifier shell fixture discard inherited `GIT_*` before
their first Git command, disable caller global/system Git configuration.
Never rely on `git -C` alone. `test_git_fixture_isolation.py` runs those
fixtures plus agent-cleanup fixture with disposable caller variables,
checks byte-for-byte metadata/work preservation, remains registered in
pre-commit/pre-push and required Pre-Commit CI. Poison only fresh temporary
caller paths; never export real repository's Git paths into test.

Level Zero fixture setup and its checker subprocess use same Git isolation.
Preserve real linked-worktree hook regression: Git itself exports `GIT_DIR`,
so clean parent shell insufficient. Old-command control may mutate
only disposable caller; fixed helper must preserve every shared Git and
linked-worktree file, including both indexes and staged/unstaged work.

## Shared envtest installer (ADR-1231)

`setup-envtest.sh` = executable consumer of envtest tool/version
fields in `build-config.env`. Both Make and Go CI call it; keep Go module
metadata check, direct GOBIN/first-GOPATH executable path, and installed-only
`path`/`env` lookup. Only `install` may fetch assets; inherited
`ENVTEST_USE_ENV` must not bypass configured selection. Do not restore
`@latest`, PATH-existence acceptance or second Kubernetes default in CI.
Preserve install/asset failures and shell-quoted export output; keep
`tests/test_envtest_single_source.py` wired to commit/push checks. See
[Research-2058](../../docs/research/2058-envtest-version-owner.md).

## Tidy ratchet counts headers via an absolute-path-safe filter (ADR-1265)

`.clang-tidy` `HeaderFilterRegex` starts `(^|/)`. clang-tidy matches ABSOLUTE
paths; a `^core/` anchor matches nothing, headers vanish as "non-user code",
ratchet reports 0 header findings forever. That was the state before ADR-1265.

Symptom of regression: `Tidy Ratchet` says every `*.h` went `N -> 0`, asks to
tighten. Do not tighten; restore the `(^|/)`.

CPU baseline = CI's `tidy-ratchet-cpu` artifact (clang-tidy 22, ubuntu-26.04),
never a local run with another clang-tidy. GPU lanes: local `make
tidy-ratchet-write LANE=<cuda|hip|sycl>`, advisory.

## check-state-md-rows.sh — the status token belongs to the section (ADR-0165)

Three independent checks, all of them widened only after a narrower version
reported a dirty file as clean. Do not narrow any of them.

1. Duplicate bug id. Matches four id shapes (`**T-ID**`, `T-ID`, `**T7-16**`,
   `Netflix/vmaf#NNN`) and anchors on the token that OPENS the first cell, not
   on the whole cell — most rows carry a description after the id.
2. Verbatim repeated row, for the ~143 prose-led rows that carry no id.
   Normalises away `_(verified YYYY-MM-DD: ...)_` before comparing.
3. Section against status. A row's status cell — the column the table header
   calls `Status`, else the last non-empty cell — must agree with the level-2
   heading the row sits under whenever the token OPENING that cell is a status
   word: `closed` / `fixed` / `resolved` / `done` only under
   `## Recently closed`, `open` only under `## Open bugs`.

Check 3 exists because checks 1 and 2 only see a *duplicate*. A resolved row
left under `## Open bugs` with no second copy is invisible to both, and reads
as an open bug forever; 24 of 62 rows were in that state on 2026-09-21.

Invariants for check 3:

- The judged cell is the `Status` column when a table header names one and the
  last non-empty cell otherwise, and the token read is the word that OPENS it.
  Requiring the whole cell to BE a status token caught `| fixed |` and skipped
  `| fixed (PR #1425) |` — the same misfiled row, one parenthetical later —
  along with 20 other live rows. Reading the leading word leaves the shapes
  that make no claim unjudged: a verification date or a parenthetical leads
  with no word at all, a branch name leads with `fix` rather than `fixed`
  (`fix/...`, `ci/...`), prose leads outside the vocabulary. Widening the
  vocabulary to guess at those fabricates failures — eight live rows end in a
  branch name. Cells are split on unescaped `|` only: `\|` inside an inline
  code span is the escaped pipe the renderer shows, not a boundary, and
  splitting on it shifts every later cell by one.
- Check 3 is a floor on this class of drift, not a proof of its absence. It
  reads one cell per row, so a status it does not recognise — buried mid-cell,
  in a column that is neither the last nor headed `Status`, or spelled outside
  the vocabulary — is passed over in silence and the file still reports clean.
- Of its two silent-disable paths, it fails closed on ONE. If a row claims a
  status whose owning section heading is absent, the gate errors rather than
  passing over rows that have become ungated, so renaming `## Open bugs` or
  `## Recently closed` breaks the build on purpose. The other path — a status
  cell the extraction does not recognise — is uncovered, and is the likelier
  of the two, since it needs one row edit rather than a heading rename. This
  file and ADR-0165 both asserted that the check fails closed on its *one*
  silent-disable path; that was false when written and the claim is corrected
  here rather than left standing.
- The fix for a hit is always to MOVE the row. Rewriting the status to match
  where the row landed is the failure, dressed up as the repair.

Header rows are excluded the same way for all three checks: a `|---|---|`
separator retracts the record for the line immediately above it, and only when
that line is itself a table row — `prev` and `prevline` both reset on a
non-table line. A separator whose header was lost to a dropped rebase hunk
otherwise retracts the status of the last data row above the blank line, which
silently unjudges a real row.
