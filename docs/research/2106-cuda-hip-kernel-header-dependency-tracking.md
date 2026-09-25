<!-- markdownlint-disable MD013 MD060 -->
# Research-2106: CUDA and HIP device target header dependency tracking — 2026-09-25

**Status:** Complete

**Workstream:** [ADR-1320](../adr/1320-cuda-hip-kernel-header-dependency-tracking.md)

**Scope:** Meson build dependency tracking across 22 CUDA fatbin (`cu_ptx_target_*`)
and 22 HIP HSACO (`hip_hsaco_*`) custom targets in `core/src/meson.build`.
No modification to Netflix golden data or model files.

## Problem statement

In `core/src/meson.build`, device code generation for CUDA fatbins and HIP HSACO
binaries declared only the main `.cu` or `.hip` file as input:

```meson
custom_target('cu_ptx_target_' + name,
    output : ['@0@.fatbin'.format(name)],
    input : _cu,
    ...
)
```

Because neither `depend_files` nor dynamic compiler depfiles were wired to Ninja,
editing any header file included by device kernels (such as
`core/src/feature/cuda/integer_adm_cuda.h` or `core/src/feature/hip/integer_adm_hip.h`)
did not invalidate the custom targets. Incremental builds (`ninja -C build`)
reported `ninja: no work to do.`, leaving stale device binaries linked into
`libvmaf.so`.

In Integer ADM, `AdmFixedParametersCuda` is passed by value into device kernels.
When a host struct layout was edited, the host code passed the updated struct layout
while the kernel read parameters at outdated offsets, resulting in silent numeric
drift (~0.2 on ADM scores) with zero build errors or warnings. Developers were forced
to manually `touch` all kernel files before rebuilding.

## Toolchain analysis & compiler depfiles

We audited the behavior of `nvcc` (CUDA 13.4) and `hipcc` (ROCm 7.2):

1. **NVIDIA `nvcc`**:
   - `nvcc --help` documents `-MD -MF <file> -MT <target>`.
   - On Linux/POSIX, nvcc invokes the host preprocessor with `-MD -MF` to write a
     Makefile-compatible depfile containing all transitively included headers.
   - On Windows, nvcc delegates preprocessing through `cl.exe`. Under MSVC syntax,
     `-MD` selects the multithreaded dynamic runtime DLL rather than emitting a
     Makefile depfile. If Meson unconditionally passes `depfile: ...` when
     `nvcc_dep_flags` is empty, Ninja emits a rule expecting a `.d` file that is
     never created.
   - Therefore, on Windows `cu_depfile` must evaluate to `''` (which Meson cleanly
     omits from the Ninja build statement), while POSIX uses `@0@.fatbin.d`.

2. **AMD `hipcc`**:
   - Under `--genco`, `hipcc` acts as an `amdclang++` driver script that ignores
     top-level `-MD`/`-MF`.
   - Direct clang frontend options (`-Xclang -dependency-file -Xclang @DEPFILE@ -Xclang -MT -Xclang @OUTPUT@`)
     successfully instruct the device compilation frontend to write a depfile.

3. **Meson `depend_files`**:
   - Meson's `custom_target` supports `depend_files: files(...)` alongside `depfile`.
   - `depend_files` adds explicit order-independent dependencies (`| dep1 dep2 ...`)
     directly into the Ninja manifest rule.
   - This provides declarative, hermetic dependency tracking across all operating
     systems (including Windows) from the very first build, independent of compiler
     flags.

## Reconciled inventory

All 44 device targets (22 CUDA, 22 HIP) were audited for included headers:

- `cuda_cu_sources`: 22 targets (`adm_cm`, `adm_csf`, `adm_csf_den`, `adm_dwt2`,
  `cambi_score`, `ciede_score`, `filter1d`, `float_adm_score`, `float_motion_score`,
  `float_psnr_score`, `float_vif_score`, `integer_ssim_score`, `moment_score`,
  `motion_score`, `motion_v2_score`, `ms_ssim_score`, `psnr_hvs_score`,
  `psnr_score`, `speed_score`, `ssim_score`, `ssimulacra2_blur`, `ssimulacra2_mul`).
- `hip_kernel_sources`: 22 targets mirroring the CUDA extractors.

We declared `cuda_kernel_shared_headers` and `hip_kernel_shared_headers` encompassing:

- Common headers: `cuda/common.h`, `cuda/cuda_helper.cuh`, `hip/common.h`.
- Feature headers: `integer_adm_cuda.h`, `adm_decouple_inline.cuh`, `integer_vif_cuda.h`,
  `vif_statistics.cuh`, `integer_motion_cuda.h`, `integer_motion_v2_cuda.h`,
  `integer_psnr_cuda.h`, `integer_moment_cuda.h`, `integer_ciede_cuda.h`,
  `integer_ssim_cuda.h`, `ssim_cuda.h`, `integer_ms_ssim_cuda.h`,
  `integer_psnr_hvs_cuda.h`, `float_psnr_cuda.h`, `float_motion_cuda.h`,
  `float_vif_cuda.h`, `ssimulacra2_cuda.h`, `float_adm_cuda.h`,
  `integer_cambi_cuda.h`, `speed_chroma_cuda.h`, `speed_temporal_cuda.h`,
  `adm_angle_flag.h`, and their HIP counterparts.

Every target wires `depend_files` pointing to its backend's shared header set.

## Verification & regression shape

`core/test/test_device_target_header_dependencies.py` implements:

1. **RED test**: Uses a synthetic Ninja environment without header tracking; touching
   the header leaves the output unchanged (`ninja: no work to do.`), reproducing the
   original bug.
2. **GREEN test**: Demonstrates that with `depend_files` and `depfile`, touching the
   header triggers an incremental rebuild.
3. **Static Meson verification**: Confirms all 44 device targets in `core/src/meson.build`
   declare `depend_files` and conditional `depfile` logic, and verifies all listed
   headers exist on disk.
4. **Live Ninja manifest verification**: Checks `build/build.ninja` to verify that
   all 22 `.fatbin` and 22 `.hsaco` rules carry the header dependencies.
5. **Live incremental rebuild dry-run**: Tests that touching `integer_adm_cuda.h` or
   `integer_adm_hip.h` triggers rebuild planning in Ninja without running kernels
   on a GPU.
