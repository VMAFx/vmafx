<!-- markdownlint-disable MD013 MD060 -->

# ADR-1320: CUDA fatbin and HIP HSACO kernel header dependency tracking

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: lusoris
- **Tags**: `build`, `cuda`, `hip`, `gpu`, `dependencies`

## Context

In `core/src/meson.build`, device code generation for the CUDA and HIP backends
is handled by `custom_target` blocks: `cu_ptx_target_*` invokes `nvcc` to
compile `.cu` source files into `.fatbin` binaries, and `hip_hsaco_*` invokes
`hipcc --genco` to generate `.hsaco` objects. Historically, these targets only
declared their primary source file as `input`, with no `depend_files` and no
compiler depfile tracking.

Consequently, when a header file included by device kernels changed (such as
`core/src/feature/cuda/integer_adm_cuda.h` or
`core/src/feature/hip/integer_adm_hip.h`), Ninja was unaware of the dependency.
Incremental builds (`ninja -C build`) reported `ninja: no work to do.`, leaving
stale device binaries linked into `libvmaf.so`. In the case of Integer ADM,
where `AdmFixedParametersCuda` is passed by value into device kernels, a
host-side struct layout change silently paired the new host struct with an
outdated kernel layout. This resulted in silent score drift (e.g., ADM delta ~0.2)
without any compiler diagnostic or build error, requiring developers to manually
`touch` all kernel files before rebuilding (tracked as
`T-CUDA-FATBIN-NO-HEADER-DEP-2026-09-05` in `docs/state.md`).

Furthermore, compiler depfile support differs across platforms:

- `nvcc` supports `-MD -MF @DEPFILE@ -MT @OUTPUT@` on POSIX hosts, but on
  Windows preprocessing delegates through MSVC (`cl.exe`), where standard depfile
  generation flags are not enabled and can cause Ninja to fail on missing depfiles.
- `hipcc` under `--genco` is an `amdclang++` driver script that does not process
  top-level `-MD`/`-MF`, requiring direct clang frontend flags
  (`-Xclang -dependency-file -Xclang @DEPFILE@ -Xclang -MT -Xclang @OUTPUT@`).
- Relying solely on compiler-generated depfiles means that until a target has been
  compiled once, dependency information does not exist, and platforms without
  depfile flags lack tracking entirely.

## Decision

We establish durable, cross-platform header dependency tracking for all 22 CUDA
fatbin and 22 HIP HSACO targets in `core/src/meson.build`:

1. **Declarative shared dependency sets (`depend_files`)**:
   We define `cuda_kernel_shared_headers` and `hip_kernel_shared_headers` using
   Meson's `files()` function, encompassing all shared headers and per-feature
   device headers (such as `cuda/common.h`, `cuda/cuda_helper.cuh`,
   `feature/cuda/integer_adm_cuda.h`, `hip/common.h`,
   `feature/hip/integer_adm_hip.h`, `feature/adm_angle_flag.h`). Every
   `cu_ptx_target_*` and `hip_hsaco_*` custom target specifies
   `depend_files` pointing to its respective shared header list.
2. **Compiler depfile flags for dynamic transitive tracking**:
   - For CUDA on POSIX hosts, `nvcc` passes `-MD -MF @DEPFILE@ -MT @OUTPUT@` with
     `depfile: cu_depfile`.
   - For CUDA on Windows hosts, `cu_depfile` evaluates to `''` and `nvcc_dep_flags`
     remains empty, preventing Ninja from expecting non-existent depfile outputs
     while still enforcing rebuilds through `depend_files`.
   - For HIP, `hipcc` passes `-Xclang -dependency-file -Xclang @DEPFILE@ -Xclang -MT -Xclang @OUTPUT@`
     with `depfile: name + '.hsaco.d'`.
3. **Deterministic device-free verification**:
   We implement `core/test/test_device_target_header_dependencies.py` (registered
   in `core/test/meson.build` under the `fast` suite and mirrored in
   `scripts/ci/tests/test_device_target_header_dependencies.py`). It reproduces
   the RED failure mode (demonstrating that untracked targets fail to rebuild on
   header updates), confirms the GREEN fix (incremental rebuild triggered via
   `depend_files` and `depfile`), statically asserts that all 44 device targets in
   `core/src/meson.build` declare these dependencies, and validates live Ninja
   manifest rules without executing kernels on a GPU.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Rely only on compiler depfiles (`depfile`) | Dynamic discovery of all included headers | Fails on Windows where `nvcc` depfiles are absent; empty until first build; Ninja fails if compiler doesn't emit file | Incomplete platform support and fragile on Windows MSVC hosts |
| Rely only on explicit `depend_files` lists | Fully declarative; works across all compilers and OSes | Requires manual listing; does not automatically catch new deep transitive includes | Used in combination with depfiles rather than exclusively |
| Per-kernel manual dependency mapping in Meson | Narrowest possible rebuild scope per target | High maintenance burden across 44 targets; high risk of drift when kernels add includes | Shared device headers change infrequently; grouping into backend shared sets is robust and maintainable |
| Hybrid: `depend_files` + compiler `depfile` (Selected) | Combines declarative baseline across all platforms with dynamic transitive discovery | Slightly larger set of targets rebuild when a shared header changes | Chosen: robust, durable, cross-platform, and eliminates silent stale-device-binary bugs |

## Consequences

- **Positive**: Editing any shared device header (including struct layout changes)
  triggers an immediate incremental Ninja rebuild of the affected device targets
  and relinks `libvmaf.so.3.0.0`.
- **Positive**: Manual `touch` workarounds before rebuilding are eliminated.
- **Positive**: Windows builds avoid invalid Ninja depfile rules while retaining
  dependency tracking through `depend_files`.
- **Positive**: Deterministic regression contracts run in CI fast suites without
  requiring GPU hardware or kernel execution.
- **Negative / Neutral**: Touching a shared header such as `integer_adm_cuda.h`
  triggers recompilation of the device targets that list the shared header set.
  Compilation time for 22 targets is brief (< 5s) and occurs only when device
  headers are modified.

## References

- [Research-2106](../research/2106-cuda-hip-kernel-header-dependency-tracking.md)
- [`T-CUDA-FATBIN-NO-HEADER-DEP-2026-09-05`](../state.md)
- [`docs/rebase-notes.md`](../rebase-notes.md)
- Source: `core/src/meson.build`, `core/test/test_device_target_header_dependencies.py`
