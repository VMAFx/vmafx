<!-- markdownlint-disable MD013 MD060 -->
# ADR-1317: Isolate Netflix golden gate build profile to eliminate floating-point drift from compiler variations

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: VMAFx maintainers
- **Tags**: `ci`, `golden-gate`, `compiler`, `build-system`, `reproducibility`, `floating-point`

## Context

The Netflix CPU golden-data gate (`make test-netflix-golden`, D24) is the repository's
authoritative numerical ground truth, executing 271 assertions whose values originate
from Netflix's reference CPU implementation. Under [ADR-0024](0024-netflix-golden-preserved.md),
these golden assertions and reference scores are immutable and must never be altered.

Historically, `make test-netflix-golden` depended on target `build`, which built the
generic development directory `BUILD_DIR = core/build`. On workstations configured
for heterogeneous accelerator development (CUDA, SYCL, oneAPI), `core/build` is
regularly configured with Intel oneAPI `icx` / `icpx` (reported as `intel-llvm` in
`meson-info/intro-compilers.json`).

Intel `icx` defaults to relaxed floating-point contraction (`-ffp-contract=on`),
whereas GCC defaults to `-ffp-contract=off`. Because floating-point multiply-accumulate
convolutions contract differently under `icx`, `VMAF_feature_motion_score` and
float-VIF components drift by `7.1e-05` to `8.2e-05` against a `places=4` (1e-4) tolerance.
This caused 11 test assertions to fail consistently across `quality_runner_test.py`
and `vmafexec_test.py` on any branch tested against an `icx`-configured `core/build`,
despite the code under test being completely valid.

Furthermore, `compat/python-vmaf/__init__.py` hardcoded the binary search path to
`core/build/tools/vmaf`, preventing the test runner from pointing at an alternate
or isolated build directory.

State item `T-GOLDEN-GATE-ICX-FP-DRIFT-2026-09-05` tracked this defect. Resolving it
required isolating the golden gate into a deterministic CPU build profile with an
explicitly supported compiler (`gcc` or `clang`), or failing loudly if that profile
cannot be created, without altering any Netflix golden assertion.

## Decision

1. **Isolate the Golden Gate Build Directory (`GOLDEN_BUILD_DIR`)**:
   - In `Makefile`, define `GOLDEN_BUILD_DIR ?= $(LIBVMAF_DIR)/build-golden`.
   - Introduce target `build-golden` depending on `$(MESON)` and `$(NINJA)`.
   - Update `clean` target in `Makefile` to clean `$(GOLDEN_BUILD_DIR)`.
   - Add `build-golden/` and `core/build-golden/` to `.gitignore`.

2. **Deterministic Golden Profile Runner (`scripts/ci/setup-golden-build.sh`)**:
   - Create a dedicated script that configures an isolated CPU-only build:
     `--buildtype release -Denable_float=true -Denable_cuda=false -Denable_sycl=false -Denable_hip=false -Denable_dnn=disabled -Denable_tests=false -Denable_docs=false`.
   - Enforce an explicitly supported host C compiler (`gcc` or `clang`), probed automatically
     or overridden via `GOLDEN_CC`. Fail immediately with an actionable error if the configured
     compiler is unsupported (e.g. `intel-llvm`) or unavailable.
   - Provide a `--check-compiler <build_dir>` validator mode for hermetic contract testing.
   - Compile the required `tools/vmaf` executable with Ninja.

3. **Decouple Python Harness from Hardcoded Build Path**:
   - In `compat/python-vmaf/__init__.py`, honour `VMAF_BUILD_DIR` from the environment,
     defaulting to `os.path.join("core", "build")`.
   - In `compat/python-vmaf/config.py`, honour `VMAF_PATH` and `VMAFEXEC_PATH` environment
     overrides, taking precedence when set and valid.
   - Ensure `ExternalProgram` safely queries `config.VmafExternalConfig` even when the
     legacy `externals` module is not installed.

4. **Update Golden Gate Makefile Target**:
   - Update `test-netflix-golden` to depend on `build-golden` instead of generic `build`.
   - Pass `VMAF_BUILD_DIR="$(CURDIR)/$(GOLDEN_BUILD_DIR)"` to `pytest`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Force `-ffp-contract=off` globally on `icx` in `core/meson.build` | Keeps a single build directory | Modifies compiler optimization properties for developer and accelerator builds across all targets; does not guarantee bit-exactness on all architectures. | Does not isolate the gate's numerical requirements from developer-specific build options. |
| Loosen Netflix golden test tolerance to `places=3` | Trivial change | Violates ADR-0024; sacrifices numerical fidelity and upstream Netflix compatibility. | Strict invariant violation. |
| Restrict `test-netflix-golden` to CI only | No local build changes | Local developer verification before push becomes impossible on oneAPI workstations. | Developer preflight must remain viable and authoritative. |
| Refuse to run without building a separate directory | Avoids creating `core/build-golden` | Requires developers to reconfigure their main build tree every time they test golden data. | Poor developer ergonomics; destroys active build configurations. |

## Consequences

- **Positive**:
  - `make test-netflix-golden` passes 271/271 assertions deterministically, regardless of whether `core/build` was configured with oneAPI ICX.
  - Zero modification to any Netflix golden assertion, tolerance, or fixture.
  - Full backward compatibility for developers and CI; ordinary builds (`make`, `make test`) are unaffected.
  - Contract-tested isolation via `scripts/ci/tests/test_golden_gate_makefile_contract.py` and `python/test/golden_gate_isolation_test.py`.
- **Negative**:
  - Adds ~20 MB disk usage for the isolated `core/build-golden` directory when running the golden gate.
- **Neutral / follow-ups**:
  - Closed `T-GOLDEN-GATE-ICX-FP-DRIFT-2026-09-05` in `docs/state.md`.

## References

- [ADR-0024](0024-netflix-golden-preserved.md): Netflix golden data and assertions are immutable.
- [ADR-0700](0700-vmafx-repo-layout.md): `libvmaf` to `core` directory reorganization.
- State item: `T-GOLDEN-GATE-ICX-FP-DRIFT-2026-09-05`.
