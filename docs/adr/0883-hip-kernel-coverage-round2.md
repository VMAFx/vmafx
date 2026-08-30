<!-- markdownlint-disable MD013 MD060 -->
# ADR-0883: HIP kernel parity-test coverage round 2

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: `hip`, `tests`, `gpu`, `coverage`

## Context

PR #351 (ADR-0868) closed the largest cross-backend kernel parity gaps for
CUDA, SYCL, HIP, and Metal — including `psnr_hip` and `vif_hip` for the AMD
runtime.  That left the HIP backend at 4 / 17 extractors with a parity gate
(adm + motion3 from earlier work, plus psnr + vif from PR #351), the lowest
coverage of any GPU backend on the fork.  Per ADR-0214 every GPU feature
extractor needs a synthetic-fixture CPU-vs-GPU parity assertion before it
can be considered production-ready; the gap was tracked as the
"tier-2 follow-up backlog" in
[docs/research/gpu-backend-kernel-coverage-audit-2026-05-30.md](../research/gpu-backend-kernel-coverage-audit-2026-05-30.md).

This ADR documents the second round: 5 additional HIP parity tests covering
the highest-value remaining kernels (CIEDE2000, PSNR-HVS, motion v1, SSIM,
MS-SSIM).  Selection priority was driven by (a) score appearance in the
libvmaf-2.x.x default-model feature set, (b) CHUG re-extract column
coverage, and (c) kernel complexity (filtered features ranked above
unfiltered reductions because they have more failure modes).

## Decision

We will add 5 new HIP parity tests under `core/test/`:

- `test_hip_ciede_parity` — `ciede2000`, places=4
- `test_hip_psnr_hvs_parity` — `psnr_hvs`, places=4
- `test_hip_motion_parity` — `VMAF_integer_feature_motion_score` (v1), places=4
- `test_hip_ssim_parity` — `ssim`, places=3
- `test_hip_ms_ssim_parity` — `float_ms_ssim`, places=3

Each follows the template established by `test_hip_vif_parity.c` (PR #351):
a synthetic 256×144 YUV420P fixture, CPU reference vs. HIP score, skip
cleanly with `[skip: no HIP device]` when `vmaf_hip_state_init()` fails.
Tolerances follow ADR-0214 (places=4 for unfiltered reductions, places=3
for filtered features whose reduction trees accumulate more rounding).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One mega-test for all 5 kernels | Single executable, less meson churn | Granularity loss — hard to isolate which kernel regressed; one skip blocks the others | Rejected; per-kernel split mirrors the established PR #351 template |
| Bit-exact (`places=15`) tolerance | Strongest possible gate | HIP/CPU reductions are *not* bit-exact (memory CLAUDE.md "Golden gate CPU-only; GPUs NOT bit-exact"); guaranteed false-fail | Rejected; places=3/4 per ADR-0214 is the documented contract |
| Cover all 12 untested HIP extractors in this PR | Closes the gap in one go | ~1,800 LOC PR, harder to review; some extractors (speed_*) have unstable CPU paths | Rejected; round-2 covers the 5 highest-value, round-3 sweeps the rest |
| Use real YUVs from `python/test/resource/yuv/` | Realistic scores | Read-from-disk slows the gate; Netflix YUVs are reserved for the golden gate | Rejected; synthetic gradient matches PR #351 template |

## Consequences

- **Positive**: HIP backend coverage rises from 4 / 17 to 9 / 17 parity-gated
  extractors (≈53%), nearly matching CUDA's ratio.  Future kernel
  refactors (e.g. `__shfl_down_sync` → `atomicAdd` migrations like ADR-0539)
  get a CI gate per touched feature.
- **Negative**: 5 additional executables in the `gpu` test suite — total CI
  wall time on the AMD runner grows by ~30 s (each test runs 2 frames
  through libvmaf, container-cached so build is shared).
- **Neutral / follow-ups**: Round-3 backlog (≈7 extractors): `cambi_hip`,
  `ssimulacra2_hip`, `speed_chroma_hip`, `speed_temporal_hip`,
  `float_adm_hip`, `float_motion_hip`, `float_psnr_hip`, `float_ssim_hip`,
  `float_moment_hip`.  Tracked in the same audit doc.

## References

- ADR-0214 — cross-backend parity tolerance gate (places=4 unfiltered, places=3 filtered).
- ADR-0539 — integer ADM HIP atomicAdd reduction pattern.
- ADR-0868 — round-1 GPU kernel coverage gap-fill (PR #351).
- [docs/research/hip-kernel-coverage-round2-2026-05-30.md](../research/hip-kernel-coverage-round2-2026-05-30.md).
- Related PRs: PR #351 (round-1), PR #290 (HIP ssimulacra2 leak), PR #308 (HIP/Metal ENOSYS stubs).
- Source: `req` (operator dispatch 2026-05-30: "Extend HIP kernel test coverage beyond PR #351").
