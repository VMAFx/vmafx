<!-- markdownlint-disable MD013 MD060 -->
# ADR-0947: CUDA kernel parity coverage — round 3 (float-path twins + ssimulacra2)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude
- **Tags**: testing, cuda, parity, fork-local, gpu-coverage

## Context

The 2026-05-30 GPU-backend kernel coverage audit
(`docs/research/gpu-backend-kernel-coverage-audit-2026-05-30.md`, ADR-0868)
catalogued 18 CUDA feature extractors under `core/src/feature/cuda/` and
found only 1 had a CPU-vs-CUDA parity test on `origin/master`
(`test_cuda_motion3_parity.c`, ADR-0214 gate).

Round 1 (PR #351, ADR-0868) added 2 CUDA parity tests
(`psnr_cuda`, `ciede_cuda`). Round 2 (PR #374, ADR-0886) is in
flight and queues 5 more (`adm_cuda`, `motion_v2_cuda`, `cambi_cuda`,
`psnr_hvs_cuda`, `integer_ssim_cuda`).

That leaves the **float-path twins** and the new `ssimulacra2_cuda` kernel
without a cross-backend assertion gate. The float path is what the
`vmaf_float_v0.6.1` lineage and every research-time `--feature float_*`
invocation exercises; without a parity test, a SIMD or kernel rewrite on
either backend could silently drift the float-path scores away from the
CPU reference and only surface weeks later via a CHUG re-extract diff.

Five kernels remain on the ADR-0886 backlog after rounds 1+2:
`float_adm_cuda`, `float_motion_cuda`, `float_psnr_cuda`,
`float_vif_cuda`, `float_ms_ssim_cuda`, `float_moment_cuda`,
`speed_chroma_cuda`, `speed_temporal_cuda`, `ssimulacra2_cuda`. The
`speed_*` twins use a 25×25 eigendecomp on host that splits the
numerical-equivalence story (see ADR-0567); they are deferred to a
separate ADR with their own tolerance budget.

## Decision

Add 5 fork-local parity tests under `core/test/`, one per kernel,
following the `test_cuda_motion3_parity.c` template (ADR-0214
`places=4` / 1e-4 tolerance, skip-on-no-device guard):

| Test | Kernel | CPU twin | Emitted feature(s) probed |
|---|---|---|---|
| `test_cuda_float_psnr_parity` | `float_psnr_cuda` | `float_psnr` | `float_psnr` (luma) |
| `test_cuda_float_vif_parity` | `float_vif_cuda` | `float_vif` | `VMAF_feature_vif_scale[0..3]_score` |
| `test_cuda_float_ms_ssim_parity` | `float_ms_ssim_cuda` | `float_ms_ssim` | `float_ms_ssim` |
| `test_cuda_float_moment_parity` | `float_moment_cuda` | `float_moment` | `float_moment_{ref,dis}{1st,2nd}` |
| `test_cuda_ssimulacra2_parity` | `ssimulacra2_cuda` | `ssimulacra2` | `ssimulacra2` |

Each test wires into `core/test/meson.build` behind the existing
`get_option('enable_cuda')` guard, registers with suite `['fast', 'gpu']`,
and links the same `(pthread, cuda, math)` dependency triple as
`test_cuda_motion3_parity`.

Post-PR CUDA-extractor assertion coverage rises from **~17 %**
(3 of 18 on `origin/master` — motion3 + the round-1 `vif` parity gate +
the existing `motion3` test) to **~44 %** (8 of 18) once rounds 1+2 land
behind this PR, and **~72 %** (13 of 18) once all three round PRs merge.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Pick `float_*` only (4 kernels) | Smaller PR; one coherent theme | Leaves `ssimulacra2_cuda` (the most newly-landed kernel, highest drift risk) without a gate | `ssimulacra2_cuda` has no CPU-twin parity test and was added recently — covering it now while the design memory is fresh is cheaper than later |
| Pick all 9 remaining kernels (incl. `speed_*` + `float_motion_cuda`) | Closes the backlog in one PR | `speed_*` involves a host-side eigendecomp with its own tolerance budget (ADR-0567 deferred); `float_motion_cuda` overlaps PR #374's `motion_v2_cuda` blend formula | Right-size to ADR-0108's 200–800 LOC bundle; defer `speed_*` to ADR-N+1 |
| Add per-feature option matrices (chroma on/off, ms_ssim's `enable_lcs/db/clip_db`) | Tighter coverage of option combinations | Triples test surface for no observed bug; option matrix has no audit hit | Stick to default options; matrix work is a separate ADR if drift surfaces |
| Defer until rounds 1+2 merge | Avoids meson.build conflicts | Loses parallelism on the kernel-coverage push; round 3 picks distinct kernels so the per-file conflict surface is `core/test/meson.build` only | Conflicts on a single file are trivially resolvable; the parallel work outweighs the conflict cost |

## Consequences

- **Positive**: closes the largest remaining gap in CUDA kernel
  cross-backend coverage; pins the float-path numerical contract that
  every `vmaf_float_*` model depends on; the new `ssimulacra2_cuda` gate
  prevents the kind of silent drift that the round-1 PSNR/CIEDE gates
  caught during 2026-05 audits.
- **Negative**: +5 test binaries (~1000 LOC) to the build matrix; ~5 s
  added to `meson test --suite=gpu` wall time on a CUDA-enabled runner.
- **Neutral / follow-ups**: ADR-N+1 will cover `speed_chroma_cuda` /
  `speed_temporal_cuda` with their own tolerance budget once the
  ADR-0567 chroma-eigen path is reviewed. `float_motion_cuda` is
  deferred to the same follow-up (overlaps PR #374's
  `motion_v2_cuda` blend coverage).

## References

- [ADR-0214](0214-cross-backend-parity-gate.md) — cross-backend tolerance gate (`places=4`)
- [ADR-0868](0868-gpu-backend-kernel-coverage.md) — round 1 (psnr + ciede)
- [ADR-0886](0886-cuda-kernel-coverage-round2.md) — round 2 (adm/motion_v2/cambi/psnr_hvs/ssim)
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six-deliverables rule
- [ADR-0567](0567-speed-chroma-cuda-real-impl.md) — speed_chroma host-side eigendecomp
- `docs/research/gpu-backend-kernel-coverage-audit-2026-05-30.md` — round 1 audit + backlog
- Source: req (CUDA kernel coverage round 3 — extend beyond PRs #351 + #374)
