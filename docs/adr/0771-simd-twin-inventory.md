# ADR-0771: SIMD twin coverage inventory and gap prioritisation

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `simd`, `docs`, `planning`

## Context

A systematic audit of `core/src/feature/` was performed to determine which
features have x86 AVX2, AVX-512, and arm64 NEON accelerated twins and which
do not. This ADR records the findings and ranks the missing twins by
implementation leverage so future `/add-simd-path` invocations are
prioritised correctly.

**Audit method**: cross-referenced every `*.c` entry-point file under
`core/src/feature/` against `x86/<feature>_avx2.c`,
`x86/<feature>_avx512.c`, and `arm64/<feature>_neon.c`, then inspected
each feature's dispatch block to distinguish "no SIMD file" from "SIMD
file exists but dispatcher does not call it".

## Decision

We accept the inventory below as the authoritative coverage baseline and
record the three highest-leverage gaps for future work. No implementation
work is done in this ADR; each gap will be addressed via a dedicated
`/add-simd-path` PR referencing this ADR.

### Full coverage matrix

| Feature | AVX2 | AVX-512 | NEON | Notes |
| --- | --- | --- | --- | --- |
| `adm` (integer) | YES | YES | YES | Full dispatch in `integer_adm.c` |
| `cambi` | YES | YES | YES | |
| `ciede` | YES | YES | YES | |
| `float_adm` | YES | YES | YES | |
| `float_moment` | YES | — | YES | `moment_avx512.c` never created; AVX-512 dispatch absent |
| `float_motion` | YES | YES | YES | |
| `float_ms_ssim` | — | — | — | Delegates to `ssim_avx2/neon` via `iqa_ssim_set_dispatch`; effectively covered |
| `float_psnr` | YES | YES | YES | |
| `float_ssim` | — | — | — | Same delegation as `float_ms_ssim`; effectively covered |
| `float_vif` | — | — | — | Delegates through `vif_tools.c` which has inline AVX-512 convolution; effectively covered |
| `integer_adm` | YES | YES | YES | Full dispatch |
| `integer_motion` | YES | YES | — | `motion_neon.c` exports only `x_convolution_16_neon`; `y_convolution` and `sad` have no NEON path |
| `integer_motion_v2` | YES | YES | YES | Full dispatch including NEON |
| `integer_psnr` | YES | YES | YES | Full dispatch |
| `integer_ssim` | — | — | — | **Zero SIMD**: no dispatch block, no SIMD includes |
| `integer_vif` | YES | YES | YES | Full dispatch |
| `moment` | YES | — | YES | Same as `float_moment` — `moment_avx512.c` absent |
| `motion` | YES | YES | YES | |
| `ms_ssim` | — | — | — | Delegates to `ssim_avx2/neon`; effectively covered |
| `ms_ssim_decimate` | YES | YES | YES | |
| `psnr` | YES | YES | YES | |
| `ssim` | YES | YES | YES | |
| `ssimulacra2` | YES | YES | YES | |
| `vif` | YES | YES | YES | |

**Dropped features** (no SIMD twins expected):

- `ansnr` — deliberately removed (PR #38 / commit `70ed8b3c`). Prior SIMD
  tests (`test_ansnr_simd`) were orphaned and removed in follow-up fixes.

### True gaps (no effective SIMD coverage)

1. **`integer_ssim` — zero SIMD** (AVX2, AVX-512, NEON all absent). The
   inner loops (`gaussian_filter_init`, horizontal/vertical accumulation,
   SSIM term) are fully scalar. `integer_ssim` is the codec-side SSIM
   surface used by every non-float pipeline.

2. **`integer_motion` NEON incomplete** — `motion_neon.c` provides only
   `x_convolution_16_neon`; the equally hot `y_convolution_8/16_neon` and
   `sad_neon` are absent. On arm64 hosts `integer_motion` therefore runs
   its SAD and Y-convolution scalar. `integer_motion_v2` does NOT share
   this gap (it has a complete NEON dispatch).

3. **`moment` / `float_moment` AVX-512** — `moment_avx2.c` and
   `moment_neon.c` exist; `moment_avx512.c` does not. The two hot kernels
   (`compute_1st_moment`, `compute_2nd_moment`) are load-heavy reductions
   that benefit from wider vector lanes on Skylake-X / Ice Lake / Sapphire
   Rapids class hardware.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Add SIMD for float_ssim / float_vif directly | Direct control of the dispatch path | Both already delegate through shared helpers that have SIMD; redundant work | Rejected — delegation is effective |
| Batch all three gaps into one PR | Fewer PRs | Large diff, hard to review, one failure blocks all | Rejected — each gap gets its own `/add-simd-path` call |
| No-op / defer indefinitely | Zero effort | ARM64 runtime performance regresses relative to AVX2 parity; integer_ssim on GPU-less servers is fully scalar | Rejected |

## Consequences

- **Positive**: future `/add-simd-path` agents have a definitive baseline;
  no re-audit needed before implementation PRs.
- **Negative**: three gaps remain open until dedicated PRs land.
- **Neutral follow-up**: `integer_ssim` AVX2 is gap #1; `integer_motion`
  NEON completion is gap #2; `moment` AVX-512 is gap #3 in priority order
  (see research digest for rationale).

## References

- Code audit: `core/src/feature/x86/`, `core/src/feature/arm64/`, and
  dispatcher blocks in `integer_*.c` / `float_moment.c` (2026-05-29).
- ADR-0179 — prior `float_moment` AVX2 work that established the
  `moment_avx2.c` pattern.
- Research digest: `docs/research/simd-twin-inventory-2026-05-29.md`.
