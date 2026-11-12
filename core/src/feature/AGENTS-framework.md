<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# Collector, registry, and shared rules: `core/src/feature/` agent invariants

feature_extractor*, feature_collector*, feature_name.cpp, picture_copy*,
perceptual_weight.c, pelorus_udu_wrap.h, float_*.c, integer_adm*, adm_*,
barten_csf_tools.h, integer_vif.c, vif_tools.c, integer_ssim.c, brisque*,
cambi.c, ciede.c, delta_e_itp.c, niqe.c, pu21.c, speed*.c, ssimulacra2.c,
y_funque_plus.c, sycl/*adm*; M_PI/M_E, close_fex, alloc/fmaf bans, min-dim,
unwind.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `feature_extractor.h`, `float_vif.c`, `ssimulacra2.c` | [allocation-limits](AGENTS.d/allocation-limits.md) | Per-frame malloc or aligned_malloc for geometry-sized buffers is strictly prohibited. |
| `feature_extractor.cpp`, `feature_collector.cpp` | [ansnr-removal](AGENTS.d/ansnr-removal.md) | ANSNR and float_ansnr extractors remain removed per ADR-0865. |
| `integer_adm.c`, `integer_vif.c`, `cambi.c` | [codeql-renames](AGENTS.d/codeql-renames.md) | CodeQL declaration-hides-variable renames preserve bit-exact behavior across extractors. |
| `feature_extractor.h`, `feature_collector.cpp` | [context-pool-lifetime](AGENTS.d/context-pool-lifetime.md) | VmafFeatureExtractorContextPool fex_list growable pointer table lifecycle rules. |
| `feature_collector.cpp`, `feature_collector.h` | [feature-collector](AGENTS.d/feature-collector.md) | feature_collector.cpp mount/unmount traversal and single-authority lifecycle contracts. |
| `feature_extractor.cpp`, `feature_extractor.h` | [feature-registration](AGENTS.d/feature-registration.md) | feature_extractor_list[] is exactly-once and all extractors register in feature_extractor.cpp. |
| `feature_name.cpp`, `brisque_math.h` | [float-equality](AGENTS.d/float-equality.md) | Explicit floating-point comparison contracts replace direct equality tests. |
| `feature_extractor.h` | [fmaf-ban](AGENTS.d/fmaf-ban.md) | Scalar references never call libm fmaf to prevent contraction divergence across hosts. |
| `feature_extractor.cpp`, `feature_collector.cpp` | [governing-adrs](AGENTS.d/governing-adrs.md) | Governing ADR index and compliance contracts for feature extractors. |
| `brisque.c`, `ciede.c`, `delta_e_itp.c`, `float_adm.c`, `float_moment.c`, `float_motion.c`, `float_ms_ssim.c`, `float_psnr.c`, `float_ssim.c`, `float_vif.c`, `integer_adm.c`, `integer_ssim.c`, `integer_vif.c`, `niqe.c`, `pu21.c`, `speed.c`, `ssimulacra2.c`, `/core/test/test_libc_named_internal_functions.py` | [libc-named-statics](AGENTS.d/libc-named-statics.md) | no static C function named after C library function; extractor close callback is close_fex. |
| `/core/meson.build`, `/core/src/meson.build`, `adm_csf_tools.h`, `adm_tools.c`, `adm_tools.h`, `barten_csf_tools.h`, `ciede.c`, `integer_adm.h`, `integer_ssim.c`, `speed.c`, `speed_internal.c`, `speed_qa.c`, `vif_tools.c`, `y_funque_plus.c`, `sycl/float_adm_sycl.cpp`, `sycl/integer_adm_sycl.cpp` | [math-constants](AGENTS.d/math-constants.md) | No TU defines M_PI / M_E; <math.h> provides them, Windows via meson's -D_USE_MATH_DEFINES. |
| `float_motion.c`, `float_vif.c` | [min-dimension-guards](AGENTS.d/min-dimension-guards.md) | Minimum-dimension guards cover every plane, not only luma. |
| `feature_collector.cpp`, `feature_name.cpp` | [model-options](AGENTS.d/model-options.md) | Model options gate GPU twin selection; every option must be parsed or safely rejected. |
| `feature_collector.cpp`, `feature_extractor.h` | [non-finite-publication](AGENTS.d/non-finite-publication.md) | Non-finite values follow NaN and infinity cross-backend publication semantics. |
| `feature_extractor.cpp`, `feature_collector.cpp`, `feature_name.cpp` | [orientation](AGENTS.d/orientation.md) | Scope, ground rules, discoverability, option tables, and feature extractor workflows. |
| `feature_collector.cpp`, `feature_extractor.h`, `pelorus_udu_wrap.h`, `perceptual_weight.c` | [perceptual-side-data](AGENTS.d/perceptual-side-data.md) | Perceptual side-data weighting golden-isolation invariant and normalization contracts. |
| `picture_copy.cpp`, `picture_copy.h` | [picture-copy](AGENTS.d/picture-copy.md) | picture_copy channel selection and high-bit-depth sample normalization contracts. |
| `feature_extractor.h`, `feature_collector.h` | [shared-headers](AGENTS.d/shared-headers.md) | Shared C/C++ headers must declare one enum definition, never C++-only narrow enum. |
| `feature_extractor.cpp` | [split-helpers](AGENTS.d/split-helpers.md) | Helper functions split to meet LOC bounds must preserve expressions without truncation. |
| `ciede.c`, `feature_collector.cpp` | [unwind-helpers](AGENTS.d/unwind-helpers.md) | Error exits use unwind helpers instead of goto labels; cleanup order is invariant. |
