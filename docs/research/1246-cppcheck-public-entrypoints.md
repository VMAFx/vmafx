<!-- markdownlint-disable MD013 MD060 -->
# Cppcheck public entrypoint model

The `fb793901` exhaustive CPU profile contains 53 unused-function findings.
Sixteen functions are public roots: each has a `VMAF_EXPORT` declaration in a
header selected by `core/include/libvmaf/meson.build`, and the retained CPU
library exports every name. Header installation is conditional for backend and
DNN options; `model.h` is unconditional. The model addresses missing external
call sites, not implementation or numerical behavior.

## Verified initial roots

| Symbol | Public declaration | CPU definition |
| --- | --- | --- |
| `vmaf_dnn_is_codec_aware` | `core/include/libvmaf/dnn.h` | `core/src/dnn/dnn_attach_api.c` |
| `vmaf_hip_available` | `core/include/libvmaf/libvmaf_hip.h` | `core/src/hip/stubs.c` |
| `vmaf_hip_state_init` | `core/include/libvmaf/libvmaf_hip.h` | `core/src/hip/stubs.c` |
| `vmaf_hip_import_state` | `core/include/libvmaf/libvmaf_hip.h` | `core/src/hip/stubs.c` |
| `vmaf_hip_state_free` | `core/include/libvmaf/libvmaf_hip.h` | `core/src/hip/stubs.c` |
| `vmaf_hip_list_devices` | `core/include/libvmaf/libvmaf_hip.h` | `core/src/hip/stubs.c` |
| `vmaf_metal_available` | `core/include/libvmaf/libvmaf_metal.h` | `core/src/metal/stubs.c` |
| `vmaf_metal_state_init` | `core/include/libvmaf/libvmaf_metal.h` | `core/src/metal/stubs.c` |
| `vmaf_metal_import_state` | `core/include/libvmaf/libvmaf_metal.h` | `core/src/metal/stubs.c` |
| `vmaf_metal_state_free` | `core/include/libvmaf/libvmaf_metal.h` | `core/src/metal/stubs.c` |
| `vmaf_metal_list_devices` | `core/include/libvmaf/libvmaf_metal.h` | `core/src/metal/stubs.c` |
| `vmaf_metal_state_init_external` | `core/include/libvmaf/libvmaf_metal.h` | `core/src/metal/stubs.c` |
| `vmaf_metal_picture_import` | `core/include/libvmaf/libvmaf_metal.h` | `core/src/metal/stubs.c` |
| `vmaf_metal_wait_compute` | `core/include/libvmaf/libvmaf_metal.h` | `core/src/metal/stubs.c` |
| `vmaf_metal_read_imported_pictures` | `core/include/libvmaf/libvmaf_metal.h` | `core/src/metal/stubs.c` |
| `vmaf_default_model_version` | `core/include/libvmaf/model.h` | `core/src/model.c` |

## Mechanism and controls

Cppcheck format-2 accepts top-level `<entrypoint name="exact_symbol"/>` nodes.
Both unused-function analysis paths compare the configured name directly.
The 2.21.1 real-tool controls verify direct and build-directory analysis:

- A listed exported function with no internal callers stops producing
  `unusedFunction`; an unlisted private helper still fails.
- An uninitialized read inside a listed function still fails.
- An invalid model fails instead of silently disabling the policy.
- A same-named static function is also treated as an entrypoint. This is a
  measured name-only limitation, not a proof that Cppcheck understands linkage.

The initial root list deliberately excludes Pelorus denoise helpers: their
header is not installed and they lack `VMAF_EXPORT`. It also excludes private
CAMBI/ADM helpers without callers. The feature-vector score helper has a real
consumer in generated C++ test source, so a native-file-only search would miss
its dependency. Preserve these distinctions before changing source.

## Compatibility and limits

Ubuntu Noble publishes Cppcheck 2.13.0-2ubuntu3. Its tagged parser accepts
`--check-level=exhaustive`; its schema, library parser and both unused-function
paths support format-2 entrypoints. The local real-tool tests use 2.21.1.
This is source-level compatibility verification for Noble, not a hosted apt
execution or proof of identical findings across tool versions.

The local driver retains `--enable=all`; remote CI retains its existing
warning/performance/portability selection. Do not represent this model as
new remote unused-function coverage, or widen it to all 53 findings.

The diagnostic ledger, source excerpts, real-tool XML reports and rejected
same-name collision control are retained under
`.workingdir2/cache/cppcheck-unused-diagnosis-20260908/`; implementation
validation is retained under `.workingdir2/evidence/cppcheck-public-entrypoints-2026-09-08/`.

## Sources

- [Cppcheck 2.21.1 schema](https://github.com/danmar/cppcheck/blob/2.21.1/cfg/cppcheck-cfg.rng).
- [Cppcheck 2.21.1 library parser](https://github.com/danmar/cppcheck/blob/2.21.1/lib/library.cpp).
- [Cppcheck 2.21.1 unused-function analysis](https://github.com/danmar/cppcheck/blob/2.21.1/lib/checkunusedfunctions.cpp).
- [Cppcheck 2.13 option parser](https://github.com/danmar/cppcheck/blob/2.13.0/cli/cmdlineparser.cpp).
- [Cppcheck 2.13 model schema](https://github.com/danmar/cppcheck/blob/2.13.0/cfg/cppcheck-cfg.rng).
- [Ubuntu Noble package](https://packages.ubuntu.com/noble/cppcheck).

## Implementation validation

The only production analyzer change is the shared model argument in each
existing path. The configured-driver suite passes 20 tests, including model
schema/declaration checks, invalid/private/uninstalled-name negatives, conditional-list consumption, header
and model hook routing, real Make execution, all command variants and unchanged
build databases. An impact-planner regression confirms cfg-only, driver-only
and control-only changes each enable `c_core`; the existing `scripts/ci/**`
full-routing policy needs no configuration change. The existing real-tool suite passes 10 tests with Cppcheck
2.21.1: listed-only functions pass; unlisted private functions remain diagnosed
in direct and build-directory modes; listed bodies retain uninitialized-read
failures; missing/malformed files fail; the static-name collision limitation is
explicitly measured. Other existing pthread, constructor and branch-budget
controls remain present. Tests are CPU-only analyzer controls, not a fresh
native build, whole-tree lint, hosted CI or backend-runtime acceptance.

The required Pre-Commit job runs the lightweight declaration/schema/driver
suite. Actual Cppcheck controls remain in the existing Cppcheck job after its
tool installation, so Pre-Commit does not acquire an undeclared analyzer
installation prerequisite. Local and remote severity selections are unchanged.

No native source, public header, callback, feature calculation, Netflix golden
assertion, FFmpeg patch or lint baseline changes. The accompanying CI guide's
FFmpeg overview removes a stale Vulkan leg after checking the actual workflow:
its ordinary matrix is Linux GCC/macOS Clang with a separate SYCL build leg.
ADR-1246's alternatives table supplies the decision matrix; the script invariant,
rebase note, changelog fragment and [verification commands in the CI guide](../development/ci.md#local-lint-build-profile-and-receipts) complete the deliverables.

## Offline Make fixture correction

The original real-Make tests passed on a networked host but failed in the
isolated acceptance container: recursive `make build` did not inherit the
outer `-o bin/meson -o bin/ninja`, and the missing `fixture-venv/bin/pip`
prerequisite triggered actual venv creation and package installation. The
outer flags alone did not enforce the comment's no-bootstrap claim. Plain
GNU Make 4.4.1 reproduces both failures, independently of the acceptance
harness's Make wrapper.

Create a failing, recording pip sentinel before the fake Meson/Ninja files.
The real dependency graph now sees already-provisioned prerequisites in both
Make processes. Assert that pip was not called or overwritten and no real
`pyvenv.cfg` appeared. `PIP_NO_INDEX=1` adds an offline guard. Keep the actual
recursive Make call, reconfigure/build stubs, all command variants and original
native/polluted database assertions unchanged.

Using the same pinned CPU tool image, plain GNU Make and disabled Docker
network, the original 20-case suite fails its two Make fixtures; the corrected
suite passes all 20. Evidence and exact commands live under
`.workingdir2/evidence/configured-lint-fixture-offline-2026-09-08/`.
This is fixture hermeticity, not a production Makefile, analyzer, native-source,
backend or baseline change. No alternatives: satisfy the existing dependency
contract; no new ADR or user-facing surface is introduced.
