# AGENTS.md — core/src/feature/common

Orientation for agents working on the cross-architecture helpers
shared by feature extractors. Parent: [../AGENTS.md](../AGENTS.md).

## Scope

```text
feature/common/
  alignment.{c,h}            # vmaf_align / pinned-buffer alignment helpers
  blur_array.{c,h}           # ring-buffer blur backing for motion / motion_v2
  convolution.{c,h}          # scalar separable convolution (used by float_adm / float_vif / motion / SpEED-class extractors)
  convolution_avx.c          # ADR-0143 generalised AVX2 scanlines (8-wide FMA)
  convolution_avx512.c       # ADR-0504 AVX-512F port of same scanlines (16-wide FMA)
  convolution_internal.h     # private helpers (RESTRICT, MAX_FWIDTH_AVX_CONV, ...)
  macros.h                   # cross-toolchain FORCE_INLINE / RESTRICT / UNUSED_FUNCTION
```

Note: the `iqa/` tree (`iqa_convolve`, `iqa_ssim_tools`) is a separate
SSIM-specific scalar reference — do not confuse it with the
`convolution.c` here, which handles the wider feature population
(`float_adm`, `float_vif`, `motion`, etc.).

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md) +
  [../../AGENTS.md](../../AGENTS.md)).
- **Headers are upstream-mirror at the structural level** (Netflix
  copyright on `macros.h`, `alignment.{c,h}`, `convolution.{c,h}`)
  even though `convolution_avx.c` is fork-local at the surface
  level (ADR-0143).
- **`RESTRICT` is the project-wide spelling** for the C99 `restrict`
  qualifier; `FORCE_INLINE` is the project-wide spelling for the
  always-inline attribute. Both are defined in `macros.h` with MSVC
  / GCC / Clang dispatch. Do not introduce a parallel spelling.

## Rebase-sensitive invariants

- **`convolution_avx.c` scanline helpers are fork-local `static`**
  (ADR-0143). The four
  `convolution_f32_avx_s_1d_{h,v}_scanline` helpers inside the TU
  carry `static` linkage in the fork (upstream leaves them with
  external linkage out of habit, but no other TU references them).
  Strides are `ptrdiff_t` inside helpers, `int` at the public
  `convolution_f32_avx_*_s` wrappers, with `(ptrdiff_t)` casts at
  pointer-offset multiplication sites. **On rebase**: keep the
  fork's `static` and `ptrdiff_t` unless upstream adopts them. The
  detailed reason lives in [../AGENTS.md
  §"Generalised AVX convolve scanline helpers"](../AGENTS.md) and
  [ADR-0143](../../../../docs/adr/0143-port-netflix-f3a628b4-generalized-avx-convolve.md);
  [`docs/rebase-notes.md` §0036](../../../../docs/rebase-notes.md)
  carries the port history.

- **`convolution_avx512.c` inherits the same invariants** (ADR-0504).
  The four static scanline helpers (`convolution_f32_avx512_s_1d_*`)
  and three public wrappers use `static` linkage and `ptrdiff_t`
  strides identically to the AVX2 counterparts. Key additional
  constraint: the project-wide allocation contract is `MAX_ALIGN == 32`,
  so AVX-512 vertical scanlines must use `_mm512_loadu_ps` /
  `_mm512_storeu_ps` even when the stride is a multiple of 16 floats.
  Do not reintroduce aligned 64-byte AVX-512 memory ops unless
  `MAX_ALIGN` and every float-buffer caller are promoted together.
  **Results are NOT bit-identical to the AVX2 path** (wider FMA tree
  → different rounding) — this is accepted for the float path per
  ADR-0214.

- **`MAX_FWIDTH_AVX_CONV` in `convolution_internal.h`** sizes the
  `__m256 f[]` filter-tap buffer in `convolution_avx.c`. Bumping
  this constant changes the per-call stack frame for the convolution
  fast path; keep it the upstream-canonical 33 unless an ADR
  justifies the bump.

- **Header-level Netflix copyright on `convolution.{c,h}` and
  `macros.h`** — these are upstream-mirror files. On rebase, prefer
  upstream's shape and only annotate fork-local divergence with an
  inline comment + an ADR reference.

## Twin-update rules

- **`convolution_avx.c` ↔ `convolution.c`**: the AVX path mirrors
  the scalar separable convolve. Any kernel-shape change in the
  scalar (boundary handling, accumulator type, kernel-width
  semantics) needs a paired AVX edit.

- **`alignment.{c,h}`** is consumed by every SIMD TU (`__m256` /
  `__m512` / `float64x2_t` aligned spills) and most GPU host-glue
  TUs (CUDA pinned host, SYCL USM host, Vulkan staging buffers).
  Renaming `vmaf_align` is a project-wide event; don't.

- **`blur_array.{c,h}`** is consumed by `motion.c`, `motion_v2.c`,
  and several GPU motion twins (`../cuda/integer_motion_*_cuda.c`,
  `../sycl/integer_motion_*_sycl.cpp`, `../vulkan/motion*_vulkan.c`).
  The ring-buffer carry semantics are part of the GPU twins'
  ping-pong contract — see [../AGENTS.md §"motion3_score GPU
  contract"](../AGENTS.md).

## Governing ADRs

- ADR-0143
  ([`0143-port-netflix-f3a628b4-generalized-avx-convolve.md`](../../../../docs/adr/0143-port-netflix-f3a628b4-generalized-avx-convolve.md))
  — generalised AVX convolve scanlines (`static` + `ptrdiff_t`
  fork-local invariants).
- ADR-0504
  ([`0504-float-convolution-avx512-port.md`](../../../../docs/adr/0504-float-convolution-avx512-port.md))
  — AVX-512F port of the same scanlines; dispatched before AVX2 in
  `vif_tools.c`; inherits all ADR-0143 invariants.
- [ADR-0146](../../../../docs/adr/0146-nolint-sweep-function-size.md) —
  helper-decomposition discipline applied across the IQA, common,
  and VIF surfaces.
