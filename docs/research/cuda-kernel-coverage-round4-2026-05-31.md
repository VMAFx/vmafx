<!-- markdownlint-disable MD060 -->
# Research Digest — CUDA kernel parity coverage round 4 (2026-05-31)

**Companion to**: [ADR-0956](../adr/0956-cuda-kernel-coverage-round4.md)
**Builds on**: [ADR-0868](../adr/0868-gpu-backend-kernel-coverage.md) (round 1),
[ADR-0886](../adr/0886-cuda-kernel-coverage-round2.md) (round 2),
[ADR-0947](../adr/0947-cuda-kernel-coverage-round3.md) (round 3),
[ADR-0214](../adr/0214-cross-backend-parity-gate.md) (places=4 / 1e-4 gate)

## Context

Rounds 1–3 of the CUDA kernel coverage push (PRs #351, #374, #442)
brought CUDA-extractor parity coverage from 2 of 19 kernels (~11 %)
on `origin/master` to 14 of 19 (~74 %) once all three PRs merge.
Round 4 closes the final five-kernel gap and brings cumulative
coverage to 100 % (19 of 19).

## Kernel enumeration (origin/master, 2026-05-31)

The audit re-runs the round-3 enumeration with one correction: the
total is **19 registered feature extractors**, not 18 — `ssim_cuda.c`
and `integer_ssim_cuda.c` are two distinct translation units (the
first registers `integer_ssim_cuda`, the second registers
`float_ssim_cuda`). Round-3 collapsed them into a single row, which
explains the apparent 18.

| # | Kernel (CUDA reg name) | TU | CPU twin | Status before R4 |
|---|---|---|---|---|
| 1 | `motion_cuda` | `integer_motion_cuda.c` | `integer_motion` | covered (test_cuda_motion3_parity.c, master) |
| 2 | `vif_cuda` | `integer_vif_cuda.c` | `integer_vif` | covered (test_integer_vif_cpu_cuda_parity.c, ADR-0541) |
| 3 | `psnr_cuda` | `integer_psnr_cuda.c` | `psnr` | covered (PR #351, round 1) |
| 4 | `ciede_cuda` | `integer_ciede_cuda.c` | `ciede` | covered (PR #351, round 1) |
| 5 | `adm_cuda` | `integer_adm_cuda.c` | `adm` | covered (PR #374, round 2) |
| 6 | `motion_v2_cuda` | `integer_motion_v2_cuda.c` | `motion_v2` | covered (PR #374, round 2) |
| 7 | `cambi_cuda` | `integer_cambi_cuda.c` | `cambi` | covered (PR #374, round 2) |
| 8 | `psnr_hvs_cuda` | `integer_psnr_hvs_cuda.c` | `psnr_hvs` | covered (PR #374, round 2) |
| 9 | `integer_ssim_cuda` | `ssim_cuda.c` | `integer_ssim` | covered (PR #374, round 2) |
| 10 | `float_psnr_cuda` | `float_psnr_cuda.c` | `float_psnr` | covered (PR #442, round 3) |
| 11 | `float_vif_cuda` | `float_vif_cuda.c` | `float_vif` | covered (PR #442, round 3) |
| 12 | `float_ms_ssim_cuda` | `integer_ms_ssim_cuda.c` | `float_ms_ssim` | covered (PR #442, round 3) |
| 13 | `float_moment_cuda` | `integer_moment_cuda.c` | `float_moment` | covered (PR #442, round 3) |
| 14 | `ssimulacra2_cuda` | `ssimulacra2_cuda.c` | `ssimulacra2` | covered (PR #442, round 3) |
| 15 | `float_adm_cuda` | `float_adm_cuda.c` | `float_adm` | **R4 picks (parity)** |
| 16 | `float_motion_cuda` | `float_motion_cuda.c` | `float_motion` | **R4 picks (parity, 2 of 3 features)** |
| 17 | `float_ssim_cuda` | `integer_ssim_cuda.c` | `float_ssim` | **R4 picks (parity)** |
| 18 | `speed_chroma_cuda` | `speed_chroma_cuda.c` | _(none — CUDA-only)_ | **R4 picks (smoke)** |
| 19 | `speed_temporal_cuda` | `speed_temporal_cuda.c` | _(none — CUDA-only)_ | **R4 picks (smoke)** |

## Why these five

### float_adm_cuda / float_motion_cuda / float_ssim_cuda

The integer-path ADM / motion / SSIM twins are already gated by
round-2 (PR #374). The float-path twins are what the
`vmaf_float_v0.6.1` lineage and every research-time
`--feature float_*` invocation exercises. The CPU and CUDA float
paths are independent translation units, so a SIMD pivot on the CPU
side or a kernel-grid change on the CUDA side could silently shift
scores away from each other without surfacing in any existing gate.

**Feature-surface subtlety for `float_motion`**: the CPU twin emits
three features (`VMAF_feature_motion_score`, `..._motion2_score`,
`..._motion3_score`); the CUDA kernel emits the first two only —
motion3 is the host-side moving-average produced by the integer
`motion_cuda` path (already gated by `test_cuda_motion3_parity.c`,
ADR-0214). The R4 test therefore restricts comparison to motion +
motion2.

### speed_chroma_cuda / speed_temporal_cuda

These are **CUDA-only** — there is no CPU twin emitting the same
`Speed_{chroma,temporal}_feature_*_score` family. The closest CPU
surface, `speed_qa.c` (ADR-0253), computes a different scalar
(`speed_qa = spatial + temporal` aggregated on the CPU).

Three implementation paths were considered:

1. **Port the eigendecomp to a CPU-only reference and parity-test
   against it.** Rejected: the CUDA kernel already calls the
   host-side `speed_internal_*` eigendecomp (ADR-0567), so we'd be
   parity-testing the CUDA path against the same CPU code that the
   CUDA path already invokes — circular.
2. **Smoke test: register, run, assert finite output.** Chosen.
   Catches the high-impact failure modes (NaN/Inf drift from kernel
   grid changes, covariance-matrix degenerate cases) without
   inventing a redundant reference.
3. **Skip the speed kernels entirely.** Rejected: kicks the
   coverage-push backlog a fourth round; the failure modes that the
   smoke test catches are exactly the ones the ADR-0567 design memo
   warns about.

## Fixture sizing rationale

The three parity tests use 256x144 YUV420P 8-bpc (matches round-3
template) — that's large enough for the 5-tap Gaussian + 4-stage
decimation of `float_adm` while still running in single-digit ms per
frame.

The two speed smoke tests need a larger fixture because
`speed_internal_init_dimensions` derives:

```text
operating_w = (w * prescale) >> NUM_SCALES        # NUM_SCALES = 4
truncated_w = (operating_w / DEFAULT_BLOCK_SIZE) * DEFAULT_BLOCK_SIZE
                                                  # DEFAULT_BLOCK_SIZE = 5
num_blocks  = truncated_w * truncated_h / 25
```

At 256x144 luma → 128x72 chroma → operating 8x4 → truncated 5x0 — the
chroma path is degenerate (truncated_height = 0 → `-EINVAL`). At
640x360 luma → 320x180 chroma → operating 20x11 → truncated 20x10 →
4x2 = 8 blocks per channel. That admits a non-singular covariance
matrix and exercises the full kernel pipeline.

For `speed_temporal_cuda` (luma path), 640x360 → operating 40x22 →
truncated 40x20 → 8x4 = 32 blocks. Plenty of degrees of freedom.

## Tolerance budget

The three parity tests use the standard ADR-0214 cross-backend
tolerance of `places=4` (1e-4 absolute delta), same as every prior
round. This is the tightest tolerance the CPU SIMD paths can hold
across compiler versions (icx vs. gcc vs. clang vary in
mul-add-contraction defaults; ADR-0161 documents the
`-ffp-contract=off` carve-out applied to SIMD-test TUs).

The two speed smoke tests have no parity tolerance — they assert only
that the score is finite. The kernel-grid drift they catch is
qualitative (NaN/Inf vs. some finite number); a numeric threshold
would either be too loose (any finite value passes) or arbitrarily
tight (false positives from legitimate kernel improvements).

## Risk register (this PR only)

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Speed-smoke fixture too small → singular covariance → kernel logs warning + zeros score → smoke test reports score = 0.0 (finite, so passes) but masks a real bug | Low | Medium | 640x360 chosen to give 8+ blocks per channel; ADR-0567's "covariance matrix singular" log path zeros the solution rather than the score, and an all-zero solution still produces a finite (non-zero) speed score because the score is the determinant ratio, not the solution vector |
| `vmaf_use_feature("float_motion_cuda")` silently registers a different kernel than expected | Negligible | Low | feature_extractor.c registry test (existing) gates the name → extractor mapping; if the registration is wrong, a different feature would fail to be readable via `vmaf_feature_score_at_index` |
| The 5-tap Gaussian in `float_adm` reflects out-of-bounds at 256x144 because the test fixture is one decimation level shy of the design minimum | Low | Low | `float_ms_ssim_min_dim` (ADR-0153) gates the bottom edge at 176x176 for the larger filter; ADM's 5-tap is safe at 256-anything |
| meson.build conflict with in-flight PRs #351 / #374 / #442 | Medium | Low | R4 appends after the R3 block at the end of the `enable_cuda` block; trivial 3-way merge |

## What this does not cover

- HIP kernel coverage (ADR-0945 round 3 in flight via PR #443)
- SYCL kernel coverage (backlog open; no ADR yet)
- Metal kernel coverage (rounds 2 + 3 in flight via PRs #379 / #447)
- Cross-GPU-vs-GPU parity (CUDA vs. HIP vs. SYCL) — that's
  ADR-0214's domain; the per-kernel CPU-vs-CUDA gates here are the
  building block, not the full matrix

## Decision

Land the 5 R4 tests in one PR. Combined with the 14 existing /
in-flight CUDA gates, this closes the kernel-coverage backlog at
100 % (19 of 19). The follow-up work is on HIP / SYCL / Metal, not
CUDA.

## References

- ADR-0214 — cross-backend parity gate
- ADR-0567 — speed_chroma/temporal real GPU + host-side eigendecomp
- ADR-0868 / 0886 / 0947 — rounds 1–3 of this coverage push
- ADR-0108 — six-deliverables rule
- `docs/research/gpu-backend-kernel-coverage-audit-2026-05-30.md` — original audit
- Source: req (CUDA kernel coverage round 4 — close the last 5 uncovered CUDA kernels)
