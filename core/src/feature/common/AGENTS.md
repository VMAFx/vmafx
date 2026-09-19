# AGENTS.md — core/src/feature/common

Orientation: cross-architecture helpers shared by feature extractors.
Parent: [../AGENTS.md](../AGENTS.md).

## Scope

```text
feature/common/
  alignment.{c,h}            # vmaf_align / pinned-buffer alignment helpers
  blur_array.{c,h}           # ring-buffer blur backing for motion / motion_v2
  convolution.{c,h}          # scalar separable convolution (used by float_adm / float_vif / motion / SpEED-class extractors)
  convolution_avx.c          # ADR-0143 generalised AVX2 scanlines (8-wide FMA)
  convolution_avx512.c       # ADR-0504 AVX-512F port of same scanlines (16-wide FMA)
  convolution_internal.h     # private helpers (RESTRICT, border folding, ...)
  macros.h                   # cross-toolchain FORCE_INLINE / RESTRICT / UNUSED_FUNCTION
```

`iqa/` tree (`iqa_convolve`, `iqa_ssim_tools`) = separate SSIM-specific scalar
reference. Do not confuse with `convolution.c` here (`float_adm`, `float_vif`,
`motion`, etc.).

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md) +
  [../../AGENTS.md](../../AGENTS.md)).
- **Headers = upstream-mirror at structural level** (Netflix copyright on
  `macros.h`, `alignment.{c,h}`, `convolution.{c,h}`). `convolution_avx.c` =
  fork-local at surface level (ADR-0143).
- **`RESTRICT` = project-wide spelling** for C99 `restrict` qualifier;
  `FORCE_INLINE` = project-wide spelling for always-inline attribute. Both
  defined in `macros.h` with MSVC / GCC / Clang dispatch. Do not introduce
  parallel spelling.

## Rebase-sensitive invariants

- **`convolution_avx.c` scanline helpers = fork-local `static`** (ADR-0143).
  Four `convolution_f32_avx_s_1d_{h,v}_scanline` helpers inside TU carry
  `static` linkage in fork (upstream leaves external linkage; no other TU
  references them). Strides = `ptrdiff_t` inside helpers, `int` at public
  `convolution_f32_avx_*_s` wrappers, with `(ptrdiff_t)` casts at
  pointer-offset multiplication sites. **On rebase**: keep fork `static` and
  `ptrdiff_t` unless upstream adopts them. Detail in [../AGENTS.md
  §"Generalised AVX convolve scanline helpers"](../AGENTS.md) and
  [ADR-0143](../../../../docs/adr/0143-port-netflix-f3a628b4-generalized-avx-convolve.md);
  [`docs/rebase-notes.md` §0036](../../../../docs/rebase-notes.md) carries
  port history.
- **`convolution_avx512.c` inherits same invariants** (ADR-0504). Four static
  scanline helpers (`convolution_f32_avx512_s_1d_*`) and three public wrappers
  use `static` linkage and `ptrdiff_t` strides identical to AVX2 counterparts.
  Key constraint: project-wide allocation contract = `MAX_ALIGN == 32`,
  so AVX-512 vertical scanlines must use `_mm512_loadu_ps` / `_mm512_storeu_ps`
  even when stride = multiple of 16 floats. Do not reintroduce aligned 64-byte
  AVX-512 memory ops unless `MAX_ALIGN` and every float-buffer caller
  promoted together. **Results NOT bit-identical to AVX2 path** (wider FMA tree
  -> different rounding) — accepted for float path per ADR-0214.
- **Horizontal convolution bounds preserve arithmetic regions**:
  `j_vec_end` = first final scalar output. Horizontal SIMD source-start count
  = `max(j_vec_end - radius, 0)`. Full vectors plus masked final loads/stores
  cover only those outputs; never compute discarded lanes past row, even when
  caller pads workspace. Keep original final scalar start and AVX2
  multiply/add versus AVX-512 FMA operations. Clamp tiny-width scalar borders
  to plane. Normal, squared and cross-product wrappers share ISA horizontal
  pass. Preserve `test_convolution_horizontal` and tight final-row allocation
  contract. See [boundary
  investigation](../../../../docs/research/convolution-horizontal-boundary-2026-09-08.md).
- **`MAX_FWIDTH_AVX_CONV` in `convolution.h`** sizes `__m256 f[]` filter-tap
  buffer in `convolution_avx.c`. Bumping constant changes per-call stack frame
  for convolution fast path; keep current supported limit of 17 unless ADR
  justifies bump.
- **Header-level Netflix copyright on `convolution.{c,h}` and `macros.h`** —
  upstream-mirror files. On rebase: prefer upstream shape; annotate fork-local
  divergence only with inline comment + ADR reference.

## Twin-update rules

- **`convolution_avx.c` ↔ `convolution.c`**: AVX path mirrors scalar separable
  convolve. Kernel-shape change in scalar (boundary handling, accumulator
  type, kernel-width semantics) requires paired AVX edit.
- **`alignment.{c,h}`** consumed by every SIMD TU (`__m256` / `__m512` /
  `float64x2_t` aligned spills) and most GPU host-glue TUs (CUDA pinned host,
  SYCL USM host, other host staging buffers). Renaming `vmaf_align` =
  project-wide event; don't.
- **`blur_array.{c,h}`** consumed by `motion.c`, `motion_v2.c`, and GPU motion
  twins (`../cuda/integer_motion_*_cuda.c`,
  `../sycl/integer_motion_*_sycl.cpp`). Ring-buffer carry semantics = part of
  GPU twins ping-pong contract — see [../AGENTS.md §"motion3_score GPU
  contract"](../AGENTS.md).

## Governing ADRs

- ADR-0143
  ([`0143-port-netflix-f3a628b4-generalized-avx-convolve.md`](../../../../docs/adr/0143-port-netflix-f3a628b4-generalized-avx-convolve.md))
  — generalised AVX convolve scanlines (`static` + `ptrdiff_t` fork-local
  invariants).
- ADR-0504
  ([`0504-float-convolution-avx512-port.md`](../../../../docs/adr/0504-float-convolution-avx512-port.md))
  — AVX-512F port of same scanlines; dispatched before AVX2 in `vif_tools.c`;
  inherits all ADR-0143 invariants.
- [ADR-0146](../../../../docs/adr/0146-nolint-sweep-function-size.md) —
  helper-decomposition discipline across IQA, common, VIF surfaces.
