<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# ADM extractors and twins: `core/src/feature/` agent invariants

integer_adm*, float_adm*, adm_*, barten_csf_tools.h,
core/src/rust/feature/adm/**, and ADM SIMD/GPU twins (x86/adm_*, arm64/*adm*,
cuda/*adm*, sycl/*adm*, hip/*adm*, metal/*adm*).

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `adm_tools.c`, `adm_tools.h` | [adm-angle-flag](AGENTS.d/adm-angle-flag.md) | adm_angle_flag has exactly one definition across floating-point and integer paths. |
| `adm_csf_fixed_point.h`, `integer_adm.c` | [adm-barten-weights](AGENTS.d/adm-barten-weights.md) | One power-of-two exponent per scale; weight limits come from adm_csf_fixed_limit(), never from storage. |
| `adm_tools.c`, `adm_tools.h` | [adm-contrast-masking](AGENTS.d/adm-contrast-masking.md) | ADM contrast-masking edge policy is asymmetric across CPU and GPU twins. |
| `adm_tools.h`, `barten_csf_tools.h`, `metal/float_adm_metal.mm`, `/core/test/test_float_adm_csf_upstream.c`, `/core/test/test_float_adm_csf_upstream_contract.py`, `/core/test/barten_csf_cxx.cpp` | [adm-csf-weights-float](AGENTS.d/adm-csf-weights-float.md) | float ADM CSF weights (Watson step, Barten CSF) = upstream float arithmetic; Barten header same bits in C and C++. |
| `integer_adm.c`, `integer_adm.h` | [adm-dwt](AGENTS.d/adm-dwt.md) | Integer ADM DWT mirror table for tiny extents and 16-bit vertical int64 sums. |
| `integer_adm.c`, `adm_tools.c` | [adm-kernels-restructure](AGENTS.d/adm-kernels-restructure.md) | Integer ADM kernels header separation, AIM clipping differences, and adm_min_val clamping. |
| `integer_adm_kernels.h`, `sycl/integer_adm_sycl.cpp`, `metal/integer_adm_metal.mm`, `metal/integer_adm_metal_host.c`, `/core/test/test_integer_adm_quant_step.c`, `/core/test/test_integer_adm_quant_step_contract.py` | [adm-quant-step](AGENTS.d/adm-quant-step.md) | dwt_quant_step() exponent = float product of k, temp, temp, as upstream; CPU and SYCL copies change together. |
| `integer_adm.c`, `integer_adm_kernels.h`, `arm64/adm_neon.c`, `adm_cm_accumulator.h`, `x86/adm_avx2.c`, `x86/adm_avx512.c` | [adm-rounding](AGENTS.d/adm-rounding.md) | Integer ADM i4_adm_cm rounding overflow, row rounding, scale-0 masking, and gain limits. |
| `integer_adm.c`, `integer_adm.h`, `integer_adm_kernels.h`, `adm_csf_fixed_point.h`, `adm_cm_accumulator.h`, `adm_angle_flag.h`, `adm_score.h`, `barten_csf_tools.h`, `/core/src/rust/feature/adm/**` | [adm-rust-twin](AGENTS.d/adm-rust-twin.md) | Integer ADM arithmetic change in C = same change in core/src/rust/feature/adm; rust_twin_diff.py adm shows 0 diffs. |
| `adm_view_dist.c`, `adm_view_dist.h`, `integer_adm.c`, `cuda/integer_adm_cuda.c`, `sycl/integer_adm_sycl.cpp`, `hip/integer_adm_hip.c`, `metal/integer_adm_metal.mm`, `metal/integer_adm_metal_host.c`, `feature_extractor.h`, `/core/src/fex_ctx_vector.cpp`, `/core/src/rust/shim/rust_twins.cpp`, `/core/src/rust/feature/adm/**` | [adm-view-distance-merge](AGENTS.d/adm-view-distance-merge.md) | Second ADM distance = one context; transform once per scale, weigh per distance; scores equal separate contexts. |
| `float_adm.c`, `cuda/float_adm_cuda.c`, `sycl/float_adm_sycl.cpp`, `hip/float_adm_hip.c`, `metal/float_adm_metal.mm`, `/core/src/fex_ctx_vector.cpp`, `/core/test/float_adm_twin_parity.h`, `/core/test/test_float_adm_debug_key_refusal.c` | [float-adm-debug-key](AGENTS.d/float-adm-debug-key.md) | float_adm's debug ratio `adm` is never suffixed, on CPU and every twin; second debug instance is refused. |
| `float_adm.c`, `adm_tools.h` | [float-adm](AGENTS.d/float-adm.md) | Float ADM GPU exports, strict division (no reciprocal estimate), min dim 17x17, and signature stability. |
