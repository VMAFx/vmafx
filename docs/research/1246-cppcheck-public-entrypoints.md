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
validation and its durable evidence index follow in the same PR.

## Sources

- [Cppcheck 2.21.1 schema](https://github.com/danmar/cppcheck/blob/2.21.1/cfg/cppcheck-cfg.rng).
- [Cppcheck 2.21.1 library parser](https://github.com/danmar/cppcheck/blob/2.21.1/lib/library.cpp).
- [Cppcheck 2.21.1 unused-function analysis](https://github.com/danmar/cppcheck/blob/2.21.1/lib/checkunusedfunctions.cpp).
- [Cppcheck 2.13 option parser](https://github.com/danmar/cppcheck/blob/2.13.0/cli/cmdlineparser.cpp).
- [Cppcheck 2.13 model schema](https://github.com/danmar/cppcheck/blob/2.13.0/cfg/cppcheck-cfg.rng).
- [Ubuntu Noble package](https://packages.ubuntu.com/noble/cppcheck).
