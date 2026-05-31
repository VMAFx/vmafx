<!-- markdownlint-disable MD060 -->
# Research-0749: Sunset VmafLegacyQualityRunner

**Date**: 2026-05-28
**Author**: Agent (claude-sonnet-4-6)
**Status**: Complete

---

## Summary

`VmafLegacyQualityRunner` is a Python quality-runner class that drove the
pre-2019 VMAF scoring pipeline via an SVM model (`model_V8a.model`) trained
on four float features: vif, adm, ansnr, motion. After PR #38 dropped
`float_ansnr` from the C feature extractor registry, any invocation of the
runner results in a broken or empty result. CI agent T-LEGACY-RUNNER-ANSNR-BROKEN
(surfaced in PR #86) confirmed the runner was unconditionally broken.

---

## What was removed

| Artifact | Location | Lines removed |
|---|---|---|
| `VmafLegacyQualityRunner` class | `compat/python-vmaf/core/quality_runner.py` | ~115 |
| `test_executor_id` (legacy runner version) | `python/test/quality_runner_test.py` | ~10 |
| `test_run_vmaf_legacy_runner` | `python/test/quality_runner_test.py` | ~25 |
| `test_run_vmaf_legacy_runner_10le` | `python/test/quality_runner_test.py` | ~25 |
| `test_run_vmaf_legacy_runner_12le` | `python/test/quality_runner_test.py` | ~30 |
| `test_run_vmaf_legacy_runner_with_result_store` | `python/test/quality_runner_test.py` | ~35 |
| `ResultTest` class (legacy runner backed) | `python/test/result_test.py` | ~155 |
| `ResultStoreTest` class (legacy runner backed) | `python/test/result_test.py` | ~40 |

**Total**: approximately 435 lines of broken code removed.

---

## What was NOT removed

- `VmafFeatureExtractor` Python class — retained. Used by
  `VmafIntegerFeatureExtractor` (which maps to integer-path vmafexec keys),
  `VmafQualityRunner`, `BaggingVmafQualityRunner`, and numerous tests in
  `feature_extractor_test.py`.
- All `VmafIntegerFeatureExtractor` tests — unaffected.
- All canonical Netflix golden tests (`test_run_vmaf_runner` and checkerboard
  tests) — protected and untouched per CLAUDE §1.
- `ResultFormattingTest`, `ResultStoreTestWithNone`, `ResultAggregatingTest`,
  `ScoreAggregationTest` in `result_test.py` — these use `VmafQualityRunner`
  or static fixture files and are unaffected.

---

## Root cause

PR #38 deleted `core/src/feature/float_ansnr.c` and removed
`vmaf_fex_float_ansnr` from the C feature registry. `VmafFeatureExtractor`
in Python maps `"ansnr"` to `"float_ansnr"` in its
`ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT`. When vmafexec no longer emits a
`float_ansnr` attribute, the XML parse produces no entry for that key,
and the SVM scoring step in `VmafLegacyQualityRunner._run_on_asset` either
returns zero scores or raises a `KeyError` depending on call path.

---

## Before / after test counts

| File | Before | After | Delta |
|---|---|---|---|
| `quality_runner_test.py` | 5 removed tests + rest | rest only | -5 tests |
| `result_test.py` | `ResultTest` (2) + `ResultStoreTest` (1) + rest | rest only | -3 tests |

Canonical Netflix golden test (`test_run_vmaf_runner`) status: PASS (unmodified).

---

## Follow-up work

Per `docs/research/0733-feature-importance-audit-2026-05-28.md` Phase 2:

1. Remove `"ansnr"` from `VmafFeatureExtractor.ATOM_FEATURES` and its
   `ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT` entry.
2. Assess `feature_extractor_test.py` tests that use `VmafFeatureExtractor`
   and assert `ansnr` scores — those tests may be broken against the current
   binary.
3. Remove `float_ansnr` from HIP backend scaffolding (`float_ansnr_hip`)
   which still has references in `docs/rebase-notes.md`.

---

## References

- PR #38: drop `float_ansnr` from C backend
- PR #86: CI infra agent T-LEGACY-RUNNER-ANSNR-BROKEN
- ADR-0749: formal sunset decision
- `docs/research/0733-feature-importance-audit-2026-05-28.md`
