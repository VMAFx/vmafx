<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1146: SPEED and CAMBI Feature Rework to Fork Standards (Bit-Exact)

- **Status**: Accepted
- **Date**: 2026-09-02
- **Deciders**: lusoris
- **Tags**: `lint`, `ci`, `refactor`, `feature`, `speed`, `cambi`, `bit-exact`

## Context

Upstream Netflix feature extractors under `core/src/feature/` carry legacy coding patterns
that trigger numerous warnings under the fork's strict lint profile (clang-tidy, cppcheck,
SEI CERT C, NASA/JPL Power of 10). Following Wave 1 of the upstream-mirror rework (ADR-1138),
the SPEED and CAMBI feature families remained to be updated. On 2026-08-31, baseline scans
recorded 104 warnings across 8 translation units and headers:

- `core/src/feature/cambi.h` (27 warnings)
- `core/src/feature/speed.c` (31 warnings)
- `core/src/feature/cambi.c` (25 warnings)
- `core/src/feature/x86/cambi_avx2.c` (9 warnings)
- `core/src/feature/x86/cambi_avx512.c` (6 warnings)
- `core/src/feature/speed_qa.c` (4 warnings)
- `core/src/feature/x86/speed_avx2.c` (1 warning)
- `core/src/feature/x86/speed_avx512.c` (1 warning)

Maintainer mandate (2026-09-02):
> "we still have upstream code that isnt reworked to our standards -> do it, nothing is save anymore as long as the goldens pass"

Invariants governing this rework:

1. **Bit-exactness**: Numerical outputs must be bit-identical across all feature variants (`speed_chroma`, `speed_temporal`, `cambi` with `full_ref`, `window_size`, `topk`, `enc_width`). Integer widths, rounding, shift amounts, loop bounds, accumulation order, and thresholds are preserved without drift.
2. **Twin compatibility (ADR-1135)**: GPU twins (CUDA, SYCL, HIP) share signatures with CPU headers (`cambi_internal.h`, `speed_internal.h`). Exposed signatures are preserved to pass `scripts/ci/twin-drift-check.sh`.
3. **ADR-1138 compatibility**: C TUs preserve `NULL` using file-scoped `/* NOLINTBEGIN(modernize-use-nullptr) */` / `/* NOLINTEND(modernize-use-nullptr) */` brackets.
4. **ADR-0278 cross-TU registry**: Feature extractor definitions (`vmaf_fex_*`) require external linkage for registry inclusion by `feature_extractor.cpp`.

## Decision

Refactor all 8 SPEED and CAMBI translation units and headers to 0 clang-tidy warnings:

1. **Header Hygiene**: Include internal headers (`speed_avx2.h`, `speed_avx512.h`, `cambi_avx512.h`) to declare external prototypes; remove duplicate declarations.
2. **Function Decomposition**: Split monolithic routines exceeding `readability-function-size` (threshold 60 lines, max nesting 4) into clear, single-purpose static helpers:
   - `calculate_c_values` / `calculate_c_values_avx2`: Split into `c_values_first_pass`, `c_values_top_edge`, `c_values_middle_slide`, `c_values_bottom_edge`.
   - `calculate_c_values_row_avx2` / `calculate_c_values_row_avx512`: Factor chunk accumulation and scalar tail handling into helper functions.
   - `init` in `cambi.c`: Split into `validate_and_setup_dimensions`, `setup_contrast_and_luminance`, `alloc_cambi_buffers`, `open_heatmaps`, and `setup_callbacks`.
   - `decimate_generic_uint16_and_convert_to_10b`: Extract `decimate_same_size_16b`.
   - `est_params` and `speed_init` in `speed.c`: Extract `solve_covariance_system`, `speed_alloc_buffers`, and `speed_dispatch_cpu_kernel`.
   - `quick_select`: Extract `quick_select_partition`.
3. **Type Safety & Const-Correctness**: Add `const` qualifiers to read-only pointers and apply explicit `(ptrdiff_t)` casts on strided array and buffer indexing.
4. **Linkage & Modern C**: Mark file-internal functions `static` (`speed_extract_score`, `speed_init`, `speed_close`). Apply `NOLINTNEXTLINE(misc-use-internal-linkage)` to exported `VmafFeatureExtractor` descriptors per ADR-0278. Remove obsolete `_USE_MATH_DEFINES`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Complete rewrite in C++23 | Modern idioms, RAII buffers | Breaks upstream diff alignment; violates twin prototype contracts; high risk of subtle numeric drift | Rejected: violates bit-exact parity requirement |
| Suppress warnings via file-wide NOLINT | Minimal diff | Leaves code debt in upstream-mirror TUs; fails quality gate intent | Rejected: maintainer explicitly ordered full rework |
| Incremental per-file rework | Smaller individual commits | Leaves SPEED/CAMBI subsystems in mixed state; duplicate effort verifying bit-exactness | Rejected: unified wave 2 rework ensures holistic verification |

## Consequences

- **Positive**:
  - Total clang-tidy warnings in SPEED and CAMBI TUs reduced from 104 to 0.
  - All Netflix golden data assertions pass without drift (271 passed).
  - Max-precision (`--precision=max`) verification against master demonstrates exact 17-digit identity across all test sequences and feature configurations.
  - Passes `scripts/ci/twin-drift-check.sh` and full pre-commit hooks.
- **Negative**:
  - Future upstream syncs of `speed.c` or `cambi.c` will require manual conflict resolution due to function splits.
- **Neutral / follow-ups**:
  - Document rebase invariants in `docs/rebase-notes.md` and `core/src/feature/AGENTS.md`.

## References

- ADR-0141: Touched file cleanup rule.
- ADR-0278: Cross-TU registry pattern for feature extractors.
- ADR-1135: CI twin-drift gate (`scripts/ci/twin-drift-check.sh`).
- ADR-1138: C translation units keep `NULL` (`modernize-use-nullptr` scoping).
- Maintainer mandate: "we still have upstream code that isnt reworked to our standards -> do it, nothing is save anymore as long as the goldens pass" (2026-09-02).
