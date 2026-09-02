<!-- markdownlint-disable MD060 -->
# Research-0435: ADM decouple gather locality (2026-05-18)

## Summary

Analysis of the `adm_decouple_avx512` gather bottleneck and evaluation
of two candidate acceleration strategies.

## Findings

### LUT properties

- `adm_div_lookup` is declared `int32_t div_lookup[65537]` in
  `core/src/feature/integer_adm.h` (line 33), populated once via
  `pthread_once`.
- Size: 65 537 × 4 = 262 148 bytes ≈ 256 KB.
- Typical L1 data cache: 32–48 KB.  Typical L2: 256–512 KB.
- The table fits in L2 but shares it with the frame buffer data;
  effective hit rate is low for the scattered gather pattern.

### Index pattern

The three gather indices are `oh + 32768`, `ov + 32768`, `od + 32768`
where `oh`, `ov`, `od` are int16 DWT sub-band coefficients
sign-extended to int32 before the shift.  These values are
input-dependent and are not monotone within a row.

Inspected path (in `adm_decouple_avx512`, lines 895–945):

```c
__m512i oh = _mm512_cvtepi16_epi32(
    _mm256_loadu_si256((__m256i *)(ref->band_h + i * stride + j)));
// ... (ov, od similarly)
// indices = oh + 32768, range [0, 65536] arbitrary scatter
__m512i oh_div = _mm512_i32gather_epi32(
    _mm512_add_epi32(oh, _mm512_set1_epi32(32768)), adm_div_lookup, 4);
```

### Approach A ruling

`vpermd` + sequential load requires that all 16 indices per vector
lane fit within a contiguous 16-element window (or at most a 32-element
window accessible by a 512-bit load + permute).  DWT coefficients from
H, V, D sub-bands can take any value in [−32768, +32767]; the expected
scatter width across the 65 K-entry table far exceeds any
cache-line-width window.  Approach A is not viable without a structural
change to the LUT layout (e.g., hash-partitioned micro-tables), which
would require a scalar reference change and invalidate the bit-exact
contract.

### Approach B evaluation

Software prefetch 2 iterations ahead is standard technique for
gather-heavy kernels where the index stream is known 2 iterations
before the gather executes.  Distance of 2 × 16 = 32 elements covers
approximately 300 instructions of arithmetic between the prefetch hint
and the gather — comfortably within the L2 miss latency (100–250
cycles on Zen 4 / Skylake-X).

The implementation prefetches into T1 (L2) not T0 (L1) because:

- The 48 cache lines (3 bands × 16 elements) per iteration will
  immediately be evicted from L1 by the dense band-buffer loads that
  follow.
- L2 residence is sufficient to eliminate the L3/DRAM stall.

### Benchmark results (BBB 1080p, 302 frames, release build)

| Build | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 | Run 6 | Run 7 | Run 8 | Mean |
|---|---|---|---|---|---|---|---|---|---|
| Baseline (master) | 9364 | 8670 | 9651 | 9084 | 10520 | 9593 | 9920 | 10022 | **9603 ms** |
| Patched (ADR-0502) | 8747 | 8677 | 9714 | 9540 | 8974 | 9768 | 8876 | 8100 | **9049 ms** |

Improvement: 554 ms / 9603 ms = **−5.8 % wall time**.

### Bit-exactness verification

`diff` of all frame metrics across 302 frames × 15 metrics = 4 530
metric-frame pairs: max absolute difference = 0.  The change is a
pure memory-access-strategy change; no arithmetic path is altered.

## Conclusion

Approach B (software prefetch, T1) is viable, ships a measured 5.8 %
wall-time improvement, and preserves bit-exactness.  Implemented as
ADR-0502.
