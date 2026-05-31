<!-- markdownlint-disable MD060 -->
# ADR-0504: AVX-512F port of float separable convolution scanlines

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `simd`, `performance`, `build`

## Context

The float VIF path dominates CPU wall time in the float VMAF model (~60 % of
cycles on a representative 1080p run):

- `convolution_f32_avx_s_1d_h_scanline`: 8.48 %
- `convolution_f32_avx_s_1d_v_scanline`: 5.60 %
- `extract.lto_priv.17` (inlined per-scale loop): 35.89 %

All three hot spots route through the two separable scanline helpers in
`core/src/feature/common/convolution_avx.c`, which are 256-bit (8 floats
per FMA). Modern server and consumer CPUs (Skylake-X, Ice Lake, Sapphire
Rapids, Zen 4, and their successors) expose AVX-512F, which allows 16 floats
per FMA — doubling the arithmetic throughput in the inner convolution loop with
identical instruction count overhead.

The float VIF path already diverges from the integer VIF path at the numerical
level (ADR-0214). The fork therefore accepts ULP-level differences between SIMD
widths on the float path, provided Netflix golden assertions still pass at
their declared `places` tolerance.

## Decision

We will create `core/src/feature/common/convolution_avx512.c` porting the
four static scanline helpers and the three public wrappers
(`convolution_f32_avx512_s`, `_sq_s`, `_xy_s`) from `__m256` to `__m512`
with `_mm512_fmadd_ps`. The dispatch in `vif_tools.c` is updated to prefer
AVX-512 when `VMAF_X86_CPU_FLAG_AVX512` is set, falling back to AVX2, then
scalar. The new TU is compiled with `-mavx512f -mavx512dq -mavx512bw` inside
the existing `x86_avx512_static_lib` in `core/src/meson.build`.

The invariants from ADR-0143 are preserved in the new file:

- Scanline helpers carry `static` linkage.
- Inner-loop strides are `ptrdiff_t`.
- Horizontal pass uses `_mm512_loadu_ps` (no alignment guarantee on tmp row
  interior); vertical pass uses `_mm512_load_ps` (tmp rows are aligned to
  `MAX_ALIGN` = 64 bytes, which satisfies the 64-byte `_mm512_load_ps`
  requirement because `vmaf_ceiln(width, 16)` rounds up to a 64-byte boundary
  for float arrays allocated via `aligned_malloc`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep AVX2 only | No code addition | Leaves 40-50 % of float VIF cycles on the table on AVX-512 CPUs | Performance goal not met |
| Highway / simde abstraction library | Single source for all widths | Adds a dependency; diverges from fork's "intrinsics by hand" pattern (MEMORY.md) | Not the fork's SIMD style |
| Dual-issue two AVX2 vectors per iteration | Avoids a new file | Does not reduce instruction count; throughput gain marginal vs AVX-512 | Less clean than a proper 512-bit port |

## Consequences

- **Positive**: expected +40-50 % throughput on the float VIF path on CPUs
  that report AVX-512 (runtime-gated; safe on non-AVX-512 machines).
- **Negative**: AVX-512 increases register pressure and may cause
  frequency throttling on early Skylake-X. The throughput gain exceeds the
  throttling penalty for the convolution loop (FMA-bound, not memory-bound).
- **Neutral**: results on AVX-512 CPUs are NOT bit-identical to AVX2 results
  (wider FMA partial-sum tree → different rounding); this is accepted per
  ADR-0214 for the float path.
- **Follow-ups**: if a snapshot regression appears on `testdata/scores_cpu_*.json`,
  regenerate via `/regen-snapshots` with a commit message citing ADR-0504.

## References

- ADR-0143 (`0143-port-netflix-f3a628b4-generalized-avx-convolve.md`) — invariants
  inherited by this port.
- ADR-0214 — precedent for float-path ULP divergence between SIMD widths.
- Perf profile (`/tmp/perf_findings.md` Win #2, 2026-05-18 session).
