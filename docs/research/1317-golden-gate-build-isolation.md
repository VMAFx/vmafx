<!-- markdownlint-disable MD013 -->
# Research-1317: Netflix Golden Gate Build Isolation and Compiler FP Contraction Drift

**Item**: `T-GOLDEN-GATE-ICX-FP-DRIFT-2026-09-05`
**Date**: 2026-09-25
**Author**: VMAFx maintainers
**ADR**: [ADR-1317](../adr/1317-golden-gate-build-isolation.md)

## Summary

When running the local preflight or regression gate `make test-netflix-golden` (D24)
on a development workstation whose `core/build` tree was configured using Intel oneAPI
compilers (`icx`/`icpx`, identified as `intel-llvm` in Meson compiler introspection),
the gate reported:

```text
FAILED python/test/quality_runner_test.py::QualityRunnerTest::test_run_vmaf_runner_with_transform_score
FAILED python/test/quality_runner_test.py::QualityRunnerTest::test_run_vmaf_runner_with_transform_score2
FAILED python/test/quality_runner_test.py::QualityRunnerTest::test_run_vmaf_runner_with_transform_score3
FAILED python/test/quality_runner_test.py::QualityRunnerTest::test_run_vmaf_runner_with_transform_score4
FAILED python/test/quality_runner_test.py::QualityRunnerTest::test_run_vmaf_runner_with_transform_score5
FAILED python/test/quality_runner_test.py::QualityRunnerTest::test_run_vmaf_runner_with_transform_score6
FAILED python/test/vmafexec_test.py::VmafexecTest::test_run_vmafexec_with_transform_score
FAILED python/test/vmafexec_test.py::VmafexecTest::test_run_vmafexec_with_transform_score2
FAILED python/test/vmafexec_test.py::VmafexecTest::test_run_vmafexec_with_transform_score3
FAILED python/test/vmafexec_test.py::VmafexecTest::test_run_vmafexec_with_transform_score4
FAILED python/test/vmafexec_test.py::VmafexecTest::test_run_vmafexec_with_transform_score5
=========================== 11 failed, 260 passed, 12 skipped ===========================
```

This failure occurred on every branch regardless of the changes under test, producing
misleading false alarms on developer workstations while passing in CI (which builds
under GCC and Clang).

## Root Cause Analysis

1. **Compiler Optimization and FP Contraction**:
   - Intel `icx` defaults to `-ffp-contract=on` (relaxed floating-point contraction).
   - GCC and standard Clang default to `-ffp-contract=off`.
   - In floating-point convolution loops (`float_motion` and `float_vif`), `icx` contracts
     floating-point multiplications and additions into fused multiply-add (FMA) instructions.
   - The resulting intermediate rounding differences produce deltas of `7.1e-05` to `8.2e-05`.
   - The Netflix golden test suite asserts score equality via `assertAlmostEqual(..., places=4)`,
     which checks `round(a - b, 4) == 0` (ceiling ~0.00005). The `7.1e-05` delta exceeds
     this threshold, failing the assertion.

2. **Coupling Between Developer Build and Golden Gate**:
   - `Makefile` previously defined:

     ```makefile
     test-netflix-golden: build
         CUDA_VISIBLE_DEVICES="" VMAF_FORCE_BACKEND=cpu PYTHONPATH=$(CURDIR)/python python3 -m pytest ...
     ```

   - Target `build` operates on `BUILD_DIR = core/build`.
   - If a developer had configured `core/build` with `icx` to test SYCL or oneAPI targets,
     `test-netflix-golden` reused that binary directly.
   - `compat/python-vmaf/__init__.py` hardcoded the search path:

     ```python
     vmafexec = project_path(os.path.join("core", "build", "tools", "vmaf"))
     ```

     leaving no mechanism for the test harness to point to an alternate build profile.

## Repair Architecture

1. **Build Directory Isolation**:
   - Dedicated `GOLDEN_BUILD_DIR ?= core/build-golden` distinct from `BUILD_DIR`.
   - Target `build-golden` managed via `scripts/ci/setup-golden-build.sh`.
   - Clean target updated to remove `$(GOLDEN_BUILD_DIR)`.
   - `.gitignore` updated with `core/build-golden/` and `build-golden/`.

2. **Supported Compiler Enforcement (Fail-Closed)**:
   - `scripts/ci/setup-golden-build.sh` probes the compiler ID using `intro-compilers.json`.
   - Permitted compilers for the golden profile are `gcc` and `clang`.
   - When configured with `intel-llvm` or other non-deterministic compilers, the script fails
     immediately with an actionable error.
   - Provides `--check-compiler <dir>` mode for hermetic validation.

3. **Python Harness Environment Redirection**:
   - `compat/python-vmaf/__init__.py` reads `VMAF_BUILD_DIR` from `os.environ`, defaulting to `core/build`.
   - `compat/python-vmaf/config.py` supports `VMAF_PATH` and `VMAFEXEC_PATH` overrides.
   - `test-netflix-golden` passes `VMAF_BUILD_DIR="$(CURDIR)/$(GOLDEN_BUILD_DIR)"`.

## Verification Results

1. **Isolation & Contract Test Suite**:
   - `python/test/golden_gate_isolation_test.py`: 9/9 tests pass (default paths, custom build paths, absolute paths, precedence over config, script executability, gcc/clang accept, intel-llvm reject, missing intro-compilers fail-closed).
   - `scripts/ci/tests/test_golden_gate_makefile_contract.py`: 5/5 tests pass (variable definition, target definition, prereq isolation, recipe export, clean target).

2. **Real Golden Gate Execution**:
   - Executing `make test-netflix-golden` against `core/build-golden`:

     ```text
     ============ 271 passed, 12 skipped, 1 warning in 187.88s (0:03:07) ============
     ```

   - 100% green without touching any Netflix assertion or baseline fixture.
