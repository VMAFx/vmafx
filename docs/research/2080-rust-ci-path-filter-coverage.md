<!-- markdownlint-disable MD013 MD060 -->
# Research-2080 — required-workflow path-filter closure

- **Status**: Completed
- **Date**: 2026-09-24
- **Author**: lusoris
- **Governing issue**: canonical `BUG-098` / `T-PATH-FILTERS-WEAKEN-NEW-GATES-2026-09-22`
- **Governing ADRs**: ADR-1297 (every reporting check is required), ADR-0313 (required-check aggregator), ADR-1140 (CI impact planner)
- **ADR status**: no new ADR; this restores the already-decided required-check and fail-closed impact-routing contracts

## 1. Problem statement

The required-check aggregator accepts a required context that never reports as not
applicable. That is necessary for workflows that genuinely do not apply, but it made
workflow-level `paths:` and `paths-ignore:` filters a silent authorization boundary:
one missing glob could suppress a required check completely.

The first confirmed instance was Rust. `vmafx-sys` binds
`core/include/libvmaf/libvmaf.h`, while `rust-ci.yml` did not start for public C
header changes. Adding that one missing glob closed the known symptom but left the
unsafe mechanism in place.

The repository already had a test intended to ban workflow-level filters from every
workflow that hosts a required context. Its regex was:

```python
r"^\s+paths(-ignore)?:"
```

Without multiline mode, `^` matched only the beginning of the complete YAML string.
Since a workflow starts with `name:`, the assertion could not observe any nested
`paths:` key and passed falsely.

Adding `(?m)` was the red-cap check. It identified exactly seven required-context
workflows with trigger filters:

1. `build.yml`
2. `dev-container-build.yml`
3. `docker-image.yml`
4. `doxygen-public-api.yml`
5. `ffmpeg-integration.yml`
6. `helm-chart.yml`
7. `rust-ci.yml`

Together these workflows publish twelve required contexts. All twelve could
previously be absent rather than green or red.

## 2. Required behavior

Every required-context workflow now uses the same three-stage contract:

1. An unconditional `impact` job checks out full history and runs
   `scripts/ci/plan-ci-impact.py`.
2. A distinctly named `... work` job runs the expensive build only when the selected
   impact output is `true` and the pull request is not a draft.
3. An `if: always()` gate job owns the exact required context name and accepts only:
   `selected=true/work=success` or `selected=false/work=skipped`.

The gate fails if planning failed, if selected work failed or was cancelled, or if the
planner and work job disagree. Therefore a documentation-only change still emits a
cheap successful context, while a routing or execution failure cannot become a
silent absence.

All twelve gate names also live in the aggregator's `strictMustReport` set. Each
workflow now declares both pull-request and master-push triggers, so a missing gate
is a broken registration rather than a legitimate not-applicable result.

The routing is explicit:

| Workflow | Selector | Required contexts |
|---|---|---|
| `build.yml` | `c_core` | `Linux Intel LLVM`, `macOS Clang+Metal`, `Windows MSVC+CUDA (full)` |
| `dev-container-build.yml` | `dev_container` | `Dev Container Build` |
| `docker-image.yml` | `docker_image` | `Docker Image Build` |
| `doxygen-public-api.yml` | `doxygen` | `Doxygen Public API` |
| `ffmpeg-integration.yml` | `c_core` | `FFmpeg Ubuntu gcc`, `FFmpeg macOS clang`, `FFmpeg SYCL` |
| `helm-chart.yml` | `helm` | `helm lint + template` |
| `rust-ci.yml` | `rust` | `vmafx-sys CI`, `cargo-deny` |

The seven workflow files are themselves `full_patterns` in
`.github/ci-impact.json`, so an edit to any routing contract runs every selector.
New `docker_image`, `dev_container`, `doxygen`, and `helm` selectors preserve the
old narrow execution scope without preserving the trigger-level bypass.

Matrix work uses a shared aggregate result. Consequently one failed Build matrix row
fails all three Build gates, and one failed ordinary FFmpeg matrix row fails both of
its gates. This is stricter than reporting a sibling green while the shared required
consumer contract is broken and avoids check-name masking.

## 3. Alternatives considered

| Alternative | Benefit | Failure mode / cost | Verdict |
|---|---|---|---|
| Keep workflow filters and add the currently missing globs | Small diff; no extra planner jobs | Every future dependency edge must be duplicated perfectly in trigger YAML; another omission again becomes a silent pass | Rejected |
| Remove filters and run every heavy job on every pull request | Simplest correctness model | Needlessly schedules CUDA, SYCL, FFmpeg, containers, Rust, Doxygen, and Helm for documentation-only changes | Rejected |
| Always start; route work internally; always emit fail-closed gates | Required contexts always exist while expensive work remains impact-scoped | Adds small planner/gate jobs and explicit workflow structure | **Selected** |
| Treat missing required contexts as aggregator failures | Eliminates this bypass centrally | Breaks intentionally non-applicable contexts across the existing aggregator model and requires a broader policy migration | Deferred beyond this bug fix |

## 4. Executable evidence

- Red-cap: after fixing the multiline anchor, the workflow contract test found the
  seven files listed above instead of passing falsely.
- `python3 -m unittest scripts.ci.tests.test_hiss_replay_contract scripts.ci.tests.test_ci_impact scripts.ci.tests.test_rust_ci_workflow_contract -v`
  passes 41 tests, including exact selector routes, planner/work/gate structure,
  master-push coverage, and strict must-report membership.
- `actionlint` passes all seven modified workflows.
- PyYAML parses all seven modified workflows.
- `bash scripts/ci/check-aggregator-names.sh` reports all 79 configured required
  checks exactly once; matrix work names are distinct from their gate names.

Hosted CI and post-merge verification remain required before BUG-098 is marked closed
on `master`.
