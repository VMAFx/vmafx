---
paths:
  - core/src/feature/common/convolution.c
  - core/src/feature/common/convolution_avx.c
  - core/src/feature/common/convolution_avx512.c
invariant: Convolution scanline helpers and SIMD dispatch twin synchronization across backends.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Convolution Helpers and SIMD Dispatch Synchronization

- **Generalised AVX convolve scanline helpers** (fork-local,
  ADR-0143): four `convolution_f32_avx_s_1d_*_scanline`
  helpers in [`common/convolution_avx.c`](../common/convolution_avx.c)
  are `static` in fork (upstream leaves them extern out of
  habit). Strides are `ptrdiff_t` inside helpers, `int` at
  public `convolution_f32_avx_*_s` wrappers, with `(ptrdiff_t)`
  casts at pointer-offset multiplication sites. On rebase: keep
  fork's `static` and `ptrdiff_t` unless upstream adopts them.
  See [ADR-0143](../../../../docs/adr/0143-port-netflix-f3a628b4-generalized-avx-convolve.md)
  and [rebase-notes 0036](../../../../docs/rebase-notes.md).

## `convolution_f32_avx_rows_s`: SpEED's AVX2 vertical pass (Netflix/vmaf 9cb9479f)

Public, called from `vif_tools.c` (`vif_filter1d_vertical_dispatch_s()`) only.
Caller resolves mirrored row pointers; function takes no stride and no
alignment (unaligned loads, masked tail: no read or write past `width`), so
SpEED's fused filter runs it on any plane. Returns scalar vertical pass's
bits (products in tap order, sum from 0, no FMA). Fork form of upstream's
`convolution_f32_avx_dec16_s()` vertical half; decimated horizontal pass
stays in `vif_tools.c`. **On rebase**: do not import upstream's
`convolution_f32_avx_dec16_s()` or `VMAF_NO_FUSE` (contraction already off,
ADR-1461); port tap-order changes into this function. Guard:
`test_speed_filter` (`test_avx_rows`, `test_filter_dec16`).

## `convolution_f32_c_s` dispatches to SIMD — fix the twins, not just the scalar

`core/src/feature/common/convolution.c::convolution_f32_c_s` returns straight
into `convolution_f32_avx_s` whenever `VMAF_X86_CPU_FLAG_AVX2` is set. That is
every CI runner and dev workstation. **fix applied only to scalar
body in `convolution.c` is dead code on x86.**

AVX2 (`convolution_avx.c`) and AVX-512 (`convolution_avx512.c`) twins each
derive same vertical border split — `radius` and `height - radius` — at
three sites apiece, once per kernel variant (`_s`, `_sq_s`, `_xy_s`). Six sites
total. All of them must stay clamped via `convolution_clamp_borders` in
`convolution_internal.h`: for plane shorter than radius, `height - radius`
is negative, so trailing border loop starts at negative row and
leading one runs past end. Both are heap **writes**, not reads.

**Testing scalar kernel does not test this.**
`core/test/test_convolution_edge_small.c` calls `convolution_y_c_s` /
`convolution_x_c_s` directly and so never reaches dispatch;
`test_motion_min_dim.c` only calls `init()`. Anything asserting convolution
is safe at small sizes must go through public API — see
`core/test/test_motion_convolution_oob.c`.

**guard must mirror kernel it protects, not option that named it.**
`motion_blur_plane` keeps `filter_size = 5` for `motion_filter_size == 1` and
merely swaps in `FILTER_5_NO_OP_s`, so radius is 2 regardless. guard that
reads option value instead of filter width kernel uses
will let defective sizes through.

**Chroma plane geometry is ceiling, `(dim + ss) >> ss`, matching
`picture.c`.** Using `h / 2` under-allocates by one row for every odd luma
height, and even-height fixtures — including both Netflix golden resolutions —
never catch it.
