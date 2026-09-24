<!-- markdownlint-disable MD013 MD022 MD032 MD060 -->
# Research-2093: Resolution of CodeQL cpp/include-non-header alerts

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

- `range_foot_head` and `normalize_range` retain anonymous-namespace linkage. Narrow `vmaf_luminance_test_range_foot_head` and `vmaf_luminance_test_normalize_range` trampolines are declared in internal header `core/src/feature/luminance_tools.h`.
- `test_luminance_tools.cpp` includes `feature/luminance_tools.h` and links against `libvmaf` via `core/test/meson.build`.
- All 7 unit tests pass without source inclusion.

### 2. Alert 943 (`test_feature.cpp`)

- `feature/feature_name.h` already declared the required test functions (`vmaf_feature_name_from_options` and `vmaf_feature_name_dict_from_provided_features`).
- `test_feature.cpp` includes `feature/feature_name.h` and `dict.h`.
- `core/test/meson.build` adds `../src/feature/feature_name.cpp` to `test_feature` sources.
- All 7 unit tests pass.

### 3. Alert 955 (`test_model.c`)

- `VmafBuiltInModel` remains private to `core/src/model.c`. Narrow count and iterator-version accessors (`vmaf_built_in_model_count_for_test` and `vmaf_built_in_model_version_for_test`) are declared in internal header `core/src/model.h`.
- `BUILT_IN_MODEL_CNT` remains a file-local compile-time expression used only by `model.c`.
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

- Implementation helpers in `core/src/feature/cambi.c` retain their original `static` and `static FORCE_INLINE` linkage.
- Existing GPU-facing `vmaf_cambi_*` trampoline declarations in `core/src/feature/cambi_internal.h` remain intact; `test_cambi.c` and `test_cambi_stage_simd.c` reuse existing prefixed wrappers (`vmaf_cambi_decimate`, `vmaf_cambi_filter_mode`, `vmaf_cambi_spatial_pooling`, `vmaf_cambi_weight_scores_per_scale`, `vmaf_cambi_get_pixels_in_window`) where signatures match.
- Narrowly prefixed internal test trampolines (`vmaf_cambi_test_*`) in `cambi.c` and declared in `cambi_internal.h` provide white-box test access for the remaining static helpers and stage functions (`vmaf_cambi_test_anti_dithering_filter`, `vmaf_cambi_test_get_tvi_for_diff`, `vmaf_cambi_test_quick_select`, `vmaf_cambi_test_calculate_c_values`, etc.).
- Circular trampoline calls are eliminated (`calculate_c_values_default` invokes `calculate_c_values` directly).
- Unprefixed declarations, test-only macros (`DEFAULT_CAMBI_TVI`, `NUM_SCALES`), and test-only enums are removed from `cambi_internal.h` and kept private to the respective test translation units.
- Removed `#include "feature/cambi.c"` from both `test_cambi.c` and `test_cambi_stage_simd.c`, linking them against `libvmaf`.
- Every allocation-returning link seam is checked before use. The anti-dithering, decimation, filter-mode, spatial-mask, and generic-decimation tests gather their first failure before releasing each successfully owned picture exactly once; c-values configuration releases contrast arrays when luminance initialization fails.
- All four touched dual-use internal headers (`cambi_internal.h`,
  `luminance_tools.h`, `model.h`, and `libvmaf_priv.h`) are strict clang-tidy
  clean when each header is analyzed directly in both C23 and C++26 modes.
  C++ selects the standard C++ headers and alias spelling while C retains its
  compatibility headers and typedef spelling. A build-only C/C++ smoke target
  pins the internal enum widths to their existing unsigned-int ABI.
- All numerical operation orderings, bounded-search tests, and SIMD stage coverage are preserved; all 25 `test_cambi` tests and 14 `test_cambi_stage_simd` tests pass bit-exact.

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
- **Fast test suite**: the CPU-only GCC 15 build (`enable_cuda=false`,
  `enable_sycl=false`, `b_lto=false`) ran 147 tests with
  `meson test -C build --suite=fast`; 147 passed (0 failures).
- **Dynamic-symbol gate**: `meson test -C build check_exported_symbols` passed; no internal test seam entered the public ABI.
- **Netflix golden assertions**: the exact `make test-netflix-golden` five-file
  gate (`quality_runner_test.py`, `feature_extractor_test.py`,
  `vmafexec_test.py`, `vmafexec_feature_extractor_test.py`, and
  `result_test.py`) executed 283 tests: 271 passed, 12 skipped, 0 failures. No
  golden scores moved.
- **CodeQL non-header scan**: Confirmed zero non-header includes in all seven alerted test files.
- **CAMBI implementation prefix**: the 71,030-byte prefix through the existing
  `vmaf_fex_cambi` descriptor is byte-identical to the merge base (SHA-256
  `debb1c46961b82cfce380a7a9c8757aa95cf4166a790166f5aaa94f372d30c00`).
- **Strict tidy evidence**: the exact GCC 15 / clang-tidy 22 CPU ratchet reports
  686 observed warnings against a 686-warning baseline, zero new diagnostics,
  zero compile failures, and zero uncited suppressions. Direct primary-header
  analysis reports zero diagnostics in C23 and C++26 for each of the four
  dual-use headers; this direct check found C++-mode compatibility-header and
  typedef diagnostics that ordinary consuming translation units could not
  expose after the unity includes were removed.
