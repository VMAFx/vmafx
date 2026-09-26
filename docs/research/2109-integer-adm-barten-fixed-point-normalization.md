<!-- markdownlint-disable MD013 MD060 -->
# Research-2109: Integer ADM Barten fixed-point normalization — 2026-09-25

**Status:** Complete

**Authority inspected:** signed collector
`9e1b8d435dd858169bcfa3750f480d6129730450`

**Scope:** correctness of `adm_csf_mode=1` on CPU and the CUDA, SYCL, HIP,
and Metal integer-ADM twins. No benchmark, tuning, retraining, model weight,
score snapshot, Netflix golden assertion, dependency, public API, or FFmpeg
surface change.

## Failure and hypothesis

On the canonical 576x324 Netflix pair, the original fixed-point mode 1
published NaN for `integer_adm2`, `integer_aim`, and scale 1 and collapsed the
other scale scores toward zero. ADR-1191 replaced the silent narrowing with
`-EINVAL`, but the documented default Barten configuration remained unusable.

The raw weights explain the failure. Watson97 is near `1e-2`; Barten at the
default scale is about 1.21 at DWT scale 0 and 26.98 at scale 3. Multiplying
those values by the existing Q21/Q23/Q32 factors overflows the scale-0
`uint16_t` representation and consumes the signed headroom of later CSF and
contrast-masking operations. Widening only the stored weight cannot work: the
weighted bands and the cube accumulator remain bounded.

The checked hypothesis was that one power-of-two exponent per scale can retain
the three bands' ratios, keep every downstream integer inside its existing
budget, and be restored exactly after the contrast-masking cube.

## Headroom experiment

The raw fixed-point values were divided by the smallest shared `2^k` below a
candidate scale-1..3 ceiling, then exercised through the real CPU extractor on
a deterministic textured pair:

| Scale-1..3 exclusive ceiling | Result |
| ---: | --- |
| 2^32 | scales 1 and 2 still published NaN |
| 2^31 | scale 1 still published NaN |
| 2^30 | all seven mode-1 outputs finite and non-degenerate |

Scale 0 remains storage-limited to values below 2^16. The 2^30 ceiling leaves
two headroom bits for the signed CSF/CM arithmetic and cube accumulation.

For each scale, all three bands use the same exponent `k`; choosing separate
exponents would change the metric. Because contrast masking accumulates the
cube of the normalized signal, its final power-of-two divisor subtracts
`3k`. The denominator continues to multiply by the original floating-point
CSF factors. With `k=0`, conversion and CPU SIMD dispatch are unchanged.

## Float-reference comparison

The exact collector's 576x324 Netflix pair was scored once with fixed-point
`adm=adm_csf_mode=1` and once with `float_adm=adm_csf_mode=1`. Pooled means:

| Output | Integer ADM | Float ADM | Absolute delta |
| --- | ---: | ---: | ---: |
| `adm2` | 0.939569 | 0.939583 | 0.000014 |
| `aim` | 0.017911 | 0.017910 | 0.000001 |
| `adm3` | 0.960829 | 0.960836 | 0.000007 |
| scale 0 | 0.763542 | 0.763569 | 0.000027 |
| scale 1 | 0.848033 | 0.848042 | 0.000009 |
| scale 2 | 0.916561 | 0.916575 | 0.000014 |
| scale 3 | 0.961231 | 0.961247 | 0.000016 |

The maximum pooled absolute difference is `2.7e-5`, inside the places=4
cross-backend contract and far from the prior NaN/near-zero failure.

## Backend coupling

`adm_csf_fixed_point.h` owns the limits and conversion. CPU, CUDA, SYCL, HIP,
and Metal store `k` for each scale and restore `3k` in their host-side CM
finalizers. CUDA and Metal apply it to DLM and AIM. SYCL and HIP do not provide
AIM/ADM3, so they apply it to ADM2 and per-scale outputs only. Every denominator
uses the unnormalized float factors.

Metal integer ADM previously rejected every nonzero CSF mode despite declaring
the CPU range. It now uses the common factor selection and normalization for
modes 0–3, and its `adm_csf_mode` entry no longer carries
`VMAF_OPT_FLAG_DEFAULT_ONLY`. GPU `float_adm` remains mode-0-only.

## Verification

- `test_adm_csf_representable` passes all six cases, including finite,
  non-degenerate mode-1 `adm2`, `aim`, and `adm3`, an already-representable
  Barten configuration, the maximum accepted Barten scale, and continued
  rejection of an invalid blend-table geometry.
- The canonical CPU run above completed in an isolated, device-less container;
  no live `vmaf-dev-mcp` container was touched.
- CUDA and HIP wrappers plus their mode-1 parity executables compile and link
  in isolated no-device builds. On the idle local AMD `gfx1036`, the real HIP
  kernel build passes all five focused cases, including mode-1 parity at
  places=4. The CUDA executable returns Meson's hardware skip status 77 when
  no device is passed through; the local RTX 4090 was already occupied, so it
  was deliberately not multiplexed.
- The SYCL kernel and mode-1 parity executable compile and link under oneAPI
  2026.1. Its inherited LTO/plugin mismatch was isolated by setting
  `b_lto=false` for this verifier; the device-less run then skips with status
  77 as designed.
- The device-free option-capability contract passes with Metal integer ADM
  removed from the default-only inventory. The Metal parity source includes a
  places=4 mode-1 case; an Apple hardware run remains the platform gate.

## Result

Full-scale Barten is a valid fixed-point ADM configuration again. It produces
finite, reference-consistent scores without weakening invalid-input guards or
changing already-representable arithmetic. Performance work is intentionally
deferred until the correctness-first RC1 cleanup is complete.
