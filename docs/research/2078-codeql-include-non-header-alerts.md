<!-- markdownlint-disable MD013 MD022 MD032 MD060 -->
# Research-2078: Resolution of CodeQL cpp/include-non-header alerts

## Scope and alert background

CodeQL rule `cpp/include-non-header` flags direct inclusion of source files (`.c`, `.cpp`) within other translation units. Seven alerts on `VMAFx/vmafx` master targeted unit tests in `core/test/`:

| Alert | File | Included TU | Historical rationale |
|---|---|---|---|
| **908** | `core/test/test_luminance_tools.cpp` | `feature/luminance_tools.cpp` | Direct white-box coverage of unexported helpers `range_foot_head` and `normalize_range` |
| **943** | `core/test/test_feature.cpp` | `feature/feature_name.cpp` | ADR-0729 wave 3 renaming from `.c` to `.cpp`; unity inclusion drove TU-local helpers |
| **955** | `core/test/test_model.c` | `model.c` | Inspection of static built-in model array size and private model structures |
| **1043** | `core/test/test_flush_context_ordering.c` | `libvmaf.c` | White-box verification of `VmafContext.flushed` flag and internal `flush_context*` execution paths |
| **1203** | `core/test/test_flush_context_ordering.c` | `feature_collector.c` | Direct symbol availability for feature collector management alongside `libvmaf.c` |
| **1218** | `core/test/test_cambi_stage_simd.c` | `feature/cambi.c` | Bit-exact reference against shipped scalar CAMBI stages (ADR-1207) |
| **1241** | `core/test/test_cambi.c` | `feature/cambi.c` | Fine-grained unit tests for TVI thresholding, quick-select bounds, and mask generation |

## Seam design and implementation

Rather than suppressing findings via comments (`// NOLINT`, `// codeql[...]`) or deleting white-box tests, each site was refactored into a proper internal header and link-time seam. None of these changes expose private symbols through public API headers (`core/include/libvmaf/`).

### 1. Alert 908 (`test_luminance_tools.cpp`)

- `range_foot_head` and `normalize_range` were given `extern "C"` linkage and declared in internal header `core/src/feature/luminance_tools.h`.
- `test_luminance_tools.cpp` includes `feature/luminance_tools.h` and links against `libvmaf` via `core/test/meson.build`.
- All 7 unit tests pass without source inclusion.

### 2. Alert 943 (`test_feature.cpp`)

- `feature/feature_name.h` already declared the required test functions (`vmaf_feature_name_from_options` and `vmaf_feature_name_dict_from_provided_features`).
- `test_feature.cpp` includes `feature/feature_name.h` and `dict.h`.
- `core/test/meson.build` adds `../src/feature/feature_name.cpp` to `test_feature` sources.
- All 7 unit tests pass.

### 3. Alert 955 (`test_model.c`)

- `typedef struct VmafBuiltInModel` and function `unsigned vmaf_built_in_model_count(void)` were declared in internal header `core/src/model.h` and implemented in `core/src/model.c`.
- `BUILT_IN_MODEL_CNT` in `model.h` delegates to `vmaf_built_in_model_count()` for external consumers while retaining constant-expression evaluation inside `model.c`.
- `test_model.c` includes internal `model.h` and links against `libvmaf.get_static_lib()`.
- All 62 unit tests pass.

### 4. Alerts 1043 & 1203 (`test_flush_context_ordering.c`)

- `core/src/libvmaf_priv.h` was extended with test accessors:
  - `bool vmaf_context_is_flushed(const VmafContext *vmaf);`
  - `bool vmaf_context_has_thread_pool(const VmafContext *vmaf);`
  - `int vmaf_context_flush_threaded_for_test(VmafContext *vmaf);`
  - `int vmaf_context_flush_for_test(VmafContext *vmaf);`
- Implemented in `core/src/libvmaf.c`.
- `test_flush_context_ordering.c` includes `libvmaf_priv.h` and links against `libvmaf`, removing both `#include "feature_collector.c"` and `#include "libvmaf.c"`.
- All 3 flush ordering tests pass.

### 5. Alerts 1218 & 1241 (`test_cambi_stage_simd.c` and `test_cambi.c`)

- `core/src/feature/cambi_internal.h` was established previously to share CAMBI routines with GPU twins (CUDA/HIP).
- Expanded `cambi_internal.h` to declare:
  - Scalar processing stages (`decimate_row_scalar`, `anti_dithering_filter_row_scalar`, `filter_mode_row_scalar`, `compute_dp_row_scalar`, etc.)
  - Numerical helpers (`spatial_pooling`, `quick_select`, `average_topk_elements`, `get_pixels_in_window`, `weight_scores_per_scale`, `adjust_window_size`, `get_tvi_for_diff`, `tvi_condition`, `set_contrast_arrays`, `tvi_hard_threshold_condition`, `get_vlt_luma`)
  - Stage callback typedefs and `enum CambiTVIBisectFlag`
- Removed `#include "feature/cambi.c"` from both `test_cambi.c` and `test_cambi_stage_simd.c`, linking them with `libvmaf`.
- All 25 `test_cambi` tests and 14 `test_cambi_stage_simd` tests pass.

## Alternatives considered

| Approach | Result | Decision |
|---|---|---|
| In-code suppression (`// NOLINT(bugprone-suspicious-include)`, `// codeql[...]`) | Merely silences scanner, preserves unity antipattern, risks ODR violations | Rejected |
| Delete white-box tests | Loses regression coverage for TVI search bounds, quick-select extremes, and flush ordering | Rejected |
| Expose test hooks in public headers (`include/libvmaf/`) | Pollutes public API surface with test-only and internal implementation details | Rejected |
| Internal headers + link seams | Completely eliminates non-header includes, preserves 100% test fidelity, keeps public API clean | Chosen |

## Verification evidence

- **Affected test suites**:
  - `test_luminance_tools`: 7/7 passed
  - `test_feature`: 7/7 passed
  - `test_model`: 62/62 passed
  - `test_flush_context_ordering`: 3/3 passed
  - `test_cambi`: 25/25 passed
  - `test_cambi_stage_simd`: 14/14 passed
  - Total: 118/118 tests passed.
- **Fast test suite**: `meson test -C build --suite=fast` ran 144 tests, 144 passed (0 failures).
- **Netflix golden assertions**: `pytest python/test/` executed 283 tests: 271 passed, 12 skipped, 0 failures. No golden scores moved.
- **CodeQL non-header scan**: Confirmed zero non-header includes in all seven alerted test files.
