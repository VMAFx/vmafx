<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# SIMD, GPU twins, and shared kernels: `core/src/feature/` agent invariants

common/convolution*, iqa/*, simd_dx.h, compat_builtin.h, arm64/*sve2*, cuda/*,
hip/*, sycl/*, metal/*, GPU option-table parity, row strides, helper-header
licences, and ISA invariance.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `compat_builtin.h` | [compat-builtin](AGENTS.d/compat-builtin.md) | MSVC builtin clz shim must use _BitScanReverse, never __lzcnt. |
| `common/convolution.c`, `common/convolution_avx.c`, `common/convolution_avx512.c` | [convolution-dispatch](AGENTS.d/convolution-dispatch.md) | Convolution scanline helpers and SIMD dispatch twin synchronization across backends. |
| `feature_extractor.h`, `feature_name.cpp` | [gpu-option-tables](AGENTS.d/gpu-option-tables.md) | GPU-twin VmafOption tables mirror CPU table entry-for-entry with full semantics. |
| `cuda/*`, `hip/*`, `sycl/*`, `metal/*` | [gpu-row-stride](AGENTS.d/gpu-row-stride.md) | GPU code never advances a pointer to samples wider than a byte by a byte stride. |
| `cuda/speed/speed_cuda_params.h`, `hip/float_adm/float_adm_hip_math.h`, `hip/float_ssim/ssim_decimate.h`, `hip/integer_ciede/ciede_hip_math.h`, `metal/float_ms_ssim_option_semantics.h`, `sycl/sycl_ciede_math.h`, `sycl/sycl_integer_ssim_math.h`, `sycl/sycl_ssim_terms.h`, `sycl/sycl_ssimulacra2_math.h` | [helper-header-licences](AGENTS.d/helper-header-licences.md) | Helper header = EUPL-1.2 AND licences of exactly code reproduced, notices added; no reference code = EUPL-1.2. |
| `iqa/convolve.c`, `iqa/ssim_tools.c` | [iqa](AGENTS.d/iqa.md) | IQA/VIF SIMD helper decomposition and reserved-identifier cleanup invariants. |
| `/core/test/test_feature_isa_invariance.c` | [isa-invariance](AGENTS.d/isa-invariance.md) | Feature scores must not depend on host ISA; promotions and reductions must match scalar. |
| `common/convolution_internal.h`, `common/convolution.c` | [reflect-padding](AGENTS.d/reflect-padding.md) | Reflect-101 mirror padding loops, short circuits, and border clamping contracts. |
| `simd_dx.h` | [simd-dx](AGENTS.d/simd-dx.md) | SIMD DX framework macros and recurring vector kernel patterns across targets. |
| `arm64/moment_sve2.c`, `arm64/ssimulacra2_sve2.c` | [sve2-simd](AGENTS.d/sve2-simd.md) | SVE2 SIMD ports portability, build gating, and fallback guarantees. |
