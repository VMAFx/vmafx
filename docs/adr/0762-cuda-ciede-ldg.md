<!-- markdownlint-disable MD013 MD060 -->
# ADR-0762: CUDA CIEDE2000 8bpc/16bpc — `__ldg()` read-only cache routing (F3 fix)

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cuda`, `performance`, `ciede`, `fork-local`

## Context

`calculate_ciede_kernel_8bpc` and `calculate_ciede_kernel_16bpc` in
`core/src/feature/cuda/integer_ciede/ciede_score.cu` each take two `VmafPicture`
struct arguments by value. The struct carries `void *data[3]` channel pointers. Because
the void pointers are wrapped in a by-value struct, the compiler's non-coherent-load
analysis cannot see that the per-pixel reads (`r_y[x]`, `r_u[cx]`, etc.) are alias-free,
so it uses the ordinary L1/L2 cache path instead of routing them through the L1 read-only
texture cache via `__ldg()`.

ADR-0754 (PR #93) applied the identical fix — "F3" in the fork's CUDA perf taxonomy — to
`calculate_ssim_vert_combine`, achieving measurable L2 pressure reduction at 1080p and
above. ADR-0743 first established the pattern for VIF filter1d tmp buffers.

The CIEDE2000 kernel reads 6 channel samples per pixel (3 ref + 3 dis), all independent
and alias-free. Extracting typed `const uint8_t *__restrict__` (or `uint16_t *__restrict__`
for 16bpc) pointers before the pixel body and replacing indexed access with `__ldg(&ptr[i])`
makes the alias invariant visible and routes all 6 loads through the read-only path.
`__launch_bounds__(BLOCK_X * BLOCK_Y)` (= 256) is added as a register-budget hint,
consistent with the 16×16 block configuration.

## Decision

Extract raw `__restrict__` channel pointers from both `VmafPicture` arguments before the
per-pixel body of both kernels and use `__ldg()` for all 6 indexed channel reads. Add
`__launch_bounds__(BLOCK_X * BLOCK_Y)` to both kernels. Mirror the F3 pattern established
in ADR-0754 exactly.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `__ldg()` on extracted pointers (chosen) | Exposes alias-free invariant; routes via read-only cache; < 10 LOC change | None | Chosen |
| `const __restrict__` on kernel parameters directly | Slightly less verbose | `void *data[3]` in `VmafPicture` prevents applying restrict to the pointer-inside-struct at the call site | Struct layout prevents this without an API change |
| No change | Zero risk | Leaves 6 cache-line-filling loads per pixel on the coherent path | Performance opportunity missed; inconsistent with ADR-0754/0743 precedent |

## Consequences

- **Positive**: 6 per-pixel loads per kernel routed through L1 read-only cache, reducing
  L2 traffic at resolutions where the chroma planes don't fit in L1. Consistent with F3
  pattern applied to SSIM (ADR-0754) and VIF (ADR-0743).
- **Negative**: None; the change is semantics-preserving. CUDA vs CPU correctness
  confirmed at places=4 (max diff = 0.0 on 576×324 Netflix reference pair).
- **Neutral**: `integer_vif_cuda.c` merge-conflict stub (inherited from commit `24bb5daf89`)
  resolved in this PR; HEAD side retained (ADR-0743 comment block preserved).

## References

- ADR-0754 (PR #93): identical F3 fix on SSIM vert_combine.
- ADR-0743: first `__ldg()` application on VIF filter1d.
- ADR-0214: GPU vs CPU parity gate (places=4).
- req: "Apply the F3 fix ONLY to `calculate_ciede_kernel_8bpc` + `calculate_ciede_kernel_16bpc`. Mirror PR #93's pattern exactly."
