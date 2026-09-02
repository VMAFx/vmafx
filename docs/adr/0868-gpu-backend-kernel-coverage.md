<!-- markdownlint-disable MD060 -->
# ADR-0868: GPU backend kernel parity-test coverage gap-fill

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: tests, cuda, hip, sycl, metal, coverage

## Context

A coverage audit of `core/test/` versus the registered GPU feature
extractors revealed broad gaps. CUDA shipped 14 extractors but only
`motion_cuda` and `vif_cuda` had cross-backend parity tests (the
remaining 12 — `psnr_cuda`, `ciede_cuda`, `cambi_cuda`, `adm_cuda`,
`float_psnr_cuda`, `float_vif_cuda`, `float_adm_cuda`,
`float_motion_cuda`, `psnr_hvs_cuda`, `integer_ssim_cuda`,
`float_ssim_cuda`, `float_ms_ssim_cuda`, `float_moment_cuda`,
`speed_chroma_cuda`, `speed_temporal_cuda` — were unverified at CI
time). HIP shipped 18 extractors with only `motion_hip` and
`adm_hip` parity-gated. SYCL shipped 17 extractors with `motion_sycl`
and `cambi_sycl` covered. Metal shipped 8 extractors with only the
runtime smoke (`motion_v2_metal`) asserting registration.

A regression in any of the uncovered kernels' reductions, separable
filters, or per-plane accumulators would have escaped CI and surfaced
only at downstream model-prediction time, polluting CHUG re-extracts
and vmaf-tune feature exports.

## Decision

We add six cross-backend parity tests plus one Metal registration
audit, picked to:

1. spread coverage uniformly across CUDA / HIP / SYCL / Metal,
2. target the two highest-leverage kernels per backend (PSNR for
   the reduction path, VIF for the separable-filter path,
   CIEDE2000 for the colour-conversion path),
3. mirror the established `test_*_motion3_parity.c` scaffold so the
   review surface is uniform.

The new tests are:

| Test | Backend | Kernel | Tolerance |
|---|---|---|---|
| `test_cuda_psnr_parity` | CUDA | `psnr_cuda` (integer_psnr_cuda.c + psnr_score.cu) | 1e-4 (places=4) |
| `test_cuda_ciede_parity` | CUDA | `ciede_cuda` (integer_ciede_cuda.c + ciede_score.cu) | 1e-4 |
| `test_hip_psnr_parity` | HIP | `psnr_hip` (integer_psnr_hip.c + psnr_score.hip) | 1e-4 |
| `test_hip_vif_parity` | HIP | `vif_hip` (integer_vif_hip.c + vif_statistics.hip) | 1e-3 |
| `test_sycl_psnr_parity` | SYCL | `psnr_sycl` (integer_psnr_sycl.cpp) | 1e-4 |
| `test_sycl_vif_parity` | SYCL | `vif_sycl` (integer_vif_sycl.cpp) | 1e-3 |
| `test_metal_kernel_registration` | Metal | 8 extractor registrations + TEMPORAL flag audit | n/a |

Tolerances follow the ADR-0214 cross-backend gate: places=4 (1e-4)
for unfiltered reductions, places=3 (1e-3) for filtered features
(VIF) where the separable Gaussian's accumulator order changes the
last few bits. Each parity test skips cleanly with a `[skip: no <backend>
device]` notice when the runtime is unavailable, matching the existing
`test_*_motion3_parity.c` skip pattern.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Full coverage in a single PR (~30 tests across the 45+ extractor gap) | Closes the audit in one shot. | 30+ test files in one PR is past the 200–800-LOC sweet spot called out in the PR-hygiene rule; reviewer cost balloons. | Land highest-leverage 6+1 here, leave a follow-up backlog item for the remaining ~38. |
| Synthetic fixtures only (current approach) | Deterministic, no file I/O, runs in <1s per backend. | Doesn't exercise the realistic-content code paths that natural video stresses. | Accepted — same trade-off the existing `test_*_motion3_parity.c` makes. The CHUG / netflix-benchmark sweep covers natural content end-to-end. |
| Bit-exactness (1e-9) tolerance per ADR-0138/0139 | Strongest gate. | GPUs are NOT bit-exact vs CPU per the user-memory rule `feedback_golden_gate_cpu_only.md`; would force false-positive failures. | Use the documented near-exact 1e-4 / 1e-3 places-budget instead. |
| One big merged parity test with subtests per kernel | Fewer executables. | Couples backend availability — a failed CUDA driver init would mask a SYCL regression. Existing tests are one-extractor-per-binary; keep the pattern. | Stay one-binary-per-kernel. |

## Consequences

- **Positive**: 6 GPU kernels gain a cross-backend gate that fires
  on every PR. Future regressions in the reduction / filter
  accumulator on any of CUDA / HIP / SYCL are caught at CI time
  rather than at CHUG re-extract time.
- **Positive**: Metal's 8-extractor registration is now fully
  audited (was 2/8 spot-checked); a future T8-1d refactor that
  drops a `.mm` translation unit will surface immediately.
- **Negative**: 7 new test binaries inflate the GPU CI lane by
  roughly 7 × (driver-init time + 1 frame). On a 4090 / Arc /
  MI300 lane this adds well under 5 s total.
- **Follow-up**: ~38 GPU extractors remain uncovered (full list in
  `docs/research/gpu-backend-kernel-coverage-audit-2026-05-30.md`).
  Track via a `.workingdir2/BACKLOG.md` row tagged
  `gpu-coverage-tier-2`. Not gating on this PR.

## References

- ADR-0214 — cross-backend tolerance budget (places=4 unfiltered,
  places=3 filtered).
- ADR-0361 — Metal backend rollout (T8-1c/d).
- ADR-0420 — Metal kernel-template runtime.
- ADR-0421 — Metal extractor MSL shaders.
- ADR-0108 — six fork-local deliverables rule.
- `feedback_golden_gate_cpu_only` (user memory) — GPU paths are not
  bit-exact with CPU; near-exact tolerance is the correct gate.
- Source: `req` — request to push test coverage on GPU backend
  kernels covering CUDA + HIP + SYCL + Metal feature extractors,
  with the avoid-list (PRs #289, #290, #293, #294, #308, #315) and
  the instruction to wire into `core/test/meson.build` under the
  appropriate suite tags.
