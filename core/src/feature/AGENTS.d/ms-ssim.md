---
paths:
  - core/src/feature/ms_ssim.c
  - core/src/feature/ms_ssim.h
  - core/src/feature/float_ms_ssim.c
invariant: MS-SSIM decimate LPF coefficients, mirror bounds, -ENOMEM handling, and SIMD parity.
area: ssim-and-metrics
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# MS-SSIM Decimate Coefficients, Bounds, and SIMD Parity

- **MS-SSIM decimate LPF coefficients**: 9-tap 9/7 biorthogonal
  filter table (`ms_ssim_lpf_h` / `ms_ssim_lpf_v`) appears verbatim in
  four TUs that must stay byte-identical for bit-exactness
  contract — `ms_ssim_decimate.c`, `x86/ms_ssim_decimate_avx2.c`,
  `x86/ms_ssim_decimate_avx512.c`, and
  `arm64/ms_ssim_decimate_neon.c`. source of truth upstream is
  `g_lpf_h` / `g_lpf_v` in `ms_ssim.c`. If rebase touches any of
  those five files, diff all five against each other before pushing.
  See [ADR-0125](../../../../docs/adr/0125-ms-ssim-decimate-simd.md).
- **KBND_SYMMETRIC mirror**: `ms_ssim_decimate_mirror` is duplicated
  across same four TUs and must match upstream
  `KBND_SYMMETRIC` branch in `iqa/convolve.c`. Changing boundary
  semantics in any one of them breaks bit-identity.
- **MS-SSIM decimate `-ENOMEM` contract** (ADR-0877, 2026-05-30):
  `malloc`-failure branch in all four MS-SSIM decimate TUs returns
  `-ENOMEM` (negative POSIX errno), matching libvmaf internal
  convention documented in `ms_ssim_decimate.h`. On rebase, if
  upstream port re-introduces `return -1` here, restore
  `-ENOMEM` form (and `#include <errno.h>`). Caller
  `ms_ssim.c:207-208` uses truthy check so either compiles. Errno
  form is required by ADR-0877 for higher-level error reporters
  (Go controller, MCP server, ffmpeg filter) that surface cause.
- **SSIM / MS-SSIM SIMD bit-exactness invariants** (fork-local,
  ADR-0138 + ADR-0139 + ADR-0140): AVX2 / AVX-512 / NEON paths
  in `x86/ssim_avx2.c` / `x86/ssim_avx512.c` /
  `arm64/ssim_neon.c` / `x86/convolve_avx2.c` /
  `x86/convolve_avx512.c` / `arm64/convolve_neon.c` are
  bit-identical to scalar reference under FLT_EVAL_METHOD == 0.
  Two rules are load-bearing and must be preserved on rebase:
  1. **Convolve taps**: each tap is *single-rounded `float * float`
     → widen to `double` → `double` add*. No FMA. Mirrors scalar
     `sum += img[i] * k[j]` in
     [`iqa/convolve.c`](../iqa/convolve.c). Changing scalar to `fmaf`
     or to double-mul pattern requires matching all three SIMD
     variants.
  2. **SSIM accumulate**: `2.0 *` literal in
     [`ssim_accumulate_default_scalar`](../iqa/ssim_tools.c)
     (`2.0 * ref_mu[i] * cmp_mu[i] + C1` and
     `2.0 * srsc + C2`) is C `double` literal, which promotes
     float operands to double before multiply. All three
     SIMD accumulators do `2.0 *` numerator + division + final
     `l*c*s` product per-lane in scalar double to match. If
     upstream ever changes `2.0` literal to `2.0f` (or
     restructures l/c numerators), all three SIMD variants
     need matching rewrite.
  3. **AVX-512 vector-double per-lane reduction**: AVX-512
     accumulator (`x86/ssim_avx512.c`) computes `lv`, `cv`, `sv`,
     and `lv*cv*sv` lane-wise in two 8-wide `__m512d` passes via
     `_mm512_cvtps_pd` widening + plain `_mm512_mul_pd` /
     `_mm512_add_pd` / `_mm512_div_pd` (no `_mm512_fmadd_pd`),
     then spills to `_Alignas(64) double[16]×4` and accumulates
     left-to-right scalar into `local_*`. vector-double form
     is bit-identical to scalar lane-wise by IEEE-754, but only
     because: () op order matches scalar's parse — `((2*rm)*cm
     /l_den`, etc.; (b) no FMA contraction; (c) running
     sum stays scalar left-to-right, lane 0 → lane 15. Tree
     reductions over 16-lane block break ADR-0139's
     running-sum invariant against scalar and are forbidden
     unless scalar itself is rewritten in lockstep. AVX2 and NEON
     stay on per-lane scalar path (`ssim_accumulate_lane`)
     for now — vectorising them with same `__m256d` /
     `float64x2_t` widening would follow same three rules.

- **MS-SSIM `enable_lcs` GPU implementation (T7-35, PR #207 MERGED)**
  — wires existing CPU `enable_lcs` 15-extra-metrics through
  CUDA + Vulkan + SYCL MS-SSIM kernels. On rebase: ensure
  option metadata stays declared on GPU paths even if
  body is still TODO.
- **`float_ms_ssim` `enable_chroma` (ADR-0583, ADR-1334)**:
  `float_ms_ssim.c` has `bool enable_chroma` field in `MsSsimState`
  and per-plane loop in `extract()` emitting `float_ms_ssim_cb` /
  `float_ms_ssim_cr`. default is `false` (luma-only, backward-
  compatible). Chroma dimensions use ceil subsampling, so the exact 4:2:0
  luma floor for a 176x176 chroma pyramid is 351x351. SYCL and Metal compute
  all three planes; HIP accepts the option but remains explicitly luma-only;
  CUDA does not expose it. If
  upstream Netflix adds any option to `float_ms_ssim.c`, mirror it to all
  GPU twins in same PR per twin-parity invariant.
