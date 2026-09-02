<!-- markdownlint-disable MD013 MD060 -->
# ADR-0664: Install Windows CUDA directly in CI

- **Status**: Accepted
- **Date**: 2026-05-21
- **Deciders**: Lusoris maintainers
- **Tags**: ci, build, cuda, windows, github-actions

## Context

`Build — Windows MSVC + CUDA (build only)` is a required PR gate because it is the
only CI leg that compiles the CUDA backend through the MSVC toolchain. On
2026-05-21, PR #1463 failed before Meson configuration: the
`Jimver/cuda-toolkit@v0.2.35` Windows step exited inside the action after printing
only its input block, so there was no compiler, Meson, or NVIDIA installer failure
to fix in libvmaf. The job carried GitHub's `windows-2025` runner-image notice
for the pending Visual Studio 2026 transition window, but the failing step was
the CUDA wrapper itself, not libvmaf compilation.

The Linux CUDA legs still install CUDA 13.2.0 successfully through the same action.
The Windows leg already uses NVIDIA's Windows network installer semantics through
that wrapper, so the smallest durable fix is to perform the Windows network install
directly in the workflow and keep the CUDA version and sub-package list unchanged.

## Decision

We will install CUDA 13.2.0 in the Windows MSVC + CUDA CI leg with a first-party
PowerShell step that downloads NVIDIA's `cuda_13.2.0_windows_network.exe`, installs
only `nvcc`, `cudart`, `crt`, `nvvm`, and `visual_studio_integration`, exports
`CUDA_PATH`, and verifies `nvcc.exe --version`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `Jimver/cuda-toolkit` for Windows and rerun | No workflow churn; keeps Linux and Windows installer wrappers symmetric | The failure happened inside the wrapper before setup and provided no actionable diagnostics; reruns leave master blocked by a third-party action path | Rejected; the required gate needs deterministic setup output |
| Pin Windows CUDA back to 13.0.0 | Known historical path from ADR-0121 | Diverges from ADR-0603, which moved the fork to CUDA 13.2.0 for current toolchain compatibility; would not address the wrapper failure mode | Rejected; version rollback is a workaround, not a setup fix |
| Make the Windows CUDA leg advisory | Avoids blocking unrelated PRs when Windows setup breaks | Removes the only required MSVC CUDA compilation gate and contradicts ADR-0121 | Rejected; the gate is intentionally required |

## Consequences

- **Positive**: The required Windows CUDA gate no longer depends on the broken
  Windows path in the third-party installer action and now emits direct download,
  installer exit-code, `CUDA_PATH`, and `nvcc` verification evidence.
- **Negative**: The Windows leg no longer shares the same wrapper as the Linux CUDA
  legs, so future CUDA-version bumps must update the Windows URL and package
  suffixes explicitly.
- **Neutral / follow-ups**: Keep Linux CUDA legs on `Jimver/cuda-toolkit` while
  they remain green; re-evaluate if that action publishes a Windows fix or if
  NVIDIA changes the network installer package names.

## References

- [ADR-0121](0121-windows-gpu-build-only-legs.md) — Windows GPU build-only gates.
- [ADR-0603](0603-ubuntu-26-04-fallout-fixes.md) — CUDA 13.2.0 pin context.
- [GitHub job 77141384870](https://github.com/VMAFx/vmafx/actions/runs/26216865470/job/77141384870)
  — failed `Jimver/cuda-toolkit` Windows setup on PR #1463.
- Source: req (user: "why did you stop? O.o you dont ask or say anything, do something for hours and still stop??")
