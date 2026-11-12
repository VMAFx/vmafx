---
paths:
  - core/src/feature/arm64/speed_neon.c
  - core/src/feature/arm64/psnr_hvs_neon.c
  - core/src/meson.build
invariant: arm64_v8_fp strict-FP library placement, pointer accumulators, SpEED covariance, and MSVC ARM64 portability.
---
# FP library placement, SpEED covariance, and MSVC ARM64

## Ground rules

- **Float-arithmetic NEON TUs belong in `arm64_v8_fp`** (not `arm64_v8`).
  Static lib `arm64_v8_fp` uses compiler-native no-contraction arguments
  from `vmaf_strict_fp_args` (ADR-0873); `arm64_v8` is integer-only and carries
  no FP flag. Adding float-arithmetic TU to `arm64_v8` is bit-exactness
  regression risk. Moving integer-only TU to `arm64_v8_fp` is harmless but
  unnecessary.
- **`accumulate_error()` and similar reductions thread accumulators by
  pointer** — do NOT introduce local-float accumulator inside helper.
  ADR-0159: local accumulator drifts Netflix golden by ~5.5e-5
  (`psnr_hvs_neon.c`).

- **SpEED covariance: `speed_neon.c` is row kernel of ADR-1459, not upstream's
  kernel.** `speed_cov_row_neon` pairs one x block with up to five y blocks at
  consecutive columns; lanes = separate covariance sums (two `float64x2_t` +
  one scalar), `vmulq_f64` then `vaddq_f64`, so every sum has bits of
  `compute_cov_kernel_scalar` (`../speed.c`; contract `../speed_cov.h`).
  `test_speed_simd` compares with `memcmp` under qemu, GCC + clang. Lives in
  `arm64_v8_fp` (no contraction); scalar fifth lane keeps product in own
  statement. Never `vfmaq_f64`, never one sum split over lanes: upstream
  `compute_cov_kernel_neon` (Netflix/vmaf 15297286) does both (4061 of 18480
  sums off, max relative 3.5e-12) and stays out on sync.
  `compute_cov_kernel_scalar` carries function-scoped no-contraction guard
  (ADR-1057 pattern): without it clang fused its loop remainder and no kernel
  could match at every width.

- **MSVC compiles this directory (ADR-1260, `Windows ARM64 MSVC` lane).**
  `cl.exe` ARM64: no GCC vector extensions on NEON types (`v[0]`, `a + b`,
  brace-initialised or compound-literal vectors), no `_x2`/`_x3`/`_x4`
  multi-register loads, `__attribute__` / `#pragma GCC` / `#pragma clang`
  only under `#if defined(__GNUC__)` or `defined(__clang__)` guards as
  today. `<arm_neon.h>` is include on every compiler. Strict FP comes from
  shared `vmaf_strict_fp_args` in `core/src/meson.build`: `/fp:precise` on
  MSVC, `/fp:precise /clang:-fno-fast-math /clang:-fcomplex-arithmetic=full
  /clang:-ffp-contract=off` on `intel-llvm-cl`,
  `/clang:-ffp-contract=off` on clang-cl, and `-ffp-contract=off` on GCC/clang.
  Never restore literal strict-FP option in arm64 `c_args`. SVE2 TUs
  never build under MSVC (no `<arm_sve.h>`; probe skipped). Local MSVC check
  is impossible; QEMU cross build covers GCC/clang side only.
