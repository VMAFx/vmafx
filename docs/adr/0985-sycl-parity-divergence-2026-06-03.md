<!-- markdownlint-disable MD060 MD013 -->
# ADR-0985: SYCL parity divergence investigation — float_ssim + ssimulacra2 on Arc A380

- **Status**: Proposed
- **Date**: 2026-06-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: sycl, parity, ci, gpu, precision, arc

## Context

The cross-backend parity matrix run on Intel Arc A380 (Research-0730,
2026-05-27) recorded three FAIL rows in the SYCL matrix:

| Feature | Max abs diff | Tolerance | Status |
|---|---:|---:|---|
| `float_ssim` | 2.68e-04 | 5.0e-05 | FAIL |
| `ssimulacra2` | 8.72e-02 | 5.0e-03 | FAIL |
| `float_ansnr` | 1.59e-04 | 5.0e-05 | FAIL |

A follow-up investigation (Research-0985, 2026-06-03) found:

1. **`float_ansnr`**: Stale row. The extractor was removed in PR #38.
   No ANSNR code exists in the tree. This row is closed.

2. **`float_ssim`**: Two-cause divergence. The primary cause is a
   formula difference between the CPU (L×C×S with `sqrt(var_ref*var_cmp)`)
   and all GPU backends (combined Wang 2004 Eq.(13) with `2*covar`).
   This formula difference is intentional; the same divergence is present
   for CUDA and Vulkan. The secondary cause is Arc A380's lack of native
   fp64, which amplifies fp32 accumulation error over the full-frame
   reduction (~566×314 pixels). The same SYCL kernel passes at places=4
   on RTX 4090 and lavapipe.

3. **`ssimulacra2`**: Arc A380 lacks native fp64. The 3-pole IIR
   Charalampidis blur in `ssimulacra2_sycl.cpp` accumulates fp32
   rounding over up to 329 rows of recurrence state per pyramid scale
   (6 scales). The XYB + pooling + combine stages already run on host.
   Divergence is ~17× over the places=2 contract on Arc A380;
   RTX 4090 passes places=2. Cannot be fixed without hardware access.

## Decision

No kernel code change is made in this ADR. The decision is:

1. **Close the `float_ansnr` row** — extractor no longer exists.

2. **Add a `float_ssim_sycl` parity test** (`test_sycl_float_ssim_parity`)
   with places=3 (5e-04) tolerance. This tolerance accommodates both the
   intentional formula difference and Arc A380 fp32 accumulation drift.
   fp64-capable hardware (RTX 4090, lavapipe) passes at places=4.

3. **Add a clarifying comment** to `integer_ssim_sycl.cpp` documenting
   the Wang Eq.(13) combined formula and its divergence from the CPU
   L×C×S path.

4. **Add open tracking rows** to `docs/state.md` for the `float_ssim` and
   `ssimulacra2` Arc A380 calibration gap, pointing to Research-0985 and
   the required follow-up work (device-calibration YAML entries per
   ADR-0234 / Research-0730 §6).

The actual calibration entries and any kernel fix require hardware access
and measurement-driven follow-up PRs. This ADR records the investigation
findings and the immediate no-hardware-required deliverables.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Change GPU float_ssim formula to match CPU L×C×S | Removes formula divergence | Changes GPU output for all hardware (CUDA, Vulkan, SYCL, HIP, Metal); requires matching changes in 5 TUs; adds `sqrt` per pixel to every kernel | Cannot be validated without full cross-backend re-measurement; high-blast-radius change |
| Add Arc A380 device-calibration entry now | Closes the parity gate formally | Requires measurement of `scripts/ci/gpu_ulp_calibration.yaml` impact and a separate ADR with hardware evidence | Deferred to follow-up ADR; this PR cannot run the gate |
| Move ssimulacra2 IIR blur to host | Trivially achieves places=4 | Defeats the purpose of the GPU kernel (all compute on CPU) | Not implemented in this ADR |
| Kahan-compensated IIR summation | Potentially halves accumulation error | Adds 6 FP ops per output; cannot be validated without Arc A380 hardware | Deferred to follow-up with hardware access |

## Consequences

- **Positive**: `float_ssim_sycl` now has a unit test (previously
  completely untested). The formula difference is documented in code.
  The float_ansnr stale row is closed. Two open tracking rows in
  `docs/state.md` prevent the Arc A380 calibration gap from being forgotten.
- **Negative**: None — no kernel changes, no score changes.
- **Neutral / follow-ups**: Follow-up ADR for device-calibration YAML
  entries (Arc A380 `float_ssim` places=3, `ssimulacra2` places=1)
  required before Arc A380 can be promoted to a required CI lane per
  Research-0730 §6.1.

## References

- `req` — task brief: "investigate SYCL parity failures for float_ssim (2.68e-4), ssimulacra2 (8.72e-2), float_ansnr (1.59e-4); verify float_ansnr fully gone; identify divergence source; patch if high-confidence"
- Research-0985: SYCL parity divergence investigation (this companion document)
- Research-0730: Cross-backend parity — Intel Arc A380 (2026-05-27) — original measurements
- ADR-0214: GPU-parity CI gate — tolerance table and promotion criteria
- ADR-0188/ADR-0189: float_ssim GPU kernel, places=4 contract
- ADR-0192/ADR-0201/ADR-0206: ssimulacra2 precision contracts and cbrt-on-host fix
- ADR-0564: integer_ssim_sycl fp64-free precision, places=4-5
