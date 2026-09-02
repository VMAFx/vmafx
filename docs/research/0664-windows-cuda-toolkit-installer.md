# Research-0664: Windows CUDA toolkit installer failure

- **Status**: Active
- **Workstream**: ADR-0664
- **Last updated**: 2026-05-21

## Question

Why did the required `Build — Windows MSVC + CUDA (build only)` CI leg fail on
PR #1463 before build output, and what is the smallest fix that preserves the
required MSVC CUDA compilation gate?

## Evidence

- GitHub Actions job `77141384870` failed in the `Install CUDA toolkit
  (windows)` step. The job never reached nv-codec-headers, Meson configure, or
  Ninja.
- The log shows `Jimver/cuda-toolkit@3d45d157...` with CUDA `13.2.0`, method
  `network`, and Windows sub-packages `nvcc`, `cudart`, `crt`, `nvvm`, and
  `visual_studio_integration`.
- The same workflow's Linux CUDA and CUDA static legs completed successfully
  with CUDA 13.2.0, so this is not evidence of a project-wide CUDA-version
  rollback requirement.
- The pinned action source maps CUDA 13.2.0 to NVIDIA's current Windows network
  installer URL:
  `https://developer.download.nvidia.com/compute/cuda/13.2.0/network_installers/cuda_13.2.0_windows_network.exe`.
  A HEAD request on 2026-05-21 returned HTTP 200.
- The Windows network installer accepts silent package arguments with the
  `_<major>.<minor>` suffix. That is exactly what the wrapper would generate
  for `nvcc_13.2`, `cudart_13.2`, `crt_13.2`, `nvvm_13.2`, and
  `visual_studio_integration_13.2`.

## Finding

The project code did not fail. The Windows CUDA setup wrapper failed before it
could install the toolkit or expose a compiler log. Replacing only the Windows
CUDA setup step with a direct NVIDIA network-installer invocation keeps the
same toolchain version, same sub-package set, and same required build gate while
removing the failing wrapper layer.

## Validation Plan

- `bash -n` is not applicable to the PowerShell workflow body.
- Run `git diff --check`.
- Run `.venv/bin/mkdocs build --strict` after ADR/docs updates.
- Let the active PR exercise `Build — Windows MSVC + CUDA (build only)` because
  the failure is Windows-runner specific.
