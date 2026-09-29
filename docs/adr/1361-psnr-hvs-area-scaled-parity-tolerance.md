<!-- markdownlint-disable MD013 MD060 -->
# ADR-1361: Scale the psnr_hvs cross-backend tolerance with the CPU's float-sum length

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: gpu-parity, numerics, ci, testing, fork-local

## Context

The cross-backend gate compares every GPU twin with the CPU extractor at a per-feature absolute tolerance. `psnr_hvs` has 5e-4 dB ([ADR-0191](0191-psnr-hvs-vulkan.md)), which was set from measurements on the 576x324 Netflix pair.

At 3840x2160 the SYCL twin is 8.42e-4 dB from the CPU on the worst of 50 BBB frames (`psnr_hvs_y`; combined `psnr_hvs` 7.63e-4), the same on an Arc B580 and a UHD 770. The gap is not the twin's: master's kernel and the reworked one give bit-identical output, and summing the twin's block partials in double moved it further from the CPU, to 6.4e-3 ([Research-2123](../research/2123-sycl-b580-psnr-hvs-and-tile-halo-faults.md)). It comes from the reference. `calc_psnrhvs()` (`core/src/feature/third_party/xiph/psnr_hvs.c`) adds every masked coefficient error of a plane into one running `float`: N = 64 · blocks_x · blocks_y terms, with blocks_x = ⌊(w − 8) / 7⌋ + 1. That is 241 408 terms at 576x324 and 10 802 176 at 3840x2160. The twin sums 64 terms per block on the device and the blocks on the host, so its rounding error is several times smaller and differently distributed.

A fixed tolerance therefore fails correct twins at 4K, or has to be loosened for every size. The Netflix golden CPU values, the CPU extractor and the twin's arithmetic stay as they are.

## Decision

We will scale the `psnr_hvs` tolerance with the length of the CPU's running float sum, anchored at the geometry where the contract was measured.

**Bound.** Under the standard probabilistic rounding model (rounding errors independent and mean zero; Higham and Mary 2019), a recursive float sum of N terms has relative error |δ| ≤ λ · u · √N with high probability, u = 2⁻²⁴. The score is 10 · log₁₀(1 / mean error), so a relative error δ in the plane sum moves it by (10 / ln 10) · δ ≈ 4.343 · δ dB. The combined `psnr_hvs` mixes the planes' linear sums with positive weights, so its relative error is bounded by the worst plane's; the luma plane has the most terms and sets N. The bound is

  E(N) = (10 / ln 10) · λ · u · √N dB.

**Constant.** λ is fixed by keeping today's contract at the reference geometry: λ = T_ref / ((10 / ln 10) · u · √N_ref) = 5e-4 / (4.343 · 2⁻²⁴ · √241 408) = 3.93. Measured, the twin sits at λ ≈ 0.66 at 576x324 and λ ≈ 0.99 at 3840x2160, so the margin is about 4x at 4K. Under the model, a single sum exceeds λ = 3.93 with probability of order 2·exp(−λ²/2) ≈ 9e-4.

**Tolerance.** T(w, h) = max(T_ref, E(N(w, h))), which equals T_ref · max(1, √(N / N_ref)). It is 5e-4 for 576x324 and every smaller frame, so small inputs neither loosen nor tighten; 1.67e-3 at 1920x1080; 3.34e-3 at 3840x2160; 6.7e-3 at 7680x4320.

**Where.** `area_tolerance_factor()` in `scripts/ci/cross_backend_calibration.py` is the single implementation. Both gates apply it after resolving the reference tolerance: `cross_backend_parity_gate.py` (from `FEATURE_TOLERANCE` or a calibration row) and `cross_backend_vif_diff.py` (from `--places` or a calibration row). The `psnr_hvs` values in `FEATURE_TOLERANCE` and `gpu_ulp_calibration.yaml` remain the 576x324 anchor. The FP16 contract stays absolute. The per-device unit tests (`test_sycl_psnr_hvs_parity` at 256x144 and 960x540, 1e-4) are unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Area-scaled tolerance from the accumulation bound (this ADR) | Follows the error's actual growth; small inputs keep the measured contract; no arithmetic changes | A wrong twin at 4K has more room (3.3e-3) than at 576x324 | Chosen: maintainer decision, 2026-09-29 |
| Emulate the CPU's term order on the host | Closes the gap at every size | Reads back 64 floats per block (about 43 MB per 4K plane per frame) and serialises the sum on the host; the twin would reproduce the reference's rounding, not a better value | Performance cost for no accuracy gain |
| Raise the fixed tolerance to about 1e-3 or 3e-3 | One number | Loosens 576x324, where 5e-4 was measured, and still fails at 8K | Loosens small inputs |
| Accumulate the twin's partials in double | Twin closer to the exact value | Measured: moves it further from the CPU (6.4e-3 at 4K) | The gate compares against the CPU's float sum |
| Fix the CPU reference to sum in double | Removes the drift at the source | Changes the Netflix-derived `psnr_hvs` values and every CPU score the golden gate pins | Golden CPU values are untouchable |

## Consequences

- **Positive**: the gate accepts the correct 4K result (8.42e-4 against 3.34e-3) and still fails a wrong one (1e-2). The tolerance used and its scale factor appear in the gate's source label (`default+area x6.69`).
- **Negative**: the 4K tolerance is 6.7 times the 576x324 one; a twin defect that shows only as a sub-3e-3 dB error at 4K would pass the gate. The 576x324 gate, which runs in CI, is unchanged.
- **Neutral / follow-ups**: the CUDA and HIP `psnr_hvs` twins use the same block-then-plane summation, so the same scaling applies to them; they were not measured at 4K here. Any other feature whose CPU reference accumulates a whole plane in one float can join `AREA_SCALED_FEATURES` with its own term count and an ADR amendment.

## References

- Source: maintainer decision (popup, 2026-09-29): "Area-scaled tolerance (Recommended)" for `T-SYCL-PSNR-HVS-4K-PARITY-GATE-2026-09-29`.
- [ADR-0191](0191-psnr-hvs-vulkan.md) — the 5e-4 psnr_hvs contract; [ADR-0214](0214-gpu-parity-ci-gate.md) — cross-backend gate; [ADR-0234](0234-gpu-gen-ulp-calibration.md) — calibration table.
- [Research-2123](../research/2123-sycl-b580-psnr-hvs-and-tile-halo-faults.md) — the 4K measurements and the double-accumulation experiment.
- N. J. Higham and T. Mary, "A New Approach to Probabilistic Rounding Error Analysis", SIAM J. Sci. Comput. 41(5), 2019 — the λ · √N · u model.
