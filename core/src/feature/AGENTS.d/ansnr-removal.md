---
paths:
  - core/src/feature/feature_extractor.cpp
  - core/src/feature/feature_collector.cpp
invariant: ANSNR and float_ansnr extractors remain removed per ADR-0865.
area: framework
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# ANSNR and float_ansnr Feature Extractor Removal

- **ANSNR / float_ansnr feature extractor removal (ADR-0865)**:
  `ansnr` and `float_ansnr` (CPU scalar, AVX2, AVX-512, NEON, CUDA, HIP, SYCL,
  Metal) were sunset and completely removed from library. ANSNR is legacy
  pre-VMAF metric (circa 2001) never adopted in any production VMAF model.
  On any rebase or upstream sync from Netflix/vmaf:
  - If upstream re-introduces `ansnr` or `float_ansnr` sources under `libvmaf/src/feature/`
    (`ansnr.c`, `ansnr.h`, `ansnr_options.h`, `ansnr_tools.c`, `ansnr_tools.h`,
    `float_ansnr.c`, or SIMD files `ansnr_avx2.c`, `ansnr_avx512.c`, `ansnr_neon.c`),
    re-drop them.
  - Keep `feature_extractor.cpp` free of any `ansnr` registration symbols.
  - Keep dispatch registries and feature lists free of `ansnr` / `float_ansnr`.
