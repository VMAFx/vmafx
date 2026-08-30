# ADR-0416: VIF on-the-fly filter sync from Netflix upstream

Status: Accepted
Date: 2026-05-10
Tags: vif, upstream-sync, fork-local, netflix-golden, fork-internal

## Context

Master Netflix-Golden CI was red on `feature_extractor_test.py` after PR #754
reverted PR #723. PR #723 had attempted a partial port of upstream Netflix
commit `bf9ad333` ("libvmaf/feature/vif: switch from precomputed to on-the-fly
filter computation") but only ported the mirror-tap-h fix and a small slice of
the filter-dispatch rewrite. That partial port left the fork bit-exact with
upstream on default-kernelscale `compute_vif` but diverged on non-default
kernelscales, breaking 7 fork-local `test_run_vmaf_runner_float_vifks*` tests
calibrated against the old lookup-table dispatch — so PR #754 reverted #723
entirely. The revert restored the vifks tests but reintroduced 8 fextractor
tests fail with vif_num/vif_den deltas of ~8.4 / ~9.3 on values ~713 K /
~1.6 M (relative drift ~1e-5, places=0 strictness).

Investigation showed Netflix upstream has the *companion* test recalibration
in `142c0671`, `7209110e`, `d93495f5`, and `fe756c9f`. The fork was missing
those test updates because PR #723 only ported the C-side change without the
test sync. The CI failure was therefore a half-port, not a real numeric bug.

## Decision

Sync the VIF on-the-fly filter machinery in full from Netflix upstream and
adopt the companion test recalibrations.

C-side files taken verbatim from `upstream/master`:

- `core/src/feature/vif.c`
- `core/src/feature/vif.h`
- `core/src/feature/vif_tools.c`
- `core/src/feature/vif_tools.h`
- `core/src/feature/vif_options.h`

Public-API change: `compute_vif()` gains an `int vif_skip_scale0` parameter
between `vif_kernelscale` and `vif_sigma_nsq`. The single in-tree non-trivial
caller, `core/src/feature/float_vif.c::extract()`, threads
`s->vif_skip_scale0 ? 1 : 0` through to the new slot. The internal
`compute_vifdiff` recursion in `vif.c` already passes `0` for the new
parameter (upstream pattern).

Test-side cherry-picks from upstream:

- `142c0671` — VIF score values for on-the-fly kernel computation
  (`vif_scale0..3` on the `src01` Netflix CPU pair across multiple test
  variants).
- `d93495f5` — relax `VMAF_legacy_score` / `VMAF_score` tolerance on a
  handful of tests where upstream observed `libm` cross-platform precision
  drift; conflict resolution kept fork-local values where the fork's
  binary diverged from upstream's binary at places=4 (the fork already had
  the relaxation; upstream's assertion text was the change Netflix shipped).
- `fe756c9f` — loosen `vifks360o97` tolerance.
- `7209110e` — update expected `VMAF_score` values in the two
  `test_run_vmaf_runner_motion_force_zero` /
  `test_run_vmaf_runner_with_param_neg_and_model_mfz` tests (97.42843
  → 97.42835); resolution kept fork-side for the
  `feature_extractor_test.py`, `result_test.py`, `vmafexec_test.py`
  conflicts because PR #731's commit message states it already cherry-picked
  `7209110e` for those files.

## Consequences

Positive

- Netflix Golden CI green on the canonical CPU pair. Local-pytest baseline
  delta: master `feature_extractor_test.py` 8 failures → 0 failures;
  `quality_runner_test.py` 9 failures → 0 failures (modulo `skimage`
  module-not-found env issue on `niqe_runner` test, unrelated).
- VIF default-kernelscale and non-default-kernelscale now both bit-match
  Netflix upstream's reference implementation.
- Eliminates the lookup-table `vif_filter1d_table_s` and the
  `resolve_kernelscale_index()` / `ALMOST_EQUAL` machinery — fork no longer
  carries a divergent filter-dispatch path.

Negative / open

- GPU backends (CUDA / SYCL / Vulkan / HIP / Metal) still call into their
  own VIF kernels and were unaffected by this change. The Vulkan VIF
  shader (`float_vif.comp`) already saturates against CPU (ADR-0381) but
  may need a follow-up parity check now that the CPU reference shifted by
  ~1e-5; tracked as a state.md follow-up.
- Two `vmafexec_feature_extractor_test.py` test groups remain pre-existing
  failures (`*_prescale_*` and `test_todataframe_fromdataframe`) — these
  are not introduced by this PR and pre-date PR #754. Tracked separately
  in state.md.

## Alternatives considered

- **Restore PR #723 only (re-revert PR #754).** Rejected: would only flip
  the failing-test set (8 fextractor pass, 9 vifks fail) without
  delivering net green CI. The fork would still diverge from upstream
  for non-default kernelscale.
- **Defer to a focused multi-PR session.** Rejected: master CI is red on
  Netflix Golden today; the sync is well-scoped (5 C files + 4 test
  cherry-picks) and fits a single PR with proper ADR + state.md update.
- **Cherry-pick `bf9ad333` + `8c645ce3` directly** instead of taking
  upstream HEAD versions verbatim. Rejected: the cherry-pick produced
  ~11 conflict regions across 3 files due to fork's incremental
  pre-port history; taking upstream HEAD versions is cleaner and
  avoids cascading conflicts.

## References

- User direction (paraphrased): the user requested the full upstream VIF
  sync via popup option after seeing the partial-fix trade-offs of
  restoring PR #723 alone.
- PR #723 (`8c74ac7d`) — partial bf9ad333 port, reverted.
- PR #754 (`b886ae18`) — revert of #723 that exposed the half-port gap.
- PR #731 (`67b0a1c6`) — ADM noise_weight port; commit message states
  cherry-pick of `3dee9666 + 7209110e` (the non-quality-runner subset).
- Upstream Netflix commits: `bf9ad333`, `8c645ce3`, `142c0671`,
  `d93495f5`, `fe756c9f`, `7209110e`.
- ADR-0381 (Vulkan VIF scale 2/3 precision) — adjacent invariant; CPU
  reference shift may motivate a Vulkan re-check.
