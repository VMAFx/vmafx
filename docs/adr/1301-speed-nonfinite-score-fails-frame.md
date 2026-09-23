# ADR-1301: A non-finite SpEED score fails the frame instead of being published

- **Status**: Accepted
- **Date**: 2026-09-23
- **Deciders**: Lusoris
- **Tags**: `metrics`, `speed`, `correctness`, `cross-backend`, `fork-local`

## Context

Every SpEED backend bounded its score with a less-than comparison before
appending it — `MIN(x, max)` on the CPU, an inline ternary on the CUDA, HIP and
SYCL twins, a local `CLIP` macro in `speed_chroma_cuda.c`. Fourteen sites
across seven translation units, all the same shape.

Every comparison against NaN is false, so all fourteen published a NaN as
`speed_*_max_val`: a finite, plausible 1000.0 standing in for a computation
that produced no number. `+Inf` was masked the same way. `-Inf` passed the
comparison untouched and was published as `-inf`.

The cross-backend parity harness asserts `isfinite()` on the score it reads
(`core/test/test_cuda_speed_singular_parity.c`), so the clamp defeated
precisely the check that would have caught it.

**Non-finite is reachable, by two independent paths.**

The first is a real division by zero in the CPU reference.
`matrix_qr_decomposition()` in `core/src/feature/speed.c` divided the
Householder vector by its own norm with no zero guard. A deflated minor can
leave that column exactly zero in float; then `vec[k] += sign * norm` adds 0,
the vector is all-zero, and every lane computes `0.0f / 0.0f`. The NaN
propagates through `Q` into `R`, through the solved system, and out of
`update_entropy()`. `is_matrix_regular()` does not prevent it: that gate reads
a separately computed eigendecomposition and says nothing about a column going
rank-deficient mid-Householder.

`speed_internal.c` — the copy every GPU twin uses, split out under ADR-0964 —
has always carried the guard (`si_householder_qr`, `if (vn == 0.0f) continue;`).
Only the CPU reference lacked it, which made the reference the one backend that
could manufacture this NaN.

The second path needs no bug. The entropy term is
`log2(L_k * var + sigma_nn)`, where `var` is the quadratic form `x^T K x` for
the solved system. That is non-negative only in exact arithmetic; an
ill-conditioned fp32 solve can return it slightly negative. Chroma is offset by
-128 before the covariance, so eigenvalues run to 1e3–1e4, and a `var` of about
-1e-5 already drives the argument negative. `log2f` of a negative argument is
NaN; of exactly zero, `-Inf`.

Two further defects surfaced in the same reading. `speed_temporal_max_val` was
declared, parsed and documented as "larger values will be clipped", and the CPU
reference never applied it — while every GPU twin did, so the reference and its
twins disagreed for any score above the bound. And the three-way `MIN` on the
CPU chroma path is upstream code, so the divergence has to be recorded for the
next rebase.

## Decision

A non-finite SpEED score fails the frame. It is never published.

`speed_internal_clamp_score()` is the single implementation, called from all
fourteen sites:

```c
int speed_internal_clamp_score(double score, double max_val, unsigned index,
                               const char *who, const char *feature, double *out);
```

It checks finiteness *before* comparing, warns naming the extractor, the
feature, the frame and the value, and returns `-EINVAL`. On success it writes
`min(score, max_val)` — the same strict less-than the old clamps used, so every
finite score is bounded exactly as before.

That response is not invented here: `brisque.c` and `y_funque_plus.c` already
warn-and-fail on a non-finite score. This extends the existing convention to
SpEED rather than adding a second one, and putting it in `speed_internal.c`
means the CPU reference and the CUDA, HIP and SYCL twins cannot drift apart
again (HISS-19).

Two things ride along, because leaving either would keep the reference and its
twins disagreeing:

- **The QR gains the zero-norm guard** `speed_internal.c` already had. This
  removes the NaN at its source rather than refusing to publish it. Skipping
  the reflection is what the identity prescribes: `I - v·vᵀ` with `v == 0` is
  the identity, so there is nothing to apply.
- **The CPU temporal path clamps.** It now honours `speed_temporal_max_val`,
  matching the twins and the option's own documentation.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Guard, warn, fail the frame** (chosen) | Already the fork's convention in two extractors; makes the parity harness's `isfinite` assertion meaningful; a caller can tell a failure from a measurement. |
| Publish the NaN unclamped | Honest, but every consumer — the feature collector, the pooled score, the JSON writer — would have to learn to handle it, and a NaN in the pooled VMAF score is a worse failure surface than a failed frame. |
| Clamp non-finite to 0.0 | Swaps one plausible lie for another: 0.0 is a legitimate SpEED score, so it hides the failure just as thoroughly as 1000.0 did. |
| Assert | A no-op in release and an abort in debug — the exact objection `brisque.c`'s comment already raises. |
| Fix only the QR division | Closes the reachable bug but leaves the masking, so the next arithmetic path to produce a NaN is silently published again. |
| Fix each backend in place | Fourteen copies of the same guard is how the fourteen copies of the same defect arose. |

## Consequences

- **Positive**: a frame whose SpEED score is not a number is reported as a
  failure instead of as 1000.0, and `-Inf` no longer escapes unclamped.
- **Positive**: the reachable NaN source in the CPU QR is gone, and the CPU
  reference now matches `speed_internal.c`, which the twins already used.
- **Positive**: `speed_temporal_max_val` does what it says on every backend.
- **Positive**: one implementation for fourteen sites, so a future change
  cannot land on some backends and not others.
- **Neutral for scores**: a finite score clamps exactly as before. The QR guard
  only replaces an undefined `0.0f / 0.0f`; for any non-zero norm the
  arithmetic is unchanged, so no finite result moves.
- **Negative**: `core/src/feature/speed.c` is an upstream mirror and now
  diverges from Netflix in two places (the QR guard, the clamp call). Recorded
  in [docs/rebase-notes.md](../rebase-notes.md).
- **Negative**: a frame that previously produced a wrong-but-finite score now
  fails. That is the intent, but it is a behaviour change for any caller that
  was unknowingly consuming a masked 1000.0.

## References

- req: "lol fix? wtf" — the user, on being told the masking had been found and
  filed rather than fixed.
- [ADR-0964](0964-implement-speed-internal-and-wire-gpu-speed-extractors.md)
  — why the SpEED helpers are shared in `speed_internal.c` rather than
  duplicated per backend.
- `core/src/feature/brisque.c:503-511` and
  `core/src/feature/y_funque_plus.c:768-776` — the existing convention this
  follows.
- Issue #1526 — the same defect class at twelve sites outside SpEED, several of
  which publish a NaN as a *perfect* score.
