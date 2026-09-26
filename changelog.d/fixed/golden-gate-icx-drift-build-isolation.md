# Isolate Netflix golden gate build profile to eliminate ICX floating-point drift

`make test-netflix-golden` previously inherited the generic developer build
directory `BUILD_DIR = core/build`. On development workstations where `core/build`
was configured using Intel oneAPI compilers (`icx`/`icpx`, identified as `intel-llvm`),
relaxed floating-point contraction (`-ffp-contract=on`) produced slight rounding
deltas (up to `8.2e-05`) in float-motion and float-VIF convolutions, causing 11
assertions in `quality_runner_test.py` and `vmafexec_test.py` to fail falsely against
the `places=4` golden threshold.

Fix:
- Isolate the golden gate build into a dedicated CPU-only profile (`GOLDEN_BUILD_DIR ?= core/build-golden`).
- Enforce an explicitly supported compiler (`gcc` or `clang`) via `scripts/ci/setup-golden-build.sh`, failing closed if an unsupported compiler like `intel-llvm` is configured.
- Update `test-netflix-golden` in `Makefile` to depend on `build-golden` and pass `VMAF_BUILD_DIR="$(CURDIR)/$(GOLDEN_BUILD_DIR)"`.
- Decouple Python harness binary discovery in `compat/python-vmaf/__init__.py` and `compat/python-vmaf/config.py` to honour `VMAF_BUILD_DIR`, `VMAF_PATH`, and `VMAFEXEC_PATH`.
- Update `clean` target in `Makefile` and `.gitignore` to track `build-golden`.
- Verified: all 271 Netflix golden assertions pass cleanly without modifying any golden score or test assertion.
- State item: closes `T-GOLDEN-GATE-ICX-FP-DRIFT-2026-09-05` (ADR-1317).
