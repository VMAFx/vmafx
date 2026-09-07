<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1218: The GPU SpEED twins zero the device solution and report singularity from the temporal path

- **Status**: Proposed
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: `cuda`, `sycl`, `hip`, `correctness`, `feature-extractor`, `testing`

## Context

SpEED solves a 25x25 linear system per plane. `is_matrix_regular()` accepts the
covariance matrix only if **every** eigenvalue is at least `1e-6`; anything else
is singular, which the CPU comment at `speed.c:1364-1368` notes happens
routinely — "when the channel is completely flat", i.e. grayscale sources,
solid-colour frames, letterbox and pillarbox bars, static scenes, synthetic
test patterns.

The CPU reference handles it in two places:

1. `solve_covariance_system()` zeroes the **solution** buffer and returns the
   singularity to `est_params()`, which propagates it as `-EINVAL`.
2. `speed_extract_score()` uses that signal:

   ```c
   // If only one of ref and dis was numerically unstable (very rare)
   // we return 0 instead of an inflated score that may skew the average
   if ((err_ref && !err_dis) || (!err_ref && err_dis)) *score = 0.0f;
   ```

   and `speed_chroma`'s `extract()` uses it again to impute `score_uv` from the
   surviving channel.

The GPU twins diverged from both halves.

**Half one — the wrong buffer.** All six twins (`speed_chroma` and
`speed_temporal` on CUDA, SYCL and HIP) responded to a singular matrix by
`memset`-ing the **host** staging buffer `h_indterm` and uploading nothing. The
score kernel reads the **device** solution `d_sol`, which therefore kept the
previous frame's contents — or, on the first frame, whatever the allocator
handed back. `sycl::malloc_device` is explicitly uninitialised, so on SYCL that
is a genuine uninitialised read. The host `memset` was dead code regardless:
`h_indterm` is re-downloaded from `d_indterm` at the top of every pipeline run.

**Half two — the missing signal.** [ADR-1202](1202-cuda-speed-chroma-4k-launch-bounds.md)
(PR #1360) gave the three *chroma* twins a `singular_out` parameter, the
one-sided-zero rule and the u/v imputation. The three *temporal* twins were not
part of that change: `run_cpu_linalg_st()` / `run_channel_st()` returned success
on a singular matrix, so the one-sided-zero rule could not exist there, and the
twin returned the score kernel's inflated result where the CPU returns `0`.

Measured on an RTX 4090 with a 960x960 fixture whose reference frames are frozen
(zero temporal difference, singular) and whose distorted frames keep moving
(textured difference, regular) — exactly one side unstable:

```text
SpEED temporal/one-sided-singular parity FAIL: cpu=0.00000000 gpu=230.71379089
```

## Decision

We will zero the **device** solution buffer on the singular path in all six
twins (`cuMemsetD8Async` / `hipMemsetAsync` / `q.memset`), delete the dead host
`memset`, and extend ADR-1202's `singular_out` reporting and one-sided-zero rule
to the three `speed_temporal` twins so they match `speed_extract_score()`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Zero `d_sol` on the device and report singularity from the temporal path (chosen) | Matches the CPU on both the one-sided and both-sides cases; removes an uninitialised device read | Three more parameters and one branch per temporal twin | — |
| Upload the zeroed host buffer instead of a device memset | Reuses the existing H2D path | A full `25 * num_blocks * sizeof(float)` transfer to write zeros the device can write itself; and it keeps the misleading host `memset` alive | Slower and no clearer |
| Zero `d_sol` once at allocation and rely on the solve never running | One-line change | Only correct while the matrix is singular on *every* frame. As soon as one regular frame writes a real solution, the next singular frame reads it | Fixes only the first-frame case |
| Return `-EINVAL` from the twins on singularity | Smallest signal change | The GPU callers treat a non-zero return as a hard failure and abort the channel without emitting a score; the CPU emits one. Conflating the two is exactly what ADR-1202 had to undo | Contradicts ADR-1202 |
| Leave the temporal twins as they are and document the divergence | Zero code risk | A 230-point score difference on a static scene is not a documentable divergence | — |

## Consequences

- **Positive**: `speed_temporal` on CUDA, SYCL and HIP now returns `0` where the
  CPU returns `0`, instead of a fully inflated score, on any content where one
  side is numerically unstable — static scenes being the common case. The
  device solution is defined on every path.
- **Negative**: any recorded GPU `speed_temporal` score over content with static
  passages is invalid and must be re-measured. No in-tree snapshot covers this:
  the existing SpEED parity fixtures are textured on every frame.
- **Neutral / follow-ups**: the device-zeroing half has no *demonstrable* score
  impact on its own. When both sides are singular the CPU zeroes both solutions,
  every block's variance becomes `0`, `log2f(1 + 0)` is `0`, and
  `get_speed_score()` returns exactly `0` regardless — so a stale solution
  cannot move the number in that case. It is fixed because reading
  uninitialised device memory is undefined behaviour that a sanitizer or a
  dirtier allocator will surface, and because the twin's contract should not
  depend on an allocator handing back zeroed pages.

## References

- Findings 15 / 16 / 17 from the twin-drift sweep: on a singular covariance
  matrix the CUDA / HIP / SYCL twins zero the host independent-term staging
  buffer and skip the H2D upload of the solution entirely, so `d_sol_ref` /
  `d_sol_dis` keep the previous invocation's contents; and the singularity is
  never reported to the caller, so the u/v imputation branch is dead. The
  findings were written before PR #1360; that PR closed the reporting half for
  chroma only, and the device-buffer half for neither.
- [ADR-1202](1202-cuda-speed-chroma-4k-launch-bounds.md) — the chroma-side
  singularity reporting this ADR extends to the temporal twins.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — the GPU parity CI gate and its
  places=4 cross-backend tolerance.
- Fixture note: the existing SpEED parity fixtures are 768x432, whose chroma
  planes yield 4x2 = 8 blocks for a 25x25 covariance — rank-deficient by
  construction, so those tests run the singular path on *every* frame and never
  exercise the regular one. The new tests use 960x960 (36 chroma blocks, 144
  luma) so a regular frame can precede a singular one.
