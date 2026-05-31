<!-- markdownlint-disable MD013 -->
# Research-0095 — ATOM_FEATURES port (3dee9666 + 7209110e) blocker

**Date:** 2026-05-10
**Status:** Blocked — port requires C-side `adm_cm` API change, not just Python sync.
**Tracker:** `T-PY-FEXT-ATOM-SYNC` in `docs/state.md` (remains Open).

## Why a simple cherry-pick doesn't work

The Netflix upstream commits the fork is missing:

- `3dee9666` — moves `vif_scale0..3`, `adm_scale0..3`, `adm2`, `adm3`,
  `motion3` from `DERIVED_ATOM_FEATURES` to `ATOM_FEATURES` in
  `python/vmaf/core/feature_extractor.py`. Pure Python.
- `7209110e` — recalibrates ~40 `assertAlmostEqual` values across
  `python/test/feature_extractor_test.py`,
  `python/test/quality_runner_test.py`, and
  `python/test/vmafexec_test.py` to match the new derivation path.

Naive cherry-pick produces 16/65 failures in
`feature_extractor_test.py` — chiefly `KeyError:
VMAF_feature_aim_score` and family. AIM is one of the derived features
that 3dee9666 expects the binary to emit but the fork's binary does
not. That's the blocker.

## What the fork's `adm_cm_s` is missing

Upstream's AIM derivation calls `adm_cm` twice:

1. `adm_cm(decouple_a, csf_a, csf_f, …, noise_weight=0.0, …)` —
   noise-masking *disabled* for the AIM half.
2. `adm_cm(decouple_r, csf_a, csf_f, …, noise_weight=DEFAULT, …)` —
   noise-masking enabled (= the existing DLM-style call).

The fork's `adm_cm_s` (in `core/src/feature/adm_tools.c`) hardcodes
`noise_weight = DEFAULT_ADM_NOISE_WEIGHT = 0.03125` via its internal
`adm_sum_cube_s(csf_a, …)` call — there is no parameter to disable it.

So the AIM-derived `aim_score` cannot be produced by the fork's binary
today; the test recalibrations in 7209110e expect specific AIM values
(e.g., `0.026559020833333336`) that depend on the noise-weight=0
variant.

## What a real port requires

1. **C-side**: extend `adm_cm_s` (and the integer/SIMD/CUDA/SYCL/
   Vulkan/HIP equivalents — that's ~12 sites) to take a `noise_weight`
   parameter. Plumb it through `adm_run_s` → `adm_score` and friends.
2. **Python-side**: the cherry-pick from 3dee9666 + 7209110e then
   becomes plumbing — once the binary emits `aim_score`, the test
   values match.

Estimated scope: ~400–600 LOC across the C tree (one parameter
threaded through ~12 implementations) + the trivial Python
cherry-pick. ADR required (changes a public extractor's output set).

## Why this is deferred, not done in tonight's train

Tonight's train fixed 35 PRs of *bugs* — single-cause, narrow-scope
issues with clear remediation. This port is a *feature port* that
extends the C API surface. It deserves a focused PR with its own ADR,
its own benchmarking pass (does adding a noise-weight parameter
regress ADM scoring perf?), and its own GPU-backend update sweep.

## Acceptance criteria when work resumes

- `python/test/feature_extractor_test.py` passes 65/65 against the
  fork's binary.
- `python/test/quality_runner_test.py` passes excluding any tests
  fork-locally added (e.g., the `assertEqual` version probes bumped in
  PR #729).
- The Netflix golden CPU pairs (`src01_hrc01`, two checkerboard
  variants) still produce within ±5e-05 of their golden assertions
  (CLAUDE §1, §8).
- ADR documenting the `adm_cm` API extension lands alongside the C
  changes.

## References

- Upstream commits: `3dee9666`, `7209110e` on
  <https://github.com/Netflix/vmaf> master.
- Fork-local PRs that touch the same surface and must be preserved on
  any future port attempt:
  - PR #715 (VERSION 0.2.7 → 0.2.21)
  - PR #716 (PyPsnrFeatureExtractor case fix + Pypsnr* deprecated
    subclass)
  - PR #717 (corpus adapter DX)
  - PR #724 (json log roundtrip — numpy 2.x compat)
  - PR #729 (version-string assertion bumps)
- `T-PY-FEXT-ATOM-SYNC` in `docs/state.md`.
