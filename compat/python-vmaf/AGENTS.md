# AGENTS.md — python/vmaf

Orientation for agents working on the Python bindings and the **classic**
(SVM-based) VMAF training / eval harness. Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

- Python bindings around libvmaf (`vmafrc`, `quality_runner`, …)
- The upstream Netflix training / analysis harness (SVM, MOS analysis, plots)
- Fork-local scratch and resource trees relocated from the repo root

Not in scope: tiny-AI training — that lives in [../../ai/](../../ai/AGENTS.md).

```
python/vmaf/
  config.py              # WORKSPACE / RESOURCE constants + env overrides
  workspace/             # classic-harness scratch (gitignored subtrees except placeholders)
  resource/              # example datasets + param files
  matlab/                # MATLAB reference implementations (strred, SpEED, STMAD, cid_icid)
  …                      # bindings + harness modules
```

## Ground rules

- **Parent rules** apply (see [../../AGENTS.md](../../AGENTS.md)).
- **Never commit Netflix golden-score changes.** The Python-side golden
  assertions in [../test/](../test/) are the numerical-correctness gate
  for VMAF — they run in CI as a required status check and are never
  modified by any PR. See
  [ADR-0024](../../docs/adr/0024-netflix-golden-preserved.md).
- **Never commit MEX / compiled MATLAB binaries**: upstream shipped ~53
  `.mexa64` / `.dll` / `.o` / `.lib` artefacts in `matlab/`; these were
  purged on 2026-04-17 and are blocked by `.gitignore`. See
  [ADR-0038](../../docs/adr/0038-purge-upstream-matlab-mex-binaries.md).
  The `.c` and `.m` sources stay — anyone needing the MATLAB path rebuilds
  locally with `mex file.c`.
- **Workspace and resource paths go through `config.py` constants**
  (`WORKSPACE`, `RESOURCE`). Overridable via `VMAF_WORKSPACE` /
  `VMAF_RESOURCE` env vars. See
  [ADR-0026](../../docs/adr/0026-workspace-relocated-under-python.md),
  [ADR-0029](../../docs/adr/0029-resource-tree-relocated.md).
- **Precision**: `result.py` serialises floats at `%.6f` by default,
  matching the CLI (Netflix-compat golden gate). See
  [ADR-0119](../../docs/adr/0119-cli-precision-default-revert.md)
  (supersedes [ADR-0006](../../docs/adr/0006-cli-precision-17g-default.md)).

## Rebase invariants

- **`PyPsnrFeatureExtractor` is the primary class; `PypsnrFeatureExtractor` is a `@deprecated` alias.**
  If a future upstream sync touches `feature_extractor.py` around these classes, verify the hierarchy
  is preserved: `PyPsnrFeatureExtractor(PyFeatureExtractorMixin, FeatureExtractor)` as primary
  (TYPE `"PyPsnr_feature"`), `PypsnrFeatureExtractor(PyPsnrFeatureExtractor)` as deprecated alias
  (TYPE `"Pypsnr_feature"`). Same pattern applies to `PyPsnrMaxdb100FeatureExtractor` /
  `PypsnrMaxdb100FeatureExtractor`. Any upstream commit that renames or removes the `Pypsnr*`
  aliases should be absorbed without touching the `PyPsnr*` primary names — they are what the
  test file asserts against. Tracked: fix/pypsnr-feature-extractor-import PR (2026-05-10).
- **`routine.py::run_test_on_dataset()` only reads bootstrap score keys from bootstrap-capable runners.**
  Normal `VmafQualityRunner` / `PsnrQualityRunner` results do not expose
  `get_bagging_score_key()` / CI95 / all-model prediction fields; keep the
  bootstrap kwargs conditional on the full getter set. The macOS tox lane runs
  `run_testing.py` through those normal runners, so unconditional bootstrap-key
  access regresses the CLI tests before any score assertion executes.
- **Doctests must not depend on NumPy scalar `repr()` or assertion traceback
  details.** NumPy 2 may render scalar results as `np.float64(...)`, and Python
  3.14 appends assert-expression detail to `AssertionError` text. Cast numeric
  scalar examples to `float(...)` or format them, and print only the first
  exception-message line when a doctest is documenting assertion text.

## Governing ADRs

- [ADR-0006](../../docs/adr/0006-cli-precision-17g-default.md) — precision default.
- [ADR-0024](../../docs/adr/0024-netflix-golden-preserved.md) — Netflix goldens (Python-side).
- [ADR-0026](../../docs/adr/0026-workspace-relocated-under-python.md) — workspace relocation.
- [ADR-0029](../../docs/adr/0029-resource-tree-relocated.md) — resource tree relocation.
- [ADR-0030](../../docs/adr/0030-matlab-sources-relocated.md) — MATLAB source relocation.
- [ADR-0038](../../docs/adr/0038-purge-upstream-matlab-mex-binaries.md) — MEX binary purge.
