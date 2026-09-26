<!-- markdownlint-disable MD013 MD060 -->
# Research-2110: Metal float_ms_ssim option and score parity — 2026-09-25

**Status:** Complete

**Authority inspected:** signed candidates `b234f771a` and `d966c1feba`, plus
the exact correction delta on `agent/fix-metal-ms-ssim-review`.

**Scope:** Metal `float_ms_ssim` option parsing (`enable_db`, `clip_db`,
`enable_chroma`, `enable_lcs`), geometry-derived `max_db` ceiling, 3-plane
chroma computation and emission (`float_ms_ssim_cb`, `float_ms_ssim_cr`),
subsampled chroma min-dimension validation (>= 176), fail-closed L/C/S atom
handling, and option-dictionary ownership under ADR-1334. Device-free semantic
execution and mutation contracts complement Apple-Silicon parity scaffolding.
No benchmark, tuning, retraining, or Netflix golden assertion changes.

## Problem Statement

Pre-RC1 audit gap `T-GAP-METAL-MS-SSIM-DB-CHROMA-OPTIONS-2026-09-07` in `docs/state.md` noted:
CPU `float_ms_ssim.c`, SYCL `integer_ms_ssim_sycl.cpp`, and HIP
`integer_ms_ssim_hip.c` expose `enable_db`, `clip_db`, and `enable_chroma`;
CUDA exposes the two dB controls. The Metal twin
`core/src/feature/metal/float_ms_ssim_metal.mm` only exposed `enable_lcs`.
Any attempt to request dB-domain scoring or chroma planes on Metal failed
during option parsing with `-EINVAL`. Furthermore, `collect_fex_metal` passed
hardcoded `false, INFINITY` to `vmaf_ms_ssim_emit_scores()`.

Review of the first candidate found two additional correctness defects. The
Apple parity test passed the same `VmafFeatureDictionary` to CPU and Metal even
though `vmaf_use_feature()` consumes it; that was a use-after-free followed by
a double-free. The Metal reduction checked only the combined plane score. A
non-finite L/C/S atom at a zero-weight scale can therefore disappear through
`pow(NaN, 0) == 1`, publishing an apparently valid result.

ADR-1221 covers CUDA, SYCL, and HIP and explicitly calls Metal a follow-up.
ADR-1334 records this extension rather than rewriting that history.

## Parity architecture and implementation

1. **Option Registration**:
   `enable_db`, `clip_db`, and `enable_chroma` added as `VMAF_OPT_TYPE_BOOL` with default `false` in `options[]`.

2. **Per-Plane Geometry and Buffers**:
   - `MS_SSIM_MAX_PLANES 3` defined.
   - `MsSsimPlaneGeometryMetal` tracks per-scale dimensions, grid sizes, and workgroup partial block counts for each plane.
   - For `enable_chroma = false` (or YUV400P), `n_planes = 1`. For `enable_chroma = true`, `n_planes = 3`.
   - Separate pyramid buffers (`pyramid_ref[p][scale]`, `pyramid_cmp[p][scale]`) and partials buffers (`l_partials[p][scale]`, `c_partials[p][scale]`, `s_partials[p][scale]`) allocated for each active plane.
   - `alloc_metal_buffers` and `release_metal_buffers` cleanly handle all planes with complete error recovery and deallocation on failure.

3. **Chroma Dimension Validation**:
   - The 5-level 11-tap pyramid requires every dimension to be >= 11 * 2^4 = 176.
   - `check_chroma_min_dim` enforces that subsampled chroma planes are at
     least 176x176. Plane allocation uses ceil subsampling, so the exact
     YUV420P luma minimum is 351x351; 352x352 is merely the next even-sized
     input. If a scored plane is smaller, init returns `-EINVAL` with an
     informative error log. YUV400P resolves to one active plane and bypasses
     the chroma check even when the option was requested.

4. **dB conversion and `max_db` ceiling (ADR-1334 extending ADR-1221)**:
   - When `clip_db` is enabled: `max_db = ceil(10. * log10(peak * peak / mse))` where `peak = (1 << bpc) - 1` and `mse = 0.5 / (w * h)`.
   - When `!clip_db`: `max_db = INFINITY`.
   - The framework-free `float_ms_ssim_option_semantics.h` owns this formula
     and active-plane/plane-geometry rules; production and a device-free C test
     compile the same functions.
   - In `collect_fex_metal`, scores are validated with `vmaf_ssim_prepare_score_named(name, raw_score, s->enable_db, s->max_db, index, &score)`.
   - Scores emitted using `vmaf_ms_ssim_emit_scores` (plane 0) and `vmaf_ssim_emit_score_named` (planes 1 and 2), passing `s->enable_db, s->max_db`.

5. **Dispatch Strategy & Features**:
   - `provided_features` in `float_ms_ssim_metal.mm` updated with `"float_ms_ssim"`, `"float_ms_ssim_cb"`, `"float_ms_ssim_cr"`.
   - `g_metal_features` in `core/src/metal/dispatch_strategy.c` updated to register `"float_ms_ssim_cb"` and `"float_ms_ssim_cr"`.

6. **NASA Rule 4 Adherence**:
   - All helper functions decomposed so every function is <= 60 LOC and cyclomatic complexity <= 10.
   - Validated by unit test in `test_metal_ms_ssim_options_contract.py`.

7. **Failure and ownership semantics**:
   - `reduce_plane_means()` passes each scale's L/C/S triple through
     `vmaf_feature_validate_finite_scores_named()` before the first `pow()`.
     This applies to chroma and to `enable_lcs=false`.
   - CPU and Metal parity runners accept an immutable option specification and
     independently call `make_options()`. Each relinquishes the dictionary
     immediately after `vmaf_use_feature()` and cleans up all still-owned state
     on failure.
   - The contract rejects both sharing a consumed dictionary and explicitly
     freeing it after consumption. The double-free mutant runs before the
     local pointer is cleared, so it exercises the dangling owner rather than
     becoming a harmless `free(NULL)`.

## Verification & Test Matrix

- **Device-free semantic and contract suite**:
  - `core/test/test_metal_ms_ssim_option_semantics.c` executes the same helper
    production uses. Its worked oracles distinguish the correct 105 dB ceiling
    at 512x384 from a geometry-free or unbounded implementation, distinguish
    3-plane chroma from luma-only behavior, and distinguish ceil subsampling at
    odd dimensions from truncation.
  - `core/test/test_metal_ms_ssim_options_contract.py` checks option metadata,
    helper wiring, dispatch names, ownership, atom-validation ordering, and
    NASA Rule 4 limits. Its mutations remove the atom guard, reduce its count
    from all three L/C/S atoms to one, substitute or remove each individual
    L/C/S name/value mapping, replace the dB ceiling with infinity, force one
    plane, restore the raw `enable_chroma` gate, remove fresh dictionary
    construction, and free a consumed dictionary; every mutation is rejected.
  - `core/test/test_nonfinite_collector_wiring.py`: updated with `metal/float_ms_ssim_metal.mm` in `REQUIRED["vmaf_ssim_prepare_score_named"]` and option-aware `METAL_MS_SSIM_OPTIONS_DB_CALL`.
- **Metal Parity Suite**:
  - `core/test/test_metal_float_ms_ssim_parity.c`: upgraded fixture to 512x384 (chroma 256x192 >= 176). Added `test_metal_float_ms_ssim_clip_db_ceiling` and `test_metal_float_ms_ssim_parity_chroma`. Skips honestly off Apple hardware (`[skip: no Metal device]`).
- **Repository gates**: focused evidence is recorded by the correction commit;
  Apple hardware remains unavailable, so the device parity test's skip is not
  represented as a measured pass.
