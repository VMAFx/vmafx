<!-- markdownlint-disable MD013 MD060 -->
# Research-2109: SYCL motion-add-UV fixed-point oracle — 2026-09-25

**Status:** Complete

**Authority inspected:** integration head
`84f8ab38133325d90042329e89f2b24719d252e7`, Intel oneAPI DPC++ 2026.0,
Level Zero runtime 1.17.39758, Intel Arc A380.

**Scope:** `motion_sycl(motion_add_uv=true)` test arithmetic and the ADR-1206
960x540 registration. No production kernel, public API, model, snapshot,
Netflix golden assertion, benchmark, tuning, or training change.

## Reproduction

The original test used one absolute `2e-4` budget between CPU
`float_motion` and fixed-point SYCL `motion_sycl`. Rebuilding the same source
at 960x540 and running it on the idle Arc A380 reproduced the tracked failure:

```text
cpu(float_motion)=50.27938271
sycl=50.27915223
delta=2.30e-04
tol=2.00e-04
exit=1
```

The small 256x144 build passed. The failure was deterministic and exercised
the exact source and device path named by
`T-SYCL-MOTION-ADD-UV-TOLERANCE-RESOLUTION-2026-09-06`.

## Arithmetic inventory

The two extractors implement different numerical contracts:

| Stage | CPU `float_motion` | SYCL `motion_sycl` |
|---|---|---|
| Coefficients | `{0.054488685, 0.244201342, 0.402619947, ...}` in float | `{3571, 16004, 26386, ...}` summing exactly to 65536 |
| Vertical pass | float multiply/add | `(sum + 128) >> 8`, producing Q8 |
| Horizontal pass | float multiply/add | `(sum + 32768) >> 16`, retaining Q8 |
| SAD | float row sums followed by float frame sum | exact `int64_t` device reduction |
| Normalization | float division by plane area | host double `sad / 256 / area` |

The semantic option is shared, but CPU float is therefore not an
arithmetic-identical oracle. A fixture-calibrated cross-extractor tolerance
cannot distinguish an expected float reduction residual from coefficient,
border, rounding, UV inclusion, or normalization drift in the device kernel.

## Resolution-independent bound

The scalar test oracle independently implements the fixed coefficients,
reflect-101 index mapping, both rounding stages, exact per-plane SAD, and
YUV420 ceiling geometry. It calculates both Y-only and Y+U+V scores.

For an 8-bit input, a fixed blurred sample is at most `255 * 256 = 65280`.
The public picture allocator limits each side to 32768, so a plane SAD is less
than `65280 * 32768 * 32768 < 2^46`; conversion to binary64 is exact. Division
by 256 is also exact. The combined score has five rounded operations: one area
division for each of Y, U, and V, followed by two additions. With
`u = DBL_EPSILON / 2` and `gamma_5 = 5u / (1 - 5u)`, two independent host
evaluations differ by at most:

```text
2 * gamma_5 * max(1, |expected|)
```

This bound depends on normalized score magnitude, not pixel count. At 960x540
the oracle values are `24.028187542197145` for Y and
`50.279152229214894` for Y+U+V; the corresponding bounds are `2.668e-14` and
`5.582e-14`. The unmodified device path matches both.

## Hypotheses and falsification

1. **Wrong oracle, correct kernel.** Confirmed: fixed oracle and device agree
   at both fixture sizes while float and fixed differ at the large size.
2. **Coefficient or rounding mismatch.** Falsified on the unmodified source by
   the exact-oracle comparisons.
3. **960x540 tail or mirror defect.** Falsified by the large exact-oracle run;
   its non-multiple-of-16/32 geometry agrees.
4. **Atomic reduction instability.** Falsified by ten serial hardware test
   executions (five per fixture size), all passing with the unchanged result.

## Red cap

The regression was planted by changing only the first production coefficient
from 3571 to 3572 while leaving the oracle untouched. The 960x540 test failed:

```text
y delta=1.581e-03, bound=2.668e-14
uv delta=3.291e-03, bound=5.582e-14
```

The coefficient was restored with no production diff. This proves the test is
sensitive to fixed arithmetic drift rather than merely confirming that UV adds
a positive value.

## Verification

- RED: old 960x540 float-vs-fixed test, exit 1 at `2.30e-4`.
- GREEN: named 256x144 and registered 960x540 Meson tests on Arc A380.
- RED CAP: one-unit coefficient mutation rejected by the large test.
- The large variant remains in the `fast`, `gpu`, and `sycl` suites and is
  globally serialized by the existing GPU-test rule.
- The C test is clean under clang-format 22 and strict clang-tidy 22; the
  changed Meson file configures and builds both targets. Documentation, ADR
  index, changelog, rebase-note, source-citation and state-row gates pass.

## Result

The tracked gap was a test-oracle defect, not a production-kernel defect.
`test_sycl_motion_add_uv_parity` now validates the actual fixed-point contract
under a derived resolution-independent host-roundoff bound, and the 960x540
variant is registered instead of excluded.
