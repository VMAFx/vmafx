<!-- markdownlint-disable MD013 MD060 -->
# ADR-1323: Reproducible GPU tidy ratchet configuration and fail-closed guards

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `gpu`, `hip`, `sycl`, `cuda`, `clang-tidy`, `ratchet`, `makefile`

## Context

The clang-tidy whole-tree ratchet (ADR-1142) maintains baselines across multiple compilation lanes: `cpu`, `cuda`, `sycl`, `hip`, and `arm64`.

Row `T-TIDY-RATCHET-GPU-LANES-UNREPRODUCIBLE-2026-09-22` identified two reproducibility and enforcement gaps:

1. **LTO Incompatibility**: The project's default build configuration enables link-time optimization parallelism via `b_lto_threads=4` (ADR-1172), which meson renders as GCC's `-flto=4`. Clang rejects this argument during static analysis (`unsupported argument '4' to option '-flto='`), causing every translation unit to fail compilation analysis. While CPU CI lanes worked around this manually, automated enforcement is now applied to all lanes without exception.
2. **Build Directory Authority**: In-repo build directories place generated translation units (e.g. `*_hsaco.c`, `*.json.c`) directly into the measured source tree, altering measured translation unit counts and warning distributions against committed baselines.
3. **Environmental Drift**: When running `make tidy-ratchet LANE=hip` against an out-of-repo directory without `-Db_lto=false` enforcement, slight environmental drift in compiler libraries caused mismatches against stale baseline data.

## Decision

1. **Automated Fail-Closed Build Directory Validation**:
   Introduce `scripts/ci/check-tidy-build-dir.py` and wire it into the `Makefile` recipes for `tidy-ratchet` and `tidy-ratchet-write`.
   For GPU lanes (`cuda`, `hip`, `sycl`):
   - Enforce that `TIDY_RATCHET_BUILD_DIR` is outside the repository root. If inside, fail immediately before compilation database generation or measurement.
   - Inspect `meson-info/intro-buildoptions.json` and verify `-Db_lto=false`. If `b_lto` is true or missing, fail closed with an actionable error.
2. **Recipe Hook Ordering**:
   Preserve the strict ordering required by `scripts/ci/tests/test_tidy_ratchet_sycl_compdb.py`:
   `check-tidy-build-dir` < `write-compile-commands` < `TIDY_RATCHET_COMPDB_<LANE>` < `tidy-ratchet.py`.
3. **Reconcile Baseline Measurements**:
   Re-record `scripts/ci/tidy-baseline-hip.json` from the documented and validated `-Db_lto=false` out-of-repo configuration (`/tmp/tidy-hip`), ensuring clean zero-delta comparison on developer workstations and CI runners (360 TUs, 1310 warnings, reconciling the addition of 12 MEX translation units from ADR-1322 to the prior 348 TUs / 1253 warnings baseline). Update `scripts/ci/tidy-baseline-cuda.json` (391 TUs, 1897 warnings) to incorporate the 12 MEX units under the verified out-of-repo configuration. CPU baselines are rebuilt strictly from clean CPU-only CI compilation configurations (312 TUs, 867 warnings).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Rely solely on Makefile documentation comments | No code changes | Developers inadvertently run in-repo or with default LTO, failing with opaque clang errors | Rejected: fail-closed gates must prevent misconfigurations |
| Automatically reconfigure meson in Makefile | Completely invisible | Silently mutates developer build trees; unexpected side effects | Rejected: explicit, deterministic authority over build directories |
| Disable LTO globally across the entire project | Avoids special cases | Penalizes optimized release binaries | Rejected: LTO is required for optimized production libraries (ADR-1172) |

## Consequences

- **Positive:** GPU ratchet lanes (`cuda`, `hip`, `sycl`) fail early with clear, actionable diagnostics if misconfigured.
- **Positive:** generated files from in-tree builds cannot silently pollute the measured GPU TU sets.
- **Positive:** `scripts/ci/tidy-baseline-hip.json`, `scripts/ci/tidy-baseline-cuda.json`, and `scripts/ci/tidy-baseline-cpu.json` are aligned with the documented reproducible configurations and exact measured counts.
- **Negative:** developers must specify an out-of-repo build directory when invoking GPU tidy targets.
- **Neutral:** CPU and ARM64 lanes now identically require -Db_lto=false to prevent LTO parsing errors.
