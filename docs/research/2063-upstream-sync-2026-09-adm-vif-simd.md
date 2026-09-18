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

### The zero-bit shift at frame widths 17..32

Frame widths 17 to 32 give scale-0 bands of 9 to 16 samples, so the scale-0
horizontal and vertical cube shift `ceil(log2(band) - 4)` is exactly 0. Its
rounding constant was `(uint32_t)pow(2, shift - 1)` in `adm_avx2.c` and
`adm_avx512.c`, and in upstream's scalar `integer_adm.c` as well. With
`shift - 1` wrapped to `UINT32_MAX` that is `(uint32_t)inf`, which is
undefined. The fork's scalar path already guarded it with `adm_half_shift()`.
The ASan+UBSan lane found the AVX2 copy once `test_integer_adm_tiny_frames`
reached those widths.

What the undefined conversion produced depends on the instruction the
compiler picked. Compiling the pre-fix files with their real build flags
shows `vcvttsd2siq` in the AVX2 unit, whose low 32 bits of
`0x8000000000000000` are 0, the scalar value, and `vcvttsd2usi` in the AVX-512
unit, which gives `0xFFFFFFFF`. That is the whole of the AVX-512 small-width
divergence recorded in the first sweep: AVX-512 scored `integer_adm_scale0`
0.0094, 0.0103 and 4.2e-05 off scalar at 18x18, 24x24 and 20x64, and is
bit-exact at every width 17..32 once all 16 constants call the shared
helper. The row's guess, a stage that assumes a full vector per row, was
wrong.

The same widths expose two defects in the CUDA and HIP scale-0 path, measured
on an RTX 4090 against scalar CPU. The host code computes the constant as
`1 << (shift - 1)` (2^31 on x86; 32x32 scores NaN), and the scale-0
contrast-masking kernels clamp the right and bottom neighbours with the base
index, reading column `w` and rows past `h` for bands of 14 samples or fewer.
`integer_adm_scale0` is 1.20296 on CUDA against 0.99174 scalar at 18x18. The
CUDA, HIP and SYCL twins also accept frames below 17x17. The fixes are a
separate PR because they touch GPU files outside this port.

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

### Int32 overflow in the 16-bit vertical DWT pass

Found while checking a report from the SYCL work: scale 0's vertical DWT pass
forms `sum(filter[k] * s[k])` before subtracting `46342 * 2^(bpc - 1)`. The
low-pass taps are `15826, 27411, 7345, -4240`, and the first three sum to
50582, so at 16 bpc the partial sum passes `INT32_MAX` once three consecutive
samples reach 42456. The scalar `adm_dwt2_vpass_16()` and the scalar vertical
loops inside `adm_dwt2_16_avx2()` and `adm_dwt2_16_avx512()` all summed in
`int32_t`. Upstream Netflix/vmaf has the same code.

Measured with a clang `-fsanitize=undefined` build on 176x144 16-bit frames
with luma in `[49152, 65535]`: nine reports, three per kernel (scalar
`integer_adm.c:1463/1464/1513`, AVX2 `adm_avx2.c:3392/3393/3397`, AVX-512
`adm_avx512.c:3495/3496/3500`). Scores were nevertheless right. The
normalised value is at most `54822 * 32768` in magnitude, inside int32, so
two's-complement wrap-around undoes itself. That is also why no parity test
could see it: every path wrapped the same way.

Options weighed for the fix:

| Option | UB-free for | Cost | Taken |
| --- | --- | --- | --- |
| Accumulate in int64, then narrow | every input | one 64-bit add per tap | yes |
| Centre each sample first, `sum(filter[k] * (s[k] - 2^(bpc-1)))` | in-range samples only; a 10-bit plane holding 16-bit garbage still overflows | one subtract per sample | no |
| Accumulate in `uint32_t`, convert back | nothing: the final unsigned-to-signed conversion of a negative result is implementation-defined | none | no |

The int64 form matches the file's own `i4_dwt2_tap4()`, which scales 1 to 3
already use. It is `adm_dwt2_vpass16_tap4()` in `integer_adm.h`, shared by the
scalar pass and both x86 kernels. For in-range input the narrowed result
equals the old wrapped one, so no score moves: 12 runs (10, 12 and 16 bpc,
scalar, AVX2 and AVX-512) are identical at `%.17g` before and after. NEON has
no 16-bit DWT; it falls back to scalar. The CUDA, HIP and Metal twins carry
the same int32 sum and are tracked in `docs/state.md`
(`T-GPU-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18`); the SYCL twin already
forms it in int64.

## Alternatives explored

- Taking upstream's `-2/-3` DWT2 bound for the NEON tail. It is correct but one
  column more conservative than the fork's x86 invariant. The fork form keeps
  one bound across all seven DWT2 kernels.
- Porting only the `memset` for the scale-3 read. It gives determinism
  without correctness, and the ASan read remains.
- Adopting checkasm wholesale. Rejected for now, for the reasons above.

## Open questions

- `adm_cm` AVX2 and AVX-512 were not bit-exact with scalar on uncorrelated
  full-range noise: 576x324 `integer_adm_scale0` gave `0.45876092664583873`
  scalar against `0.45858330648667378` AVX2, with identical numbers upstream.
  Cause: the scalar centre tap `(int16_t)(((ONE_BY_15 * abs(a)) + 2048) >> 12)`
  wraps for `|a|` above about 15360, the vector macros kept 32 bits. Fixed by
  `fix/adm-cm-simd-bitexact`; the Netflix golden pairs never reach the wrap.
- Upstream's scalar and SIMD ADM still carry the `(uint32_t)pow(2, shift - 1)`
  conversion. Worth reporting to Netflix/vmaf with the instruction evidence
  above.
- The CUDA and HIP tiny-frame defects are tracked in `docs/state.md` as
  `T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18`.

## Related

- [ADR-1257](../adr/1257-retire-darwin-adm-dwt2-legacy-dispatch.md) — Darwin compatibility dispatch retired.
- [ADR-1057](../adr/1057-revert-float-adm-simd-dispatch-neon-fma.md), [ADR-1207](../adr/1207-feature-isa-invariance-gate.md), [ADR-0245](../adr/0245-simd-bitexact-test-harness.md).
