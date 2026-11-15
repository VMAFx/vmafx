---
paths:
  - core/src/feature/common/convolution_internal.h
  - core/src/feature/common/convolution.c
invariant: Reflect-101 mirror padding loops, short circuits, and border clamping contracts.
area: simd-and-gpu
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Reflect-101 Mirror Padding and Border Clamping

## Reflect-101 mirror padding — invariants (ADR-1166)

separable float convolution in `common/convolution_internal.h` uses
**reflect-101** mirror padding, and fold is deliberately **iterative**:

```c
FORCE_INLINE int convolution_reflect101(int idx, int size)
{
    if (size <= 1) return 0;
    while (idx < 0 || idx >= size)
        idx = (idx < 0) ? -idx : (2 * size - idx - 2);
    return idx;
}
```

Load-bearing details rebase or "simplification" must not break:

1. **loop is not decoration.** Upstream (and this fork, before ADR-1166)
   bounced once. One bounce only lands in range when `size >= radius + 1`;
   at `size == 2` tap of `-2` folds to `+2` and tap of `+3` folds to `-1`,
   and caller dereferences out of bounds. Two live CPU paths reached those
   sizes — `float_vif` on 9..15 px frames and `float_motion` with
   `motion_add_uv` on 4x4 4:2:0 chroma. Do not collapse it back to
   `if/else if`.
2. **`size <= 1` short circuit is required for termination**, not for
   correctness: at `size == 1` fold alternates between `-2` and `+2`
   forever.
3. **fold is bit-identical to single bounce for every in-contract
   size** (loop exits on first iteration), which is what lets this be
   pure safety fix with no score movement.
   `core/test/test_convolution_edge_small.c::test_large_plane_bit_identical`
   pins that against explicit single-bounce reference; if you change
   fold, that test must still pass unmodified.
4. **`convolution.c`'s `convolution_clamp_borders()` is load-bearing too.**
   `borders_right` / `borders_bottom` are derived as
   `dim - (filter_width - radius)` and go **negative** for plane narrower
   than filter, which makes trailing border loop start at negative
   index and write before destination. clamp is no-op for every
   `dim >= filter_width`.

motion extractors' own `mirror()` bodies (`integer_motion.c`,
`integer_motion_v2.c`, `x86/motion_avx2.c`, `x86/motion_avx512.c`,
`arm64/motion_v2_neon.c`, and CUDA / HIP / Metal twins) are **still
single-bounce on purpose**: they sit behind `init()` guard that rejects
`w < 3 || h < 3`, so defective sizes are unreachable. That is deliberate
divergence from Netflix/vmaf#1581, which instead fixes `mirror()` so tiny
frames can be scored. Changing it is behaviour decision, not cleanup —
see `docs/rebase-notes.md`.
