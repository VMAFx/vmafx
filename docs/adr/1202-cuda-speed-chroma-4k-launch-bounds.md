<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1202: GPU SpEED-chroma twins report singularity separately from failure

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: cuda, sycl, hip, correctness, feature-extractor

## Context

At 3840x2160 the CUDA backend returned exactly `0.000000` for
`speed_chroma_u`, `speed_chroma_v` and `speed_chroma_uv`, and the default
model's pooled VMAF came out 3.42 points below the CPU score
(63.733364 vs 67.150063 on the same pair). The same model agrees to within
2e-6 at 576x324. `vmaf` still exited 0, so nothing downstream — the CI
snapshots, the tiny-AI training corpus, a user's own run — had any signal
that the numbers were wrong.

Two independent defects combined to produce that.

The first is a kernel launch-geometry bug local to the CUDA twin. The
backward-substitution launch maps one warp to one linear system and packs
eight warps per block, but the two formulas were inverted: `warps_per_block`
was computed as `ceil(nb/8)` — which is the *block count* — and the block
size was then derived from it, so the block grew with the picture instead of
staying fixed. CUDA caps a block at 1024 threads, so every launch with more
than 256 systems returned `CUDA_ERROR_INVALID_VALUE`. 1080p and 1440p stay
under that bound; 4K does not. The SYCL and HIP twins compute the same launch
correctly and are unaffected.

The second is a contract defect shared by all three GPU twins, and it is why
the first one was silent. The CPU reference in `core/src/feature/speed.c`
uses one integer to mean *singular covariance matrix*:
`solve_covariance_system()` returns `cannot_invert`, `speed_extract_score()`
forwards it, and `extract_fex()` reads it to impute the `uv` score from
whichever chroma channel inverted. The GPU twins copied that imputation
verbatim but not its meaning — their `run_cpu_linalg()` equivalents handle
singularity internally (warn, zero the solution, return 0) and reserve the
return value for hard API failures. So the imputation could never fire for
its stated reason, and a genuine device error was routed into it instead:
one channel failing silently imputed from the other, and both channels
failing fell through to `(0 + 0) * 0.5`, appended three `0.0` scores, and
returned success.

The CPU twin also forces the per-channel score to 0 when exactly one of the
reference and distorted sides is singular, "instead of an inflated score that
may skew the average". No GPU twin implemented that rule either.

## Decision

The GPU SpEED-chroma twins will report singularity through a dedicated
`bool *singular_out` out-parameter and reserve their return value for hard
failures, which propagate and fail the frame. Imputation keys off the
singularity flag, never off the return value, and each twin adopts the CPU
rule that a channel with exactly one singular side scores 0. The CUDA solve
launch fixes the block size at eight warps and grows the block count.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Out-parameter for singularity, errors propagate (chosen) | Matches the CPU contract exactly; a device failure fails the run; imputation finally works | One more parameter on three internal functions | — |
| Clamp the CUDA block size to 1024 and stop there | One-line fix for the reported 4K symptom | Leaves the silent-zero path intact: the next device error is still swallowed, on all three backends | Fixes the instance, not the class. The launch bug was only discoverable because someone diff'd CPU against CUDA at 4K by hand |
| Return a distinguished positive value (e.g. `+1`) for singular, negative for errors | No signature change | Every caller must remember the sign convention; `err \|=` accumulation elsewhere in these files would corrupt it | Too easy to get wrong silently, which is the failure mode being fixed |
| Make singularity a hard error on GPU and drop imputation | Simplest control flow | Diverges from the CPU twin: a singular chroma channel is a legitimate, survivable numerical condition upstream handles by imputing | Cross-backend parity is the fork's contract; a GPU-only failure on input the CPU scores is a regression |
| Fail the frame when any launch fails, but keep appending scores | Preserves output shape | An exit-0 run with wrong numbers is the exact defect being removed | Silent wrong output is worse than a loud failure |

## Consequences

- **Positive**: CPU and CUDA agree at 4K (67.150063 vs 67.150063 on the pair
  above; SYCL 67.150065). A CUDA, SYCL or HIP failure inside SpEED-chroma now
  fails the frame instead of emitting `0.0`. The singular-matrix imputation
  the twins claimed to implement actually runs.
- **Negative**: input that previously produced a silent `0.0` score on a
  broken device now errors out. That is the intent, but it converts a
  quiet wrong answer into a visible failure, which will surface as new
  errors on hardware that was already failing.
- **Neutral / follow-ups**: the CUDA twin was the only one with the launch
  bug; the contract fix applies to all three. A cross-backend 4K parity case
  belongs in the GPU parity suite — the existing parity tests all run below
  the 256-system threshold, which is why none of them caught this.

## References

- `core/src/feature/cuda/speed_chroma_cuda.c`, `core/src/feature/hip/speed_chroma_hip.c`,
  `core/src/feature/sycl/speed_chroma_sycl.cpp`
- CPU contract: `solve_covariance_system()`, `speed_extract_score()` and
  `extract_fex()` in `core/src/feature/speed.c`
- [ADR-1199](1199-cuda-picture-handover-barrier.md) — the prior CUDA
  correctness fix in the same read path
- CUDA C Programming Guide, "Thread Hierarchy": a thread block may hold at
  most 1024 threads on every currently supported compute capability
- Source: found while auditing the "CUDA 4K throughput collapse" claim in
  epic #1245; per user direction to keep working the 1.0.0 blockers
