<!-- markdownlint-disable MD013 MD060 -->
# ADR-1295: Correct sureal's Gaussian density for unanimously rated stimuli in-process

- **Status**: Proposed
- **Date**: 2026-09-22
- **Deciders**: VMAFx maintainers
- **Tags**: `python`, `numerical-correctness`, `dependencies`, `testing`

## Context

ADR-1278 made every warning fatal in the classic Python harness. Three
`routine_test.TestTrainOnDataset` cases then failed, two of them on

```text
sureal/tools/stats.py:21: RuntimeWarning: divide by zero encountered in divide
```

`sureal.tools.stats.vectorized_gaussian` evaluates
`1 / sqrt(2*pi) / scale * exp(-(x - loc)**2 / (2 * scale**2))` with no guard on
`scale`. `MosModel._get_mos_and_stats` calls it with the per-stimulus sample
standard deviation, which is *exactly* zero whenever every observer rated a
stimulus identically. `python/test/resource/raw_dataset_sample.py` contains two
such stimuli by construction — the reference videos, rated `[100] * 5` and
`[90] * 5` — so `scales[0]` and `scales[2]` are `0.0`.

The zero is legitimate input. Unanimous ratings of a reference stimulus are an
ordinary subjective-test outcome, and nothing upstream produced a degenerate
array by mistake.

The warning is not cosmetic. The leading division yields `inf`, the exponent's
`0 / 0` yields `nan`, and `inf * nan` is `nan`; the `numpy.nansum` in sureal's
log-likelihood then **drops** those observations while still dividing by the
full observation count. Measured on that fixture, sureal 0.9.0 reports
`loglikelihood = -1.6932609057879255`, which is the sum over the 10 observations
that survived divided by all 20 — the 10 unanimous ones vanished. AIC and BIC
inherit the error. sureal 0.9.0 is the newest release on PyPI, so there is no
upstream version to upgrade to, and the package exposes no hook through which a
replacement density could be registered.

The third failure is unrelated to sureal and is a plain call-site defect:
`RegressorMixin._plot_one_content` obtains its per-content trend line from
`tools.misc.linear_fit`, a `scipy.optimize.curve_fit` wrapper that also
estimates a parameter covariance. A content group in `dataset_sample.py` holds
two stimuli and a line has two free parameters, so no residual degree of freedom
is left, SciPy fills the covariance with infinities and raises
`OptimizeWarning`. This is *not* a second instance of the ADR-1278 5PL defect:
that one was structural parameter redundancy in a five-parameter model that had
`b1` as an additive intercept alongside `b5`; here the parameters are perfectly
identifiable and only the covariance — which the plot never reads — is not.

## Decision

We will ship a fork-local, zero-scale-safe `vectorized_gaussian` in
`vmaf.tools.stats` and bind it over sureal's own at import of
`vmaf.core.train_test_model`, the single fork module that already hard-imports
sureal and that every subjective-modelling entry point in `vmaf.routine` pulls
in before a model is fitted. Where `scale` is zero the density takes its
Dirac-delta limit — unbounded at `x == loc`, zero elsewhere, `nan` for a missing
rating — so a dataset containing a unanimously rated stimulus reports
`loglikelihood = +inf` (AIC and BIC `-inf`), the honest statement that its
maximum likelihood is unbounded, rather than a silently truncated finite number.
Every finite non-zero scale keeps sureal's expression unchanged and is
bit-identical to it. Separately, the scatter plot fits its trend lines with
`numpy.polynomial.Polynomial.fit` and stops requesting a covariance it neither
can have nor reads.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Rebind a corrected density into sureal from the harness (**chosen**) | Fixes the arithmetic at its cause; non-degenerate results bit-identical; no new dependency; one small function | Patches a third-party module at a distance; covers only code paths that import the fork's harness | — |
| Upgrade `sureal` | Zero fork-local code | 0.9.0 *is* the newest release and still carries the defect | No release to upgrade to |
| Vendor sureal's subjective models into `compat/python-vmaf/` | Full control; no patching | Copies ~1500 lines of a maintained dependency into the rebase surface, for a one-line defect | Cost wildly out of proportion |
| Subclass `MosModel` / `DmosModel` in the harness | Explicit, no monkey-patching | The failing tests pass `sureal.subjective_model.MosModel` in directly, so a subclass is never reached | Does not fix the reported cases |
| Report `nan` for the degenerate scale (preserve today's values) | No number moves anywhere | Keeps the silent truncation: the statistic stays wrong, only quietly | Trades one defect for its symptom |
| Drop degenerate stimuli before fitting | Removes the zero | Discards real ratings and changes the MOS the harness returns — the assertions in `routine_test.py` read those | Changes groundtruth |
| Keep `curve_fit` and pass `absolute_sigma=True` for the trend line | One-word change | Silently redefines what the returned covariance means to suppress the diagnostic | Suppression wearing a fix's clothes |

## Consequences

- **Positive**: `loglikelihood`, `aic` and `bic` stop being computed over a
  subset of the observations; the degeneracy is reported instead of hidden. No
  warning filter, `catch_warnings` wrapper or `filterwarnings` mark was added.
- **Negative**: a third-party module is patched at import time. The comment at
  `compat/python-vmaf/core/train_test_model.py` names this ADR so the next
  reader finds the reasoning, and the binding must be revisited if sureal ever
  ships a fix or moves the symbol.
- **Neutral / follow-ups**: nothing in-tree consumes `loglikelihood`, `aic` or
  `bic`, so no assertion moves; the two trend-line fits agree with the previous
  `curve_fit` result to 7.8e-14 in slope and intercept on the four-point overall
  fit. Fixing the `OptimizeWarning` also unmasked a latent `ValueError` in
  `_plot_one_content`: the HISS-04 helper extraction left `if point_labels:`
  applied to the NumPy slice of the caller's label list instead of to the list
  itself, which is the "extraction, not redesign" invariant in
  `compat/python-vmaf/AGENTS.md`; the predicate is now `is not None`.

## References

- [ADR-1278](1278-python-safe-parallel-execution.md) — fatal warnings and the 5PL correction this is explicitly *not* a repeat of.
- [ADR-0165](0165-state-md-bug-tracking.md) — the `docs/state.md` row accompanying this change.
- [NumPy `nansum`](https://numpy.org/doc/stable/reference/generated/numpy.nansum.html) — treats `nan` as zero, which is what hid the truncation.
- [SciPy `curve_fit`](https://docs.scipy.org/doc/scipy/reference/generated/scipy.optimize.curve_fit.html) — `pcov` is filled with `inf` when the residual degrees of freedom are zero.
- [NumPy `polynomial.Polynomial.fit`](https://numpy.org/doc/stable/reference/generated/numpy.polynomial.polynomial.Polynomial.fit.html) — least-squares fit without a covariance estimate.
