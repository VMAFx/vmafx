# ADR-0854 — Direct AVX-512 parity tests for motion kernels

| Field      | Value                                         |
|------------|-----------------------------------------------|
| Status     | Accepted                                      |
| Date       | 2026-05-29                                    |
| Author     | Claude (Sonnet 4.6) on behalf of Lusoris      |
| Supersedes | —                                             |
| Superseded | —                                             |

## Context

A prior SIMD audit of the motion feature (`test_motion_v2_simd.c`,
ADR-0245) exercised only `motion_score_pipeline_16_avx2`.  The AVX-512
paths (`motion_score_pipeline_8_avx512`, `motion_score_pipeline_16_avx512`,
`sad_avx512`, `y_convolution_8_avx512`, `y_convolution_16_avx512`,
`x_convolution_16_avx512`) were reachable only indirectly through the
Netflix golden-data tests, which operate at full-frame granularity and do
not assert bit-exactness at the per-kernel level.  The audit report
explicitly flagged this as a coverage gap.

A correctness concern was also noted in the AVX-512 pipeline: the 16-bit
path uses `_mm512_srav_epi64` (arithmetic right shift) for the per-lane
`>> bpc` step.  The AVX2 counterpart had used `_mm256_srlv_epi64` (logical
shift), which was audited and documented in `test_motion_v2_simd.c`.
The AVX-512 version already uses the correct arithmetic shift, but no
direct test asserted this on adversarial negative-diff fixtures.

## Decision

Add `core/test/test_motion_avx512_parity.c` with ten test cases:

| Test name                          | Kernel under test                   |
|------------------------------------|-------------------------------------|
| `test_pipeline_8_random`           | `motion_score_pipeline_8_avx512`    |
| `test_pipeline_8_bright_dis`       | `motion_score_pipeline_8_avx512`    |
| `test_pipeline_16_bpc10`           | `motion_score_pipeline_16_avx512`   |
| `test_pipeline_16_bpc12`           | `motion_score_pipeline_16_avx512`   |
| `test_pipeline_16_neg_diff_bpc10`  | `motion_score_pipeline_16_avx512`   |
| `test_sad_avx512`                  | `sad_avx512`                        |
| `test_y_conv_8_avx512`             | `y_convolution_8_avx512`            |
| `test_y_conv_16_bpc10`             | `y_convolution_16_avx512`           |
| `test_y_conv_16_bpc12`             | `y_convolution_16_avx512`           |
| `test_x_conv_16_avx512`            | `x_convolution_16_avx512`           |

Each test compares the AVX-512 output to a scalar reference at
bit-exact level (`memcmp` for array outputs, `uint64_t ==` for SADs).
The test binary is gated to `['x86_64', 'x86']` arch families in meson
and skips at runtime via `simd_test_have_avx512()` on hosts that do not
expose `VMAF_X86_CPU_FLAG_AVX512`.

`simd_test_have_avx512()` is added to `simd_bitexact_test.h` alongside
the existing `simd_test_have_avx2()` function.

## Alternatives considered

**Leave AVX-512 covered only by Netflix golden tests.** Rejected: the
golden tests run at full-frame granularity and with many intervening
pipeline stages, making it difficult to isolate a kernel-level regression.
A future bug in any of the six kernels would only surface as a floating-
point score drift, not as an immediate test failure.

**Extend `test_motion_v2_simd.c` with AVX-512 cases.** Rejected: the
existing file is focused on auditing the `srlv_epi64` logical-shift
concern in the AVX2 path.  Mixing AVX-512 cases in would blur the audit
trail and make the file's comment block misleading.

**Use a tolerance rather than bit-exact comparison.** Rejected: all six
kernels operate exclusively in integer arithmetic; bit-exactness is
guaranteed by construction and is the stated contract (ADR-0138,
ADR-0139).  Using a tolerance would hide real correctness regressions.

## Consequences

- Ten new fast-SIMD unit tests run on every CI push on x86_64 hosts with
  AVX-512 support.  On hosts without AVX-512 the binary exits cleanly with
  a "skipping" message.
- The `simd_bitexact_test.h` harness gains a reusable `simd_test_have_avx512()`
  helper available to all future AVX-512 unit tests.
- No changes to production code; no performance impact.

## References

- req: "Add direct AVX-512 parity tests for motion" (user direction,
  2026-05-29 session)
- ADR-0138 / ADR-0139: bit-exactness contracts for SIMD paths
- ADR-0245: `simd_bitexact_test.h` shared harness
- `core/test/test_motion_v2_simd.c`: AVX2 precedent (srlv_epi64 audit)
