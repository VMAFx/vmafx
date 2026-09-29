<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1362: The SYCL integer ADM twin computes AIM on the device and finalises every ADM output in the CPU's float arithmetic

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: `sycl`, `adm`, `aim`, `adm3`, `gpu`, `performance`, `numerics`, `fork-local`

## Context

`integer_adm_sycl` had no AIM contrast-masking pass, so it left
`VMAF_integer_feature_aim_score` and `VMAF_integer_feature_adm3_score` out of
`provided_features[]` (`T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05`).
The fork's default model `vmaf_v1.0.16_3d0h` reads adm3, so under
`--backend sycl` the ADR-0530 name fallback scored every frame's ADM with the
CPU `integer_adm` extractor while VIF, motion and CAMBI ran on the device. At
3840x2160 that run took 48.4 ms per frame on an Arc B580 with the CLI's
default `--threads 0` (the CPU extractor then runs on the main thread) and
24.3 ms with `--threads 16`. The maintainer's RC3 requirement is that a GPU
twin does no GPU/CPU round trip inside a frame
([ADR-1358](1358-sycl-speed-device-resident-linalg.md) `req`).

AIM is the CPU's second contrast-masking pass with the roles of the two
decoupled signals swapped (`measure_aim` in `core/src/feature/integer_adm.c`):
the masking threshold comes from the CSF-weighted restored part `r`, the
measured signal is the additive impairment `t - r`, and the pass adds no noise
floor. The CUDA twin ([ADR-0746](0746-cuda-integer-adm3-aim-parity.md))
recomputes the whole 3x3 neighbourhood of `r` inline; the Metal twin stores the
filtered `r` band. The contract for this port: every ADM output bit-exact with
the CPU `integer_adm` on real content at 8 and 10 bits and at odd and tiny
sizes (maintainer decision, 2026-09-29: "bit-exact with the CPU is the
contract", which covers adm2 and `integer_adm_scale*` as well as aim and
adm3), and no fp64 instruction in the translation unit
([ADR-0220](0220-sycl-fp64-fallback.md)).

## Decision

We give `integer_adm_sycl` a device AIM pass and claim both features again:

- The decouple / CSF kernel stores, next to the DLM neighbourhood band
  `|csf(t - r)| / 30`, the AIM neighbourhood band `|csf(r)| / 30`
  (`d_csf_f_aim`), over the whole band.
- The reduction kernel becomes one work-group per row of the reduction region
  that computes all three orientation bands of the CSF denominator, the DLM
  contrast measure and the AIM contrast measure: nine sums per work-item,
  reduced per row, each row total rounded once through
  `adm_cm_round_row_total()` ([ADR-1167](1167-adm-cm-row-level-rounding.md))
  and added to a `[term][scale][band]` int64 buffer. `r` and `t - r` are
  recomputed per sample as before. The buffer is zeroed and read back once per
  frame by the existing pre- and post-graph callbacks, so a frame is still one
  upload, one graph replay and one small copy back, with no host wait in
  between. The kernel uses sub-group size 16.
- The Q15 decouple quotient `k = t / o` is clamped in 64 bits, as the CPU's
  `tmp_k` is. The old kernel narrowed it to 32 bits first, which wrapped at
  scales 1-3 whenever `|t / o| > 2^16`.
- On the host, every output -- adm2, `integer_adm_scale*`, the debug num /
  den values, aim and adm3 -- is finalised with the CPU's own float arithmetic
  (float per band and per scale, summed in double), which makes them the CPU's
  bits. The twin's former double finaliser, which left adm2 and the scales up
  to 2.9e-7 from the CPU, is removed.
- The twin gains the CPU's `adm_skip_aim` option.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Recompute the 3x3 neighbourhood of `r` inline, as CUDA does (ADR-0746) | No extra device buffer | Nine decouples per orientation per sample in the AIM threshold; the decouple (three reciprocal-table gathers, an int64 angle test) is the expensive part of this kernel | More device work than one stored band |
| A separate AIM reduction kernel after the DLM one | Smallest change to the existing kernel | Re-reads both band sets and redoes the decouple; four more launches per frame | Fusing into one pass reads and decouples each sample once |
| Keep the old shape, three work-groups per row (one per band), and add AIM to it | Smaller diff | Each of the three work-groups decoupled all three bands of every sample, so AIM would triple its cost too | One work-group per row does the decouple once |
| Store `r` from the decouple kernel instead of recomputing it | The reduction skips the decouple | Three more full-band buffers (about 25 MB at 4K) and loads; `adm_sycl` alone at 4K took 49.3 ms per frame on the UHD 770 against 45.6 recomputing | Slower on the bandwidth-bound iGPU |
| Sub-group size 32 (the old kernel's) | Unchanged launch shape | Nine live int64 sums spill on Xe-LP: 59.5 ms per 4K frame on the UHD 770 | 16 is as fast on the B580 and faster on the UHD 770 |
| Keep the twin's double finaliser for everything, aim and adm3 included | No host change | Every output lands up to 2.9e-7 from the CPU | Misses the bit-exact contract |
| Float finaliser for aim and adm3 only, double for adm2 and the scales | adm2 / scale outputs keep their previous values | Two finalisers in one file; adm3 would be built from a DLM ratio that differs from the emitted adm2 | Rejected by the maintainer: bit-exact is the contract for every output |

## Consequences

- **Positive**: under `--backend sycl` the default model's ADM runs on the
  device. With the default model on the Arc B580, 3840x2160 drops from 48.4
  to 9.2 ms per frame at `--threads 0` and from 24.3 to 9.7 at `--threads 16`
  (CPU backend, 16 threads: 26.5 to 32.1 across runs).
  aim and adm3 equal `--backend cpu` bit for bit on every frame of the
  Netflix 576x324 pair, BBB 3840x2160 (50 frames), 853x480 and 17x17 crops
  and 10-bit input, with default and default-model options, on the B580 and the
  UHD 770, and so do adm2 and `integer_adm_scale0..3`. Before, the old twin's
  `integer_adm_scale2` was up to 1.40e-6 from the CPU at 4K (the clamp) and
  every DLM output up to 2.9e-7 (the double finaliser).
- **Negative**: on the UHD 770 the default model is slower: at 4K 75.7 to
  87.5 ms per frame at `--threads 0` and 40.5 to 79.4 at `--threads 16`, at
  576x324 9.3 to 10.8 and 8.5 to 11.0. Before, the CPU computed ADM beside
  the iGPU (for several frames at once with 16 threads); now the iGPU does it.
  SYCL on that iGPU was and stays slower than the CPU backend. The twin needs
  one more full-band int32 buffer per orientation (about 25 MB at 4K). With a
  non-integer `adm_enhn_gain_limit` (for example 1.2) aim and adm3 inherit
  the Q31 gain emulation's difference from the CPU's truncated double product
  (1.4e-7 on the Netflix pair), as adm2 always has; every shipped model uses
  1.0 or 100 (`T-SYCL-ADM-FRACTIONAL-GAIN-LIMIT-2026-09-29`).
- **Neutral / follow-ups**: the HIP twin still has no AIM pass; the row stays
  open for it with this design as the reference. The fork's A380 snapshots
  (`testdata/scores_sycl_a380_*.json`) are not regenerated: they need that
  device, print six decimals, and are already recorded as a broken run
  (`T-SYCL-A380-SNAPSHOTS-MOTION-ZERO-2026-09-21`). `test_sycl_adm_parity`
  asserts every ADM output bit-exact under the default model's options and
  `test_sycl_adm_tiny_frames` on tiny frames, noise, 16-bit input and all four
  CSF modes; `python/test/gpu_default_model_test.py` expects the SYCL twin to
  emit aim and adm3.

## References

- `req` (maintainer, 2026-09-29, recorded in ADR-1358): "there shouldnt be any
  gpu cpu rountrips".
- `req` (maintainer decision relayed on PR #1625, 2026-09-29): "switch adm2 (and
  the scale outputs if the same finaliser covers them bit-exactly) to the CPU
  float finaliser — bit-exact with the CPU is the contract".
- [ADR-0746](0746-cuda-integer-adm3-aim-parity.md) (CUDA AIM pass),
  [ADR-0530](0530-hip-feature-flag-promotion-and-picture-buffer.md) (the
  `vmaf_get_feature_extractor_by_feature_name()` fallback pass),
  [ADR-1167](1167-adm-cm-row-level-rounding.md),
  [ADR-0155](0155-adm-i4-rounding-deferred-netflix-955.md),
  [ADR-0220](0220-sycl-fp64-fallback.md), [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-1358](1358-sycl-speed-device-resident-linalg.md).
- [Research-1362](../research/1362-sycl-adm-aim-device-pass.md).
