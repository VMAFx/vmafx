---
paths:
  - scripts/ci/coverage-*.sh
invariant: Floors only rise; lowering one needs ADR superseding ADR-0922; delta-gate tolerance 0.5pp.
---
<!-- markdownlint-disable MD013 MD060 -->
# Coverage Gate ratchet (ADR-0922)

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
3. **Workflow coupling.** `Compute base-branch coverage for delta
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

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `coverage-check.sh` | `tests-and-quality-gates.yml` — `Enforce coverage thresholds` step on both required `coverage` and `coverage-gpu` jobs | CLI shape (`coverage-check.sh <gcovr-summary.json> <overall_min%> <critical_min%>`) and in-script `PER_FILE_MIN` map are gate definition. Every entry in `PER_FILE_MIN` must cite ADR that justifies lower bar ([ADR-0114](../../../docs/adr/0114-coverage-gate-per-file-overrides.md)). Audit cadence + tighten/keep/remove rule codified in [ADR-0881](../../../docs/adr/0881-coverage-overrides-audit-2026-05-30.md). Gcovr's emit-path format (currently `core/src/...` relative to repo root) is join-key with `PER_FILE_MIN`; if future gcovr upgrade changes that format, override silently stops applying and global 85 % gate kicks in — per-line "min XX%" output is canary. |
