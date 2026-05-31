<!-- markdownlint-disable MD013 MD060 -->
# ADR-0500: VIF log2 LUT Shrink and Gaussian Filter Cache

- **Status**: Accepted
- **Date**: 2026-05-18
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `simd`, `perf`, `integer-vif`, `float-vif`

## Context

Integer VIF (`vif_statistic_8_avx512` / `vif_statistic_16_avx512`) is the dominant
consumer of wall-clock time in a full VMAF run (approximately 45% of total runtime at
1080p on x86-64 with AVX-512). Two independent inefficiencies were identified:

**Win #1 — LUT too large for L2 locality.**
`VifPublicState.log2_table` was declared as `uint16_t log2_table[65537]`, occupying
128 KB of data. The inner-loop normalization in `log2_32` and `log2_64` uses
`__builtin_clz` / `__builtin_clzll` to shift the input until its most-significant
bit lands at bit position 15. This means the array index after normalization is
*always* in `[32768..65535]`; entries `[0..32767]` are never read. The 128 KB table
crosses several L2 cache lines unnecessarily. By reindexing with `idx & 0x7FFF`
(strip the always-set bit 15), the table shrinks to 32768 entries (64 KB), halving
L2 footprint and reducing TLB pressure on the three AVX-512 gather sites.

**Win #3 — Redundant Gaussian filter computation per frame.**
`compute_vif` in `vif.c` called `vif_get_filter()` once per scale (4× per frame).
`vif_get_filter` calls `get_1d_gaussian_kernel`, which invokes `expf`. The
`vif_kernelscale` option is immutable after `VifState.init()`; pre-computing all
four filters once at init and caching them in `VifState` eliminates this
transcendental cost entirely.

## Decision

We will:

1. Shrink `log2_table` from 65537 to 32768 entries by stripping the always-set
   MSB (bit 15) from the normalised mantissa index. Introduce constants
   `VIF_LOG2_TABLE_SIZE = 32768` and `VIF_LOG2_TABLE_OFFSET = 0x8000` in
   `integer_vif.h`. Update `log_generate`, `log2_32`, `log2_64`, and the three
   `_mm512_i32gather_epi64` gather sites in `vif_avx512.c`.

2. Add `float filter_cache[4][128]` and `int filter_width_cache[4]` to
   `VifState` in `float_vif.c`. Populate them once in `init()`. Extend the
   `compute_vif` signature with nullable `precomputed_filters` / `precomputed_filter_widths`
   parameters; the existing internal call in `vifdiff` passes NULL (backward-compatible
   old path).

Both changes preserve bit-exactness: the uint16 values in the LUT are identical to
the original (same formula, different address), and the cached filter coefficients
are float-identical to those previously computed per-frame.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep LUT at 128 KB | No change | 128 KB in L2 competes with active pixel data | Chosen against |
| Shrink to 16384 entries (mask 0x3FFF) | Fits L1D on Zen | Wrong: mantissa range is 32768, not 16384 | Numerically incorrect; rejected |
| Move filter cache into `compute_vif` as static | Simple | Thread-unsafe; breaks concurrent multi-thread scoring | Rejected |
| Change filter to a new function without NULL-path | Cleaner signature | Would break `vifdiff` internal call | Rejected in favour of nullable param |

## Consequences

- **Positive**: Integer VIF at 1920×1080 improves from 16.2 ms/frame (61.6 FPS) to
  15.6 ms/frame (64.2 FPS), approximately +4.2% throughput.  Float VIF gains
  elimination of 4 × `expf` calls per frame (~sub-millisecond).
- **Negative**: `compute_vif` gains two new parameters; callers must be updated (only
  two call sites exist, both updated in this PR).
- **Neutral**: The `VIF_LOG2_TABLE_SIZE` / `VIF_LOG2_TABLE_OFFSET` constants are
  internal; the public C API is unchanged. No FFmpeg patch update required.

## References

- `req`: per-user direction to implement Win #1 (LUT shrink) and Win #3 (filter cache)
  from `/tmp/perf_findings.md`.
- ADR-0138 / ADR-0139: bit-exactness invariants for SIMD VIF paths.
- Related: `vif_statistic_avx512` function in `core/src/feature/x86/vif_avx512.c`.
