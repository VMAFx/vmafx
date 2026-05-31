<!-- markdownlint-disable MD013 MD060 -->
# Research-0434: VIF log2 LUT Shrink and Gaussian Filter Cache

## Summary

Investigation into two low-risk performance improvements to the integer and float VIF
paths, targeting the hot `vif_statistic_avx512` function and per-frame filter allocation.

## LUT Index Analysis (Win #1)

The `log2_32` normalization uses `__builtin_clz` to compute `k = 16 - clz(temp)`,
then shifts `temp >>= k`. After this:

- `k` is chosen so that bit 15 of the result is 1 and bit 16 is 0.
- Result is always in `[32768..65535]` = `[0x8000..0xFFFF]`.
- The always-set bit 15 is redundant as an index discriminator.
- Stripping it via `& 0x7FFF` yields an index in `[0..32767]` — a 32768-entry table.

Initial analysis incorrectly claimed a 16384-entry table (`& 0x3FFF`) was sufficient.
A C verification test (`/tmp/test_lut2.c`) was used to confirm the correct mask is
`0x7FFF` before landing. The 16384-entry version produced values off by ~1.0-1.1 VMAF
points (detected by the Netflix golden gate tests immediately).

The corrected 32768-entry table (64 KB) halves the original 128 KB footprint. It does
not fit in L1D (32 KB on AMD Zen) but reduces L2 occupancy and TLB entry count for the
three `_mm512_i32gather_epi64` sites in `vif_statistic_avx512`.

## Filter Cache Analysis (Win #3)

`vif_get_filter(out, scale, kernelscale)` calls `get_1d_gaussian_kernel(out, n, n/5.0f)`
which computes a 1D Gaussian by calling `expf` for each coefficient. With `scale` from
0–3, `kernelscale = 1.0` (default), the four filter widths are:

- scale 0: 17 taps
- scale 1: 9 taps
- scale 2: 5 taps
- scale 3: 3 taps

Across 4 scales × per-frame call = 34 `expf` evaluations per frame, fully avoidable
since `vif_kernelscale` is read-only after `VifState.init()`.

## Benchmark Results

Platform: AMD Zen (host), AVX-512, BBB 1080p YUV (300 frames), `vmaf_bench` tool.

| Path | Before | After | Delta |
|---|---|---|---|
| `vif (CPU) 1920×1080` | 16.22 ms/frame | 15.59 ms/frame | −3.9% (≈+4.2% FPS) |

Note: the projections from `/tmp/perf_findings.md` (+8–12% total speedup for integer VIF)
assumed L1 fit; since the corrected table is 64 KB (L2, not L1), the measured improvement
is smaller (~4%). The gain is real and consistent across 5 runs.

## References

- `integer_vif.h`: LUT definition, `log2_32`, `log2_64`
- `integer_vif.c`: `log_generate`
- `vif_avx512.c`: three gather sites
- `vif.c`: `compute_vif` loop
- `float_vif.c`: `VifState`, `init`, `extract`
