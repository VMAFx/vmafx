<!-- markdownlint-disable MD013 MD060 -->
# ADR-0991: Second-Opinion Batch Materializer — Smoke-Run Scaffold and Test Fix

- **Status**: Accepted
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: ai, second-opinion, materializer, testing, smoke, fork-local

## Context

[ADR-0674](0674-second-opinion-materializer-batch-manifest.md) introduced
`ai/scripts/batch_materialize_second_opinion_features.py` and its test suite.
The implementation is correct; however, two gaps were identified when the PR
(archived as #1497) was reviewed for its first real corpus run:

1. **No committed smoke-run scaffold.** There is no versioned manifest or
   fixture set that an operator can use to verify the script works end-to-end
   against a known input before pointing it at real corpus data. The analogous
   saliency batch runner ([ADR-0673](0673-saliency-materializer-batch-manifest.md))
   also lacks such a scaffold, but the second-opinion path is more frequently
   referenced in signal-mix planning.

2. **`ai/pyproject.toml` missing `pythonpath = ["scripts"]` in pytest
   configuration.** The batch tests import the scripts via
   `importlib.spec_from_file_location`, which triggers `_script_bootstrap`
   resolution at module load time. Without `ai/scripts/` on `sys.path`, all
   three second-opinion batch tests and all four saliency batch tests fail with
   `ModuleNotFoundError: No module named '_script_bootstrap'`. This is a
   pre-existing bug affecting any invocation of `pytest ai/tests/` from the
   repo root without an explicit `PYTHONPATH=ai/scripts` prefix.

## Decision

1. Add `pythonpath = ["scripts"]` to `[tool.pytest.ini_options]` in
   `ai/pyproject.toml`. This fixes the pre-existing failure for both the
   second-opinion batch tests and the saliency batch tests without changing any
   test logic.

2. Commit a smoke-run scaffold under
   `ai/testdata/smoke-second-opinion-batch/`: a `batch.json` manifest, minimal
   synthetic fixture JSONL tables, and a `README.md` with the exact invocation
   and expected output. The manifest outputs to `/tmp/` so no generated
   artifacts are committed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `PYTHONPATH=ai/scripts` to every CI step | No pyproject.toml change | Requires updating every CI job that runs ai tests; leaks an env var concern into infra rather than the package config | Rejected: the cleaner fix is the package declaring its own pytest paths |
| Fix only second-opinion tests, leave saliency broken | Smaller PR scope | Leaves a known CI failure in the saliency batch test suite | Rejected: same root cause, same one-line fix |
| Place smoke manifest in `ai/runs/` | Conventional location for run manifests | `runs/` is gitignored at the root `.gitignore` level, so the manifest would not be committed | Rejected: `ai/testdata/` is not gitignored and is the correct home for committed fixture data |
| No smoke scaffold, rely on unit tests alone | No new committed files | No single "run this to prove it works" command for operators new to the script | Rejected: operator confidence requires an end-to-end smoke path with known fixtures |

## Consequences

- **Positive**: `pytest ai/tests/` from the repo root passes for all batch
  materializer tests without a manual `PYTHONPATH` prefix.
- **Positive**: Operators can verify the second-opinion batch path end-to-end
  with one command and two synthetic tables before pointing the script at real
  corpus data.
- **Negative**: `ai/testdata/` grows by five small files (batch manifest, two
  fixture feature tables, two fixture score tables, README).
- **Neutral / follow-ups**: Run the batch manifest on real scorer sidecars for
  CHUG, KoNViD, UGC, Netflix, and BVI tables, then rerun the signal-mix audit.
  Consider adding the same pytest path fix for any other ai/scripts batch
  runners added in the future (saliency batch runner currently uses the same
  fix path).

## References

- [ADR-0657](0657-second-opinion-feature-materializer.md) — table-side
  second-opinion joiner.
- [ADR-0661](0661-ai-run-manifest-provenance.md) — AI run provenance.
- [ADR-0673](0673-saliency-materializer-batch-manifest.md) — saliency batch
  runner (same pytest fix also applied).
- [ADR-0674](0674-second-opinion-materializer-batch-manifest.md) — second-opinion
  batch manifest design.
- [Research-0991](../research/research-0991-second-opinion-batch-2026-06-03.md)
  — implementation digest.
- Source: req — "Script merged (PR #1497 archived) but no artifacts under .workingdir2/ or runs/. If wired but not run, scaffold a smoke launch."
