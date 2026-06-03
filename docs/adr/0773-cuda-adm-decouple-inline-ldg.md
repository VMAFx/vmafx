<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
# ADR-0773: CUDA ADM decouple-inline — `__ldg()` F3 fix on active path

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cuda`, `performance`, `adm`, `fork-local`

## Context

PR #106 (ADR-0763) applied the F3 `__ldg()` pattern to `adm_decouple.cu`
(scale-0 and scale-1-3 standalone kernels). That file is dead code in the fork:
the decouple logic is inlined into `adm_csf.cu` and `adm_cm.cu` via
`adm_decouple_inline.cuh` (rebase-note 0002). The ADR-0763 change was a
preparatory maintenance pass with no score impact because those kernels are
never dispatched.

The _active_ path is the six inline `__device__` helpers in `adm_cm.cu`
(`inline_i4_csf_a`, `inline_i4_decouple_r`, `inline_s0_csf_a`,
`inline_s0_decouple_r`, `inline_i4_csf_r`, `inline_s0_csf_r`) and the two
kernel templates in `adm_csf.cu` (`i4_adm_csf_kernel`, `adm_csf_kernel`).
All eight sites load six band values (`band_h/v/d` for ref and dis) via plain
struct-member access — no `__ldg()`, no `__restrict__` extraction — so ptxas
routes those loads through the coherent L2 cache instead of the read-only L1
texture path.

ADR-0756 listed `adm_decouple` as a priority-dispatch F3 target; ADR-0763
addressed the dead-code file; this ADR addresses the live path.

## Decision

Extract `const T *__restrict__` band pointers from the `cuda_*_adm_dwt_band_t`
struct arguments at the top of each of the eight helper functions / kernel
templates before any indexed load, then replace every `ref->band_h[idx]`-style
load with `__ldg(&rh[idx])`. Write-back calls (none in these helpers — they are
read-only) would use plain pointers.

The change covers:

- `adm_csf.cu`: `i4_adm_csf_kernel<>` (int32 path) and `adm_csf_kernel<>`
  (int16 path)
- `adm_cm.cu`: `inline_i4_csf_a`, `inline_i4_decouple_r`, `inline_s0_csf_a`,
  `inline_s0_decouple_r`, `inline_i4_csf_r`, `inline_s0_csf_r`

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `__ldg()` extraction in callers (chosen) | Exposes alias-free invariant per-call; < 15 LOC per site | None | Chosen |
| Move extraction into `.cuh` helper signatures | Single place | `.cuh` functions receive pre-loaded integers, not pointers | Architecture mismatch; `.cuh` functions have no pointers to extract |
| No change | Zero risk | Leaves 6 global reads per pixel on the coherent L2 path at every CSF/CM dispatch | Performance opportunity missed; inconsistent with ADR-0754/0762/0763 precedent |

## Consequences

- **Positive**: Six per-pixel band loads at every CSF and CM kernel call now route
  through the L1 read-only cache. The ADM pipeline issues CSF then CM every frame;
  at 1080p the DWT2 planes (6 × int16 or 6 × int32) are too large for L1 without
  the non-coherent hint. Consistent with F3 pattern applied across the CUDA suite.
- **Negative**: None; change is semantics-preserving and bit-exact. CUDA vs CPU
  correctness confirmed at places=4 (max diff ≤ 1.00e-06 on 576×324 Netflix pair,
  0.00e+00 on 1080p checkerboard).
- **Neutral**: `adm_decouple.cu` (dead code) already carries ADR-0763; this ADR
  addresses the live path only.

## References

- ADR-0763 (PR #106): F3 fix on the dead-code `adm_decouple.cu`.
- ADR-0756: fork-wide F3 audit; `adm_decouple` listed as dispatch target.
- ADR-0754 (PR #93): original `__ldg()` F3 fix (SSIM vert_combine).
- ADR-0762 (PR #102): F3 fix on CIEDE2000 kernels.
- ADR-0214: GPU vs CPU parity gate (places=4).
- rebase-note 0002: explains why `adm_decouple.cu` is dead code.
- req: "Apply the same F3 fix (raw `__restrict__` pointer extraction + `__ldg()`
  on read-only loads) to the INLINE header — that's the path that actually executes."
