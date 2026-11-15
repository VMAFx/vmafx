<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 MD060 -->
# `scripts/ci/` — agent invariants

Generated index. Pick area for touched paths (both if 2 fit); match `Touching`,
read each matching page first. Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../docs/development/agents-index.md)).

Parent: [../AGENTS.md](../AGENTS.md).

Fork-local CI utilities invoked from `.github/workflows/*.yml`.
Upstream Netflix/vmaf has no equivalent tree; rebase risk =
workflow drift, not merge conflict.

## Rebase-sensitive surfaces

Workflow coupling: rename or signature change must land with
matching update in same PR. Required-status-check names derive
from workflow `name:`; dropped or renamed check blocks every PR
until master fixed.

## When updating from upstream

Fork-introduced; nothing merges from upstream. Risk on
`/sync-upstream`: upstream change to feature extractor emitted-metric
names silently invalidates `FEATURE_METRICS`. Re-run matrix gate
after upstream sync touching `core/src/feature/`.

## CI impact planner (ADR-1140)

- Any file under `scripts/ci/` = CI-authority input: changing one forces
  `full` mode for that PR by design.

## Areas

| Area | Read first | Covers |
| --- | --- | --- |
| Gates | [gates](AGENTS-gates.md) | Required checks, merge gates: PR body + state-row validators, required-checks aggregator, silent-revert + citation checks, agent hooks + dispatch, provenance + scope gates. |
| Release | [release](AGENTS-release.md) | Release + supply chain: dry run, release PR, container + base images, CUDA pin, lock files, Renovate + runners, licence + mirror pins, FFmpeg patch series. |
| Tests | [tests](AGENTS-tests.md) | Parity + test infrastructure: cross-backend parity gate, exact twins, twin drift, golden gate builds, Meson test env, test-suite registry, bench + envtest helpers. |
| Tidy | [tidy](AGENTS-tidy.md) | Static analysis + lint: clang-tidy ratchet + lanes, cppcheck, native lint, assertion density. |
