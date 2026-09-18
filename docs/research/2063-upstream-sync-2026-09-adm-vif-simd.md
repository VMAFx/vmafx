<!-- markdownlint-disable MD013 -->

# Research-2063: September 2026 upstream sync — ADM / VIF SIMD fixes and checkasm

- **Status**: Active
- **Workstream**: [ADR-1257](../adr/1257-retire-darwin-adm-dwt2-legacy-dispatch.md), [ADR-1207](../adr/1207-feature-isa-invariance-gate.md), [ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md) / [ADR-0139](../adr/0139-ssim-simd-bitexact-double.md)
- **Last updated**: 2026-09-18

## Question

Netflix/vmaf gained eight commits after the last reconciled upstream commit
`f85a85369` (PR #1456). Which of them affect the fork, whose ADM and VIF SIMD
paths have been reworked heavily (ADR-0138/0139 bit-exactness, helper
extraction, split files)? What does the port look like for each? And does the
fork need upstream's new checkasm harness?

## Sources

- Upstream commits, oldest first: `03b5562c5`, `8f7d50d29`, `ea012e387`,
  `cba9343ed`, `c023bb7cb`, `1786bd961`, `1801915be`, `86da14d03`.
- Fork history: `0ed57f9f1` (PR #1339, x86 DWT2 tail bound), `a013c1410`
  (PR #1134, NEON four-tap column 0), `89a8e3258` (PR #1154, NEON DWT2 parity),
  `6d61106ed` (PR #1156, NEON VIF residuals), `195f88a22` (PR #1161, Darwin
  wrapper).
- videolan checkasm at `dc9930dd0b42` (BSD-2-Clause), the revision upstream
  pins.

## Findings

Method: every x86 claim was checked against a static fork build on an
AVX-512 host, and every AArch64 claim against a cross build under
`qemu-aarch64-static`. Two kinds of harness were used. Kernel harnesses
filled output buffers with a sentinel and compared them against the shipped
scalar functions, exported from a scratch copy. An end-to-end harness scored
the same frames through the public API under two cpumasks and compared every
per-frame score bit for bit.

| Upstream commit | Fork verdict | Evidence |
| --- | --- | --- |
| `03b5562c5` adm_decouple_avx2 OOB write | **Affected, fixed here** | `adm_avx2.c` used `right - (right % 8)` while the vector loop starts at `left`. 372 of 992 band widths (9..1000, fork stride) stored up to six int16 past `right`. Band widths 32 and 40 stored one or two past the band, into the next row or slab band. Nothing reads those samples: region outputs and all 3,072 end-to-end AVX2 scores are unchanged by the fix. |
| `8f7d50d29` s123 odd-width tail bound | **Not affected** | Fork `0ed57f9f1` uses `half_w - 1 - ((half_w - 2) % N)` in all six x86 DWT2 kernels. Its last vector column `J <= half_w - 2` needs no mirror (`2J + 2 <= w - 1`). Upstream's new `(half_w - 2) - ((half_w - 3) % N)` is one column more conservative, and upstream still carries the old bound in `adm_dwt2_8_avx2` and `adm_dwt2_8/16_avx512`. |
| `ea012e387` adm_dwt2_8_neon OOB write | **Affected, fixed here** | The horizontal 8-wide loop had no tail. On every dispatched width it stored one sample at column `stride`: into the next row, which the next row then rewrote, or on the last row into the next band's `[0][0]`, with a value built from over-read scratch. On Linux AArch64, NEON disagreed with scalar at scale 0 for frame widths 24, 32 and 40 with heights below 50, and repeated NEON runs on 32x48 disagreed with each other. After the fix, results are bit-exact with scalar, 576x324 is unchanged, and so are widths 48..200 at height 72. |
| `cba9343ed` NEON column-0 dropped tap | **Not affected** | Fixed by `a013c1410`. The fork's Darwin wrapper kept the three-tap column on Apple deliberately; ADR-1257 retires it. |
| `c023bb7cb` vif_statistic_8_neon residuals | **Not affected** | Fixed by `6d61106ed`. NEON matched scalar across w 17..130. The same harness with the residual block removed mismatched on 21 of 24 widths. |
| `1786bd961` checkasm integration | **Partially relevant** | Framework not ported (see below). Its `adm_buffer_alloc()` zeroes `data_buf`, which hides an uninitialised read the fork still had. Root cause below. |
| `1801915be` checkasm CI workflow | Infrastructure only | Only meaningful with the framework. |
| `86da14d03` CAMBI AVX2 dp/mask row | Not present | Bit-exact in 200k fuzz cases. The mask row relies on the box sum staying below 2^31, which holds because the derivative is 0/1 and the filter is 7x7. Handled by a separate CAMBI PR. |

### The scale-3 uninitialised read

With the same binary, input and cpumask, fork `integer_adm_scale3` changed
from run to run for frame heights 17..32 (x86 scalar and AArch64 alike). It
happened on 24–45 of 201 widths per height. Poisoning `data_buf` with
different byte patterns moved the scale-3 DWT outputs, so the DWT was
reading storage no stage had written. The cause is `dwt2_src_indices_1d()`. For a subsampled extent
`n_half == 2`, which is a scale-3 input of 3 or 4 samples, meaning any frame
dimension from 17 to 32, the mirrored-tail loop started at `n_half - 2 = 0`.
It then overwrote the i == 0 mirror `{1, 0, 1, 2}` with `{-1, 0, 1, 2}`. Scale
3 read row -1, which is the tail of the preceding slab band, and column -1,
which is the int32 in front of the `tmp_ref` heap block. ASan reports the
latter as a heap-buffer-overflow in `adm_dwt2_s123_combined_avx512`. Starting
the tail at `max(1, n_half - 2)`, which is the form the fork's own DWT2 test
references already used, makes the results independent of `data_buf`
contents. Upstream carries the same index code. Its `memset` makes the read
deterministic, but the read is still out of bounds. The GPU twins mirror with
`abs()` and were never affected.

### checkasm versus the fork's parity tests

checkasm compares each shipped scalar kernel against each ISA on random data,
checks callee-saved registers and the stack, and benchmarks. It needs the
scalar kernels made non-static, and ADR-1207 rejected exactly that. It is
also a fetched subproject. It compares float results with a tolerance
(`adm_cm`: `1e-4 * (|ref| + 1)`), which the fork's bit-exact contract does
not allow. The durable lesson is narrower: both missed bugs wrote outside the
compared region. The fork therefore ports the lesson and not the framework:
guard-band helpers in `simd_bitexact_test.h`, real strides and sentinels in
the ADM/VIF parity tests, and small-size sweeps.

## Alternatives explored

- Taking upstream's `-2/-3` DWT2 bound for the NEON tail. It is correct but one
  column more conservative than the fork's x86 invariant. The fork form keeps
  one bound across all seven DWT2 kernels.
- Porting only the `memset` for the scale-3 read. It gives determinism
  without correctness, and the ASan read remains.
- Adopting checkasm wholesale. Rejected for now, for the reasons above.

## Open questions

- `adm_cm` AVX2 and AVX-512 are not bit-exact with scalar on uncorrelated
  full-range noise: 576x324 `integer_adm_scale0` gives `0.45876092664583873`
  scalar against `0.45858330648667378` AVX2, with identical numbers upstream.
  Tracked in `docs/state.md`.
- AVX-512 scale 0 diverges for frame widths 17..32 even on smooth content,
  in fork and upstream alike. Tracked in `docs/state.md`.

## Related

- [ADR-1257](../adr/1257-retire-darwin-adm-dwt2-legacy-dispatch.md) — Darwin compatibility dispatch retired.
- [ADR-1057](../adr/1057-revert-float-adm-simd-dispatch-neon-fma.md), [ADR-1207](../adr/1207-feature-isa-invariance-gate.md), [ADR-0245](../adr/0245-simd-bitexact-test-harness.md).
