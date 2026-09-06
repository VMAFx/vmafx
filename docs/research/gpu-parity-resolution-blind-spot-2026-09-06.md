<!-- markdownlint-disable MD013 MD060 -->

# Finding two GPU parity bugs by varying one constant (2026-09-06)

## Why this is written down

Two cross-backend defects were sitting in `master` behind green CI. Neither was
found by reading kernels. Both were found by changing a single `#define` in the
test fixtures and re-measuring. The method is cheap and repeatable, so it is
recorded here rather than in the ADRs, which record the decisions.

## The trap: green CI, red hardware

Every CUDA parity test skips cleanly when no CUDA device is visible:

```text
[skip: no CUDA device]
```

GitHub runners have no GPU, so the whole family reported success while, on a
workstation with an RTX 4090, the shipped suite was **16/18**:

| test | delta | gate |
|---|---|---|
| `test_cuda_float_adm_parity` | 4.78e-04 on `adm2` | 1e-4 |
| `test_cuda_ssimulacra2_parity` | 2.62e-03 | 1e-4 |

The first lesson is procedural: a skip is not a pass, and a suite whose
hardware-dependent members always skip in CI needs to be run on hardware
deliberately.

## The method

The fixture size was the only variable. `FIXTURE_W` / `FIXTURE_H` were wrapped
in `#ifndef` guards so the same translation unit could be rebuilt at any size
from `-D`, then swept.

**1. Sweep, then read the shape of the failure.** Holding `W = 256` and varying
`H`, the ADM scale-3 delta was:

| scale-3 height | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 18 |
|---|---|---|---|---|---|---|---|---|---|
| delta | 8.6e-04 | 8.6e-04 | 8.2e-04 | 6.7e-04 | 5.5e-04 | 5.1e-04 | **0.00** | 0.00 | 0.00 |

A hard cliff between 14 and 15 is not float noise; it is a branch. `top` is
`(int)(h * 0.1 - 0.5)`, which is 0 for `h <= 14` and 1 from 15 up — so the
divergence happens exactly when the border crop is zero.

**2. Make a falsifiable prediction and test it.** If a zero crop is the
trigger, shrinking *width* must diverge the same way, independently of height.
Predicted before running: `W <= 224` (scale-3 width `<= 14`) diverges, `W >= 240`
is clean. Result: 192/208/224 → 6.8e-04 / 5.3e-04 / 4.8e-04; 240/256/288 →
3.4e-08 / 0.00 / 0.00. Confirmed, and the "small band" story was now
mechanism, not correlation.

**3. When the delta is resolution-*independent*, stop looking for a size bug.**
The ssimulacra2 delta was 2.62e-03 at 256x144 and 1.90e-03 at 960x540 — larger
at the *smaller* size. That ruled out the whole class the ADM bug belonged to.

**4. Bisect the pipeline by dumping intermediates.** Per-scale accumulators
showed the error concentrated in the 4-norm slots (`e1,e3,e5,e7,e9`) while
every 1-norm slot matched to ~1e-5. That signature — a few pixels wrong, not
all of them — pointed at the buffers, so the buffers were dumped and diffed:

| stage | max abs diff | pixels differing |
|---|---|---|
| linear RGB | 1.8e-07 | 3677 / 36864 |
| XYB | 1.3e-06 | 2807 |
| after blur | 6.6e-06 | 36030 |
| final score | 2.6e-03 | — |

The divergence is born in the *first* stage and amplified by the rest.

**5. Decide which side is wrong before fixing either.** The instinct was "the
GPU kernel is wrong". Forcing the CPU onto its scalar dispatch instead made CPU
and CUDA agree **exactly** (`delta = 0.00e+00`), which proved the entire CUDA
chain — host helpers and kernels — was already correct and the CPU SIMD path
was the outlier. Fixing the GPU would have been fixing the wrong side.

**6. Falsify the convenient explanation.** "GCC contracted the scalar
mul-add into an FMA" was a tidy hypothesis. Rebuilding the whole library with
`-ffp-contract=off` left the delta at 2.62e-03, unchanged — so contraction was
never the mechanism. The real cause was an *explicit* `fmaf()` in the SIMD
paths that five shipped copies never received.

## Why a 1 ULP seed became a 1e-3 score

`ssimulacra2` is ill-conditioned by construction. The edge-diff term computes
`|img - blur(img)|` — a catastrophic cancellation between two nearly equal
quantities — and pooling takes a 4-norm, which weights the few largest
survivors. Any per-pixel perturbation is therefore amplified twice. Treat
"tiny input difference, tiny output difference" as an assumption to verify for
this metric, not a given.

## Reusable checklist

- Run hardware-gated suites on hardware before believing them.
- Sweep the fixture size; a cliff in the delta locates a branch.
- Predict the symmetric case (width vs height) and test it.
- Resolution-independent deltas are a different class from size-dependent ones.
- Dump intermediates and find the *first* stage that differs.
- Establish which side is wrong before changing either.
- Kill the convenient explanation with a measurement.

## References

- [ADR-1204](../adr/1204-adm-cm-edge-clamp-gpu-twins.md),
  [ADR-1205](../adr/1205-ssimulacra2-fma-unification-scalar-and-gpu.md),
  [ADR-1206](../adr/1206-gpu-parity-large-fixture-variants.md).
- [ADR-0214](../adr/0214-gpu-parity-ci-gate.md) — the places=4 tolerance.
